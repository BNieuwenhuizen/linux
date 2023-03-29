/*
 * Copyright 2023 Advanced Micro Devices, Inc.
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
#include "kfd_priv.h"

static inline
bool amdgpu_doorbell_valid(struct amdgpu_device *adev, u32 index)
{
	if (index >= adev->doorbell.kernel_doorbells.start &&
	    index < adev->doorbell.kernel_doorbells.end)
		return true;

	if (index >= adev->mes.kernel_doorbells.start &&
	    index < adev->mes.kernel_doorbells.end)
		return true;

	if (index >= adev->kfd.dev->kernel_doorbells.start &&
	    index < adev->kfd.dev->kernel_doorbells.end)
		return true;

	return false;
}

/**
 * amdgpu_mm_rdoorbell - read a doorbell dword
 *
 * @adev: amdgpu_device pointer
 * @index: doorbell index
 *
 * Returns the value in the doorbell aperture at the
 * requested doorbell index (CIK).
 */
u32 amdgpu_mm_rdoorbell(struct amdgpu_device *adev, u32 index)
{
	if (amdgpu_device_skip_hw_access(adev))
		return 0;

	if (amdgpu_doorbell_valid(adev, index)) {
		return readl(adev->doorbell.ptr + index);
	} else {
		DRM_ERROR("reading beyond doorbell aperture: 0x%08x!\n", index);
		return 0;
	}
}

/**
 * amdgpu_mm_wdoorbell - write a doorbell dword
 *
 * @adev: amdgpu_device pointer
 * @index: doorbell index
 * @v: value to write
 *
 * Writes @v to the doorbell aperture at the
 * requested doorbell index (CIK).
 */
void amdgpu_mm_wdoorbell(struct amdgpu_device *adev, u32 index, u32 v)
{
	if (amdgpu_device_skip_hw_access(adev))
		return;

	if (amdgpu_doorbell_valid(adev, index)) {
		writel(v, adev->doorbell.ptr + index);
	} else {
		DRM_ERROR("writing beyond doorbell aperture: 0x%08x!\n", index);
	}
}

/**
 * amdgpu_mm_rdoorbell64 - read a doorbell Qword
 *
 * @adev: amdgpu_device pointer
 * @index: doorbell index
 *
 * Returns the value in the doorbell aperture at the
 * requested doorbell index (VEGA10+).
 */
u64 amdgpu_mm_rdoorbell64(struct amdgpu_device *adev, u32 index)
{
	if (amdgpu_device_skip_hw_access(adev))
		return 0;

	if (amdgpu_doorbell_valid(adev, index)) {
		return atomic64_read((atomic64_t *)(adev->doorbell.ptr + index));
	} else {
		DRM_ERROR("reading beyond doorbell aperture: 0x%08x!\n", index);
		return 0;
	}
}

/**
 * amdgpu_mm_wdoorbell64 - write a doorbell Qword
 *
 * @adev: amdgpu_device pointer
 * @index: doorbell index
 * @v: value to write
 *
 * Writes @v to the doorbell aperture at the
 * requested doorbell index (VEGA10+).
 */
void amdgpu_mm_wdoorbell64(struct amdgpu_device *adev, u32 index, u64 v)
{
	if (amdgpu_device_skip_hw_access(adev))
		return;

	if (amdgpu_doorbell_valid(adev, index)) {
		atomic64_set((atomic64_t *)(adev->doorbell.ptr + index), v);
	} else {
		DRM_ERROR("writing beyond doorbell aperture: 0x%08x!\n", index);
	}
}

/**
 * amdgpu_doorbell_index_on_bar - Find doorbell's absolute offset in BAR
 *
 * @adev: amdgpu_device pointer
 *
 * @db_bo: doorbell object's bo
 *
 * @db_index: doorbell relative index in this doorbell object
 *
 * returns doorbell's absolute index in BAR
 */
uint32_t amdgpu_doorbell_index_on_bar(struct amdgpu_device *adev,
				       struct amdgpu_bo *db_bo,
				       uint32_t doorbell_index)
{
	int db_bo_offset;

	db_bo_offset = amdgpu_bo_gpu_offset_no_check(db_bo);

	/*
	 * doorbell index granularity is maintained at 32 bit
	 * but doorbell's size is 64-bit, so index * 2
	 */
	return db_bo_offset / sizeof(u32) + doorbell_index * 2;
}

/**
 * amdgpu_doorbell_free_page - Free a doorbell page
 *
 * @adev: amdgpu_device pointer
 *
 * @db_age: previously allocated doobell page details
 *
 */
void amdgpu_doorbell_free_page(struct amdgpu_device *adev,
					struct amdgpu_doorbell_obj *db_obj)
{
	amdgpu_bo_free_kernel(&db_obj->bo,
			      &db_obj->gpu_addr,
			      (void **)&db_obj->cpu_addr);

}

