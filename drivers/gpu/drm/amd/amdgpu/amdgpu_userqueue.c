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

static uint64_t
amdgpu_userqueue_get_doorbell_index(struct amdgpu_userq_mgr *uq_mgr,
                                    struct amdgpu_usermode_queue *queue,
                                    struct drm_file *filp,
                                    uint32_t doorbell_index)
{
    struct drm_gem_object *gobj;
    struct amdgpu_bo *db_bo;
    uint64_t index;

    gobj = drm_gem_object_lookup(filp, queue->doorbell_handle);
    if (gobj == NULL) {
        DRM_ERROR("Can't find GEM object for doorbell\n");
        return -EINVAL;
    }

    db_bo = amdgpu_bo_ref(gem_to_amdgpu_bo(gobj));
    drm_gem_object_put(gobj);

    index = amdgpu_doorbell_index_on_bar(uq_mgr->adev, db_bo, doorbell_index);

    DRM_DEBUG_DRIVER("[Usermode queues] doorbell index=%lld\n", index);

    return index;
}

static int
amdgpu_userqueue_map_gtt_bo_to_gart(struct amdgpu_device *adev, struct amdgpu_bo *bo)
{
    int ret;

    ret = amdgpu_bo_reserve(bo, true);
    if (ret) {
        DRM_ERROR("Failed to reserve bo. ret %d\n", ret);
        goto err_reserve_bo_failed;
    }

    ret = amdgpu_bo_pin(bo, AMDGPU_GEM_DOMAIN_GTT);
    if (ret) {
        DRM_ERROR("Failed to pin bo. ret %d\n", ret);
        goto err_pin_bo_failed;
    }

    ret = amdgpu_ttm_alloc_gart(&bo->tbo);
    if (ret) {
        DRM_ERROR("Failed to bind bo to GART. ret %d\n", ret);
        goto err_map_bo_gart_failed;
    }


    amdgpu_bo_unreserve(bo);
    bo = amdgpu_bo_ref(bo);

    return 0;

err_map_bo_gart_failed:
    amdgpu_bo_unpin(bo);
err_pin_bo_failed:
    amdgpu_bo_unreserve(bo);
err_reserve_bo_failed:

    return ret;
}


static int
amdgpu_userqueue_create_wptr_mapping(struct amdgpu_device *adev,
				     struct drm_file *filp,
				     struct amdgpu_usermode_queue *queue)
{
    struct amdgpu_bo_va_mapping *wptr_mapping;
    struct amdgpu_vm *wptr_vm;
    struct amdgpu_bo *wptr_bo = NULL;
    uint64_t wptr = queue->userq_prop.wptr_gpu_addr;
    int ret;

    wptr_vm = queue->vm;
    ret = amdgpu_bo_reserve(wptr_vm->root.bo, false);
    if (ret)
        goto err_wptr_map_gart;

    wptr_mapping = amdgpu_vm_bo_lookup_mapping(wptr_vm, wptr >> PAGE_SHIFT);
    amdgpu_bo_unreserve(wptr_vm->root.bo);
    if (!wptr_mapping) {
        DRM_ERROR("Failed to lookup wptr bo\n");
        ret = -EINVAL;
        goto err_wptr_map_gart;
    }

    wptr_bo = wptr_mapping->bo_va->base.bo;
    if (wptr_bo->tbo.base.size > PAGE_SIZE) {
        DRM_ERROR("Requested GART mapping for wptr bo larger than one page\n");
        ret = -EINVAL;
        goto err_wptr_map_gart;
    }

    ret = amdgpu_userqueue_map_gtt_bo_to_gart(adev, wptr_bo);
    if (ret) {
        DRM_ERROR("Failed to map wptr bo to GART\n");
        goto err_wptr_map_gart;
    }

    queue->wptr_mc_addr = wptr_bo->tbo.resource->start << PAGE_SHIFT;
    return 0;

err_wptr_map_gart:
    return ret;
}

static int amdgpu_userqueue_create(struct drm_file *filp, union drm_amdgpu_userq *args)
{
    struct amdgpu_usermode_queue *queue;
    struct amdgpu_fpriv *fpriv = filp->driver_priv;
    struct amdgpu_userq_mgr *uq_mgr = &fpriv->userq_mgr;
    struct drm_amdgpu_userq_mqd *mqd_in = &args->in.mqd;
    uint64_t index;
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
    index = amdgpu_userqueue_get_doorbell_index(uq_mgr, queue, filp, mqd_in->doorbell_offset);
    if (index == (uint64_t)-EINVAL) {
        DRM_ERROR("Invalid doorbell object\n");
        r = -EINVAL;
        goto free_queue;
    }

    queue->userq_prop.doorbell_index = index;
    queue->shadow_ctx_gpu_addr = mqd_in->shadow_va;
    queue->queue_type = mqd_in->ip_type;
    queue->flags = mqd_in->flags;
    queue->vm = &fpriv->vm;
    queue->queue_id = amdgpu_userqueue_index(uq_mgr, queue);
    if (queue->queue_id < 0) {
        DRM_ERROR("Failed to allocate a queue id\n");
        r = queue->queue_id;
        goto free_queue;
    }

    r = amdgpu_userqueue_create_wptr_mapping(uq_mgr->adev, filp, queue);
    if (r) {
        DRM_ERROR("Failed to map WPTR (0x%llx) for userqueue\n", queue->userq_prop.wptr_gpu_addr);
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

static int amdgpu_userqueue_release(int queue_id, void *ptr, void *data)
{
    struct amdgpu_userq_mgr *uq_mgr = data;
    struct amdgpu_usermode_queue *queue = ptr;

    mutex_lock(&uq_mgr->userq_mutex);
    uq_mgr->userq_funcs[queue->queue_type]->mqd_destroy(uq_mgr, queue);
    amdgpu_userqueue_free_index(uq_mgr, queue->queue_id);
    mutex_unlock(&uq_mgr->userq_mutex);
    kfree(queue);
    return 0;
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

    amdgpu_userqueue_release(queue_id, queue, uq_mgr);
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
    idr_for_each(&userq_mgr->userq_idr,
                 &amdgpu_userqueue_release, userq_mgr);
    idr_destroy(&userq_mgr->userq_idr);
    mutex_destroy(&userq_mgr->userq_mutex);
}
