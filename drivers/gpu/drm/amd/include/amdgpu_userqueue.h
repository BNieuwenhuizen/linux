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

#ifndef AMDGPU_USERQUEUE_H_
#define AMDGPU_USERQUEUE_H_

#include "amdgpu.h"
#define AMDGPU_MAX_USERQ 512

struct amdgpu_userq_ctx_space {
	struct amdgpu_bo *obj;
	uint64_t gpu_addr;
	void *cpu_ptr;
};

struct amdgpu_usermode_queue {
	int queue_id;
	int queue_type;
	uint64_t flags;
	uint64_t doorbell_handle;
	uint64_t wptr_mc_addr;
	uint64_t proc_ctx_gpu_addr;
	uint64_t gang_ctx_gpu_addr;
	uint64_t gds_ctx_gpu_addr;
	uint64_t fw_ctx_gpu_addr;
	uint64_t shadow_ctx_gpu_addr;

	struct amdgpu_vm *vm;
	struct amdgpu_userq_mgr *userq_mgr;
	struct amdgpu_mqd_prop userq_prop;
	struct amdgpu_userq_ctx_space mqd;
	struct amdgpu_userq_ctx_space fw_space;
};

struct amdgpu_userq_funcs {
	int (*mqd_create)(struct amdgpu_userq_mgr *, struct amdgpu_usermode_queue *);
	void (*mqd_destroy)(struct amdgpu_userq_mgr *, struct amdgpu_usermode_queue *);
};

int amdgpu_userq_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);

int amdgpu_userq_mgr_init(struct amdgpu_userq_mgr *userq_mgr, struct amdgpu_device *adev);

void amdgpu_userq_mgr_fini(struct amdgpu_userq_mgr *userq_mgr);

#endif
