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
#include "amdgpu_vm.h"
#include "amdgpu_userqueue.h"

static inline int
amdgpu_userqueue_index(struct amdgpu_userq_mgr *uq_mgr, struct amdgpu_usermode_queue *queue)
{
    return idr_alloc(&uq_mgr->userq_idr, queue, 1, AMDGPU_MAX_USERQ, GFP_KERNEL);
}

static inline void
amdgpu_userqueue_free_index(struct amdgpu_userq_mgr *uq_mgr, int queue_id)
{
    idr_remove(&uq_mgr->userq_idr, queue_id);
}

static struct amdgpu_usermode_queue *
amdgpu_userqueue_find(struct amdgpu_userq_mgr *uq_mgr, int qid)
{
    return idr_find(&uq_mgr->userq_idr, qid);
}

static int amdgpu_userqueue_create(struct drm_file *filp, union drm_amdgpu_userq *args)
{
    struct amdgpu_usermode_queue *queue;
    struct amdgpu_fpriv *fpriv = filp->driver_priv;
    struct amdgpu_userq_mgr *uq_mgr = &fpriv->userq_mgr;
    struct drm_amdgpu_userq_mqd *mqd_in = &args->in.mqd;
    int r;

    /* Do we have support userqueues for this IP ? */
    if (!uq_mgr->userq_funcs[mqd_in->ip_type]) {
        DRM_ERROR("GFX User queues not supported for this IP: %d\n", mqd_in->ip_type);
        return -EINVAL;
    }

    queue = kzalloc(sizeof(struct amdgpu_usermode_queue), GFP_KERNEL);
    if (!queue) {
        DRM_ERROR("Failed to allocate memory for queue\n");
        return -ENOMEM;
    }

    mutex_lock(&uq_mgr->userq_mutex);
    queue->userq_prop.wptr_gpu_addr = mqd_in->wptr_va;
    queue->userq_prop.rptr_gpu_addr = mqd_in->rptr_va;
    queue->userq_prop.queue_size = mqd_in->queue_size;
    queue->userq_prop.hqd_base_gpu_addr = mqd_in->queue_va;
    queue->userq_prop.queue_size = mqd_in->queue_size;

    queue->doorbell_handle = mqd_in->doorbell_handle;
    queue->queue_type = mqd_in->ip_type;
    queue->flags = mqd_in->flags;
    queue->vm = &fpriv->vm;
    queue->queue_id = amdgpu_userqueue_index(uq_mgr, queue);
    if (queue->queue_id < 0) {
        DRM_ERROR("Failed to allocate a queue id\n");
        r = queue->queue_id;
        goto free_queue;
    }

    r = uq_mgr->userq_funcs[queue->queue_type]->mqd_create(uq_mgr, queue);
    if (r) {
        DRM_ERROR("Failed to create/map userqueue MQD\n");
        goto free_queue;
    }

    args->out.queue_id = queue->queue_id;
    args->out.flags = 0;
    mutex_unlock(&uq_mgr->userq_mutex);
    return 0;

free_queue:
    mutex_unlock(&uq_mgr->userq_mutex);
    kfree(queue);
    return r;
}

static void amdgpu_userqueue_destroy(struct drm_file *filp, int queue_id)
{
    struct amdgpu_fpriv *fpriv = filp->driver_priv;
    struct amdgpu_userq_mgr *uq_mgr = &fpriv->userq_mgr;
    struct amdgpu_usermode_queue *queue;

    queue = amdgpu_userqueue_find(uq_mgr, queue_id);
    if (!queue) {
        DRM_DEBUG_DRIVER("Invalid queue id to destroy\n");
        return;
    }

    mutex_lock(&uq_mgr->userq_mutex);
    uq_mgr->userq_funcs[queue->queue_type]->mqd_destroy(uq_mgr, queue);
    amdgpu_userqueue_free_index(uq_mgr, queue->queue_id);
    mutex_unlock(&uq_mgr->userq_mutex);
    kfree(queue);
}

int amdgpu_userq_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *filp)
{
    union drm_amdgpu_userq *args = data;
    int r = 0;

    switch (args->in.op) {
    case AMDGPU_USERQ_OP_CREATE:
        r = amdgpu_userqueue_create(filp, args);
        if (r)
            DRM_ERROR("Failed to create usermode queue\n");
        break;

    case AMDGPU_USERQ_OP_FREE:
        amdgpu_userqueue_destroy(filp, args->in.queue_id);
        break;

    default:
        DRM_ERROR("Invalid user queue op specified: %d\n", args->in.op);
        return -EINVAL;
    }

    return r;
}

extern const struct amdgpu_userq_funcs userq_gfx_v11_funcs;

static void
amdgpu_userqueue_setup_ip_funcs(struct amdgpu_userq_mgr *uq_mgr)
{
    int maj;
    struct amdgpu_device *adev = uq_mgr->adev;
    uint32_t version = adev->ip_versions[GC_HWIP][0];

    maj = IP_VERSION_MAJ(version);
    if (maj == 11)
        uq_mgr->userq_funcs[AMDGPU_HW_IP_GFX] = &userq_gfx_v11_funcs;
}

int amdgpu_userq_mgr_init(struct amdgpu_userq_mgr *userq_mgr, struct amdgpu_device *adev)
{
    mutex_init(&userq_mgr->userq_mutex);
    idr_init_base(&userq_mgr->userq_idr, 1);
    userq_mgr->adev = adev;

    amdgpu_userqueue_setup_ip_funcs(userq_mgr);
    return 0;
}

void amdgpu_userq_mgr_fini(struct amdgpu_userq_mgr *userq_mgr)
{
    idr_destroy(&userq_mgr->userq_idr);
    mutex_destroy(&userq_mgr->userq_mutex);
}