/**
 * amdgpu_doorbell_alloc_page - create a page from doorbell pool
 *
 * @adev: amdgpu_device pointer
 *
 * @db_age: doobell page structure to fill details with
 *
 * returns 0 on success, else error number
 */
int amdgpu_doorbell_alloc_page(struct amdgpu_device *adev,
				struct amdgpu_doorbell_obj *db_obj)
{
	int r;

	db_obj->size = ALIGN(db_obj->size, PAGE_SIZE);

	r = amdgpu_bo_create_kernel(adev,
				    db_obj->size,
				    PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_DOORBELL,
				    &db_obj->bo,
				    &db_obj->gpu_addr,
				    (void **)&db_obj->cpu_addr);

	if (r) {
		DRM_ERROR("Failed to create doorbell BO, err=%d\n", r);
		return r;
	}

	db_obj->start = amdgpu_doorbell_index_on_bar(adev, db_obj->bo, 0);
	db_obj->end = db_obj->start + db_obj->size / sizeof(u32);
	return 0;
}

/**
 * amdgpu_doorbell_create_kernel_doorbells - Create kernel doorbells for graphics
 *
 * @adev: amdgpu_device pointer
 *
 * Creates doorbells for graphics driver
 *
 * returns 0 on success, error otherwise.
 */
int amdgpu_doorbell_create_kernel_doorbells(struct amdgpu_device *adev)
{
	int r;
	struct amdgpu_doorbell_obj *kernel_doorbells = &adev->doorbell.kernel_doorbells;

	kernel_doorbells->doorbell_bitmap = bitmap_zalloc(adev->doorbell.num_kernel_doorbells,
							  GFP_KERNEL);
	if (!kernel_doorbells->doorbell_bitmap) {
		DRM_ERROR("Failed to create kernel doorbell bitmap\n");
		return -ENOMEM;
	}

	kernel_doorbells->size = adev->doorbell.num_kernel_doorbells * sizeof(u32);
	r = amdgpu_doorbell_alloc_page(adev, kernel_doorbells);
	if (r) {
		bitmap_free(kernel_doorbells->doorbell_bitmap);
		DRM_ERROR("Failed to allocate kernel doorbells, err=%d\n", r);
		return r;
	}

	return 0;
}

/*
 * GPU doorbell aperture helpers function.
 */
/**
 * amdgpu_device_doorbell_init - Init doorbell driver information.
 *
 * @adev: amdgpu_device pointer
 *
 * Init doorbell driver information (CIK)
 * Returns 0 on success, error on failure.
 */
int amdgpu_device_doorbell_init(struct amdgpu_device *adev)
{

	/* No doorbell on SI hardware generation */
	if (adev->asic_type < CHIP_BONAIRE) {
		adev->doorbell.base = 0;
		adev->doorbell.size = 0;
		adev->doorbell.num_kernel_doorbells = 0;
		return 0;
	}

	if (pci_resource_flags(adev->pdev, 2) & IORESOURCE_UNSET)
		return -EINVAL;

	amdgpu_asic_init_doorbell_index(adev);

	/* doorbell bar mapping */
	adev->doorbell.base = pci_resource_start(adev->pdev, 2);
	adev->doorbell.size = pci_resource_len(adev->pdev, 2);

	adev->doorbell.num_kernel_doorbells =
		min_t(u32, adev->doorbell.size / sizeof(u32),
				adev->doorbell_index.max_assignment+1);
	if (adev->doorbell.num_kernel_doorbells == 0)
		return -EINVAL;

	/*
	 * For Vega, reserve and map two pages on doorbell BAR since SDMA
	 * paging queue doorbell use the second page. The
	 * AMDGPU_DOORBELL64_MAX_ASSIGNMENT definition assumes all the
	 * doorbells are in the first page. So with paging queue enabled,
	 * the max num_kernel_doorbells should + 1 page (0x400 in dword)
	 */
	if (adev->asic_type >= CHIP_VEGA10)
		adev->doorbell.num_kernel_doorbells += 0x400;

	adev->doorbell.ptr = ioremap(adev->doorbell.base, adev->doorbell.size);
	return 0;
}

/**
 * amdgpu_device_doorbell_fini - Tear down doorbell driver information.
 *
 * @adev: amdgpu_device pointer
 *
 * Tear down doorbell driver information (CIK)
 */
void amdgpu_device_doorbell_fini(struct amdgpu_device *adev)
{
	bitmap_free(adev->doorbell.kernel_doorbells.doorbell_bitmap);
	amdgpu_doorbell_free_page(adev, &adev->doorbell.kernel_doorbells);
}
