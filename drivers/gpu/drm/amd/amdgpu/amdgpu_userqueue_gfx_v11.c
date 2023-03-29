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
    r = amdgpu_bo_reserve(mqd->obj, false);
    if (unlikely(r != 0)) {
        DRM_ERROR("Failed to reserve mqd for userqueue (%d)", r);
        goto free_mqd;
    }

    queue->userq_prop.use_doorbell = true;
    queue->userq_prop.mqd_gpu_addr = mqd->gpu_addr;
    r = gfx_v11_mqd->init_mqd(adev, (void *)mqd->cpu_ptr, &queue->userq_prop);
    amdgpu_bo_unreserve(mqd->obj);
    if (r) {
        DRM_ERROR("Failed to init MQD for queue\n");
        goto free_mqd;
    }

    DRM_DEBUG_DRIVER("MQD for queue %d created\n", queue->queue_id);
    return 0;

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

    amdgpu_bo_free_kernel(&mqd->obj,
			   &mqd->gpu_addr,
			   &mqd->cpu_ptr);
}

const struct amdgpu_userq_funcs userq_gfx_v11_funcs = {
    .mqd_create = amdgpu_userq_gfx_v11_mqd_create,
    .mqd_destroy = amdgpu_userq_gfx_v11_mqd_destroy,
};
