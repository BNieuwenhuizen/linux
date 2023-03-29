/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include "amdgpu.h"
#include "amdgpu_userqueue.h"
#include "v11_structs.h"
#include "amdgpu_mes.h"

#define AMDGPU_USERQ_PROC_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_GANG_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_FW_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_GDS_CTX_SZ PAGE_SIZE

static int
amdgpu_userq_gfx_v11_map(struct amdgpu_userq_mgr *uq_mgr,
                         struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_device *adev = uq_mgr->adev;
    struct mes_add_queue_input queue_input;
    int r;

    memset(&queue_input, 0x0, sizeof(struct mes_add_queue_input));

    queue_input.process_va_start = 0;
    queue_input.process_va_end = (adev->vm_manager.max_pfn - 1) << AMDGPU_GPU_PAGE_SHIFT;
    queue_input.process_quantum = 100000; /* 10ms */
    queue_input.gang_quantum = 10000; /* 1ms */
    queue_input.paging = false;

    queue_input.gang_context_addr = queue->gang_ctx_gpu_addr;
    queue_input.process_context_addr = queue->proc_ctx_gpu_addr;
    queue_input.inprocess_gang_priority = AMDGPU_MES_PRIORITY_LEVEL_NORMAL;
    queue_input.gang_global_priority_level = AMDGPU_MES_PRIORITY_LEVEL_NORMAL;

    queue_input.process_id = queue->vm->pasid;
    queue_input.queue_type = queue->queue_type;
    queue_input.mqd_addr = queue->mqd.gpu_addr;
    queue_input.wptr_addr = queue->userq_prop.wptr_gpu_addr;
    queue_input.queue_size = queue->userq_prop.queue_size >> 2;
    queue_input.doorbell_offset = queue->userq_prop.doorbell_index;
    queue_input.page_table_base_addr = amdgpu_gmc_pd_addr(queue->vm->root.bo);
    queue_input.wptr_mc_addr = queue->wptr_mc_addr;

    amdgpu_mes_lock(&adev->mes);
    r = adev->mes.funcs->add_hw_queue(&adev->mes, &queue_input);
    amdgpu_mes_unlock(&adev->mes);
    if (r) {
        DRM_ERROR("Failed to map queue in HW, err (%d)\n", r);
        return r;
    }

    DRM_DEBUG_DRIVER("Queue %d mapped successfully\n", queue->queue_id);
    return 0;
}

static void
amdgpu_userq_gfx_v11_unmap(struct amdgpu_userq_mgr *uq_mgr,
                           struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_device *adev = uq_mgr->adev;
    struct mes_remove_queue_input queue_input;
    int r;

    memset(&queue_input, 0x0, sizeof(struct mes_remove_queue_input));
    queue_input.doorbell_offset = queue->userq_prop.doorbell_index;
    queue_input.gang_context_addr = queue->gang_ctx_gpu_addr;

    amdgpu_mes_lock(&adev->mes);
    r = adev->mes.funcs->remove_hw_queue(&adev->mes, &queue_input);
    amdgpu_mes_unlock(&adev->mes);
    if (r)
        DRM_ERROR("Failed to unmap queue in HW, err (%d)\n", r);
}

static int amdgpu_userq_gfx_v11_create_ctx_space(struct amdgpu_userq_mgr *uq_mgr,
                                                 struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_device *adev = uq_mgr->adev;
    struct amdgpu_userq_ctx_space *ctx = &queue->fw_space;
    int r, size;

    /*
     * The FW expects atleast one page space allocated for
     * process ctx, gang ctx, gds ctx, fw ctx and shadow ctx each.
     */
    size = AMDGPU_USERQ_PROC_CTX_SZ + AMDGPU_USERQ_GANG_CTX_SZ +
           AMDGPU_USERQ_FW_CTX_SZ + AMDGPU_USERQ_GDS_CTX_SZ;
    r = amdgpu_bo_create_kernel(adev, size, PAGE_SIZE,
                                AMDGPU_GEM_DOMAIN_GTT,
                                &ctx->obj,
                                &ctx->gpu_addr,
                                &ctx->cpu_ptr);
    if (r) {
        DRM_ERROR("Failed to allocate ctx space bo for userqueue, err:%d\n", r);
        return r;
    }

    queue->proc_ctx_gpu_addr = ctx->gpu_addr;
    queue->gang_ctx_gpu_addr = queue->proc_ctx_gpu_addr + AMDGPU_USERQ_PROC_CTX_SZ;
    queue->fw_ctx_gpu_addr = queue->gang_ctx_gpu_addr + AMDGPU_USERQ_GANG_CTX_SZ;
    queue->gds_ctx_gpu_addr = queue->fw_ctx_gpu_addr + AMDGPU_USERQ_FW_CTX_SZ;
    return 0;
}

