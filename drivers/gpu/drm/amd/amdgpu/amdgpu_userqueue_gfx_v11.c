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

#define AMDGPU_USERQ_PROC_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_GANG_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_FW_CTX_SZ PAGE_SIZE
#define AMDGPU_USERQ_GDS_CTX_SZ PAGE_SIZE

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

    amdgpu_userq_gfx_v11_destroy_ctx_space(uq_mgr, queue);
    amdgpu_bo_free_kernel(&mqd->obj,
			   &mqd->gpu_addr,
			   &mqd->cpu_ptr);
}

const struct amdgpu_userq_funcs userq_gfx_v11_funcs = {
    .mqd_create = amdgpu_userq_gfx_v11_mqd_create,
    .mqd_destroy = amdgpu_userq_gfx_v11_mqd_destroy,
};