static void amdgpu_userq_gfx_v11_destroy_ctx_space(struct amdgpu_userq_mgr *uq_mgr,
                                                   struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_userq_ctx_space *ctx = &queue->fw_space;

    amdgpu_bo_free_kernel(&ctx->obj,
                          &ctx->gpu_addr,
                          &ctx->cpu_ptr);
}

static void
amdgpu_userq_set_ctx_space(struct amdgpu_userq_mgr *uq_mgr,
                           struct amdgpu_usermode_queue *queue)
{
    struct v11_gfx_mqd *mqd = queue->mqd.cpu_ptr;

    mqd->shadow_base_lo = queue->shadow_ctx_gpu_addr & 0xfffffffc;
    mqd->shadow_base_hi = upper_32_bits(queue->shadow_ctx_gpu_addr);

    mqd->gds_bkup_base_lo = queue->gds_ctx_gpu_addr & 0xfffffffc;
    mqd->gds_bkup_base_hi = upper_32_bits(queue->gds_ctx_gpu_addr);

    mqd->fw_work_area_base_lo = queue->fw_ctx_gpu_addr & 0xfffffffc;
    mqd->fw_work_area_base_lo = upper_32_bits(queue->fw_ctx_gpu_addr);
}

static int
amdgpu_userq_gfx_v11_mqd_create(struct amdgpu_userq_mgr *uq_mgr, struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_device *adev = uq_mgr->adev;
    struct amdgpu_userq_ctx_space *mqd = &queue->mqd;
    struct amdgpu_mqd *gfx_v11_mqd = &adev->mqds[queue->queue_type];
    int size = gfx_v11_mqd->mqd_size;
    int r;

    r = amdgpu_bo_create_kernel(adev, size, PAGE_SIZE,
                                AMDGPU_GEM_DOMAIN_GTT,
                                &mqd->obj,
                                &mqd->gpu_addr,
                                &mqd->cpu_ptr);
    if (r) {
        DRM_ERROR("Failed to allocate bo for userqueue (%d)", r);
        return r;
    }

    memset(mqd->cpu_ptr, 0, size);

    r = amdgpu_userq_gfx_v11_create_ctx_space(uq_mgr, queue);
    if (r) {
        DRM_ERROR("Failed to create CTX space for userqueue (%d)\n", r);
        goto free_mqd;
    }

    r = amdgpu_bo_reserve(mqd->obj, false);
    if (unlikely(r != 0)) {
        DRM_ERROR("Failed to reserve mqd for userqueue (%d)", r);
        goto free_ctx;
    }

    queue->userq_prop.use_doorbell = true;
    queue->userq_prop.mqd_gpu_addr = mqd->gpu_addr;
    r = gfx_v11_mqd->init_mqd(adev, (void *)mqd->cpu_ptr, &queue->userq_prop);
    if (r) {
        amdgpu_bo_unreserve(mqd->obj);
        DRM_ERROR("Failed to init MQD for queue\n");
        goto free_ctx;
    }

    amdgpu_userq_set_ctx_space(uq_mgr, queue);
    amdgpu_bo_unreserve(mqd->obj);

    /* Map the queue in HW using MES ring */
    r = amdgpu_userq_gfx_v11_map(uq_mgr, queue);
    if (r) {
        DRM_ERROR("Failed to map userqueue (%d)\n", r);
        goto free_ctx;
    }

    DRM_DEBUG_DRIVER("MQD for queue %d created\n", queue->queue_id);
    return 0;

free_ctx:
    amdgpu_userq_gfx_v11_destroy_ctx_space(uq_mgr, queue);

free_mqd:
    amdgpu_bo_free_kernel(&mqd->obj,
			   &mqd->gpu_addr,
			   &mqd->cpu_ptr);
   return r;
}

static void
amdgpu_userq_gfx_v11_mqd_destroy(struct amdgpu_userq_mgr *uq_mgr, struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_userq_ctx_space *mqd = &queue->mqd;

    amdgpu_userq_gfx_v11_unmap(uq_mgr, queue);
    amdgpu_userq_gfx_v11_destroy_ctx_space(uq_mgr, queue);
    amdgpu_bo_free_kernel(&mqd->obj,
			   &mqd->gpu_addr,
			   &mqd->cpu_ptr);
}

const struct amdgpu_userq_funcs userq_gfx_v11_funcs = {
    .mqd_create = amdgpu_userq_gfx_v11_mqd_create,
    .mqd_destroy = amdgpu_userq_gfx_v11_mqd_destroy,
};
