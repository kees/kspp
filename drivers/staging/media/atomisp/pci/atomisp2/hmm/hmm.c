/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
/*
 * This file contains entry functions for memory management of ISP driver
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/highmem.h>	/* for kmap */
#include <linux/io.h>		/* for page_to_phys */
#include <linux/sysfs.h>

#include "hmm/hmm.h"
#include "hmm/hmm_pool.h"
#include "hmm/hmm_bo.h"

#include "atomisp_internal.h"
#include "asm/cacheflush.h"
#include "mmu/isp_mmu.h"
#include "mmu/sh_mmu_mrfld.h"

struct hmm_bo_device bo_device;
struct hmm_pool	dynamic_pool;
struct hmm_pool	reserved_pool;
static ia_css_ptr dummy_ptr;
struct _hmm_mem_stat hmm_mem_stat;

const char *hmm_bo_type_strings[HMM_BO_LAST] = {
	"p", /* private */
	"s", /* shared */
	"u", /* user */
#ifdef CONFIG_ION
	"i", /* ion */
#endif
};

static ssize_t bo_show(struct device *dev, struct device_attribute *attr,
			char *buf, struct list_head *bo_list, bool active)
{
	ssize_t ret = 0;
	struct hmm_buffer_object *bo;
	unsigned long flags;
	int i;
	long total[HMM_BO_LAST] = { 0 };
	long count[HMM_BO_LAST] = { 0 };
	int index1 = 0;
	int index2 = 0;

	ret = scnprintf(buf, PAGE_SIZE, "type pgnr\n");
	if (ret <= 0)
		return 0;

	index1 += ret;

	spin_lock_irqsave(&bo_device.list_lock, flags);
	list_for_each_entry(bo, bo_list, list) {
		if ((active && (bo->status & HMM_BO_ALLOCED)) ||
			(!active && !(bo->status & HMM_BO_ALLOCED))) {
			ret = scnprintf(buf + index1, PAGE_SIZE - index1,
				"%s %d\n",
				hmm_bo_type_strings[bo->type], bo->pgnr);

			total[bo->type] += bo->pgnr;
			count[bo->type]++;
			if (ret > 0)
				index1 += ret;
		}
	}
	spin_unlock_irqrestore(&bo_device.list_lock, flags);

	for (i = 0; i < HMM_BO_LAST; i++) {
		if (count[i]) {
			ret = scnprintf(buf + index1 + index2,
				PAGE_SIZE - index1 - index2,
				"%ld %s buffer objects: %ld KB\n",
				count[i], hmm_bo_type_strings[i], total[i] * 4);
			if (ret > 0)
				index2 += ret;
		}
	}

	/* Add trailing zero, not included by scnprintf */
	return index1 + index2 + 1;
}

static ssize_t active_bo_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return bo_show(dev, attr, buf, &bo_device.entire_bo_list, true);
}

static ssize_t free_bo_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return bo_show(dev, attr, buf, &bo_device.entire_bo_list, false);
}

static ssize_t reserved_pool_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	ssize_t ret = 0;

	struct hmm_reserved_pool_info *pinfo = reserved_pool.pool_info;
	unsigned long flags;

	if (!pinfo || !pinfo->initialized)
		return 0;

	spin_lock_irqsave(&pinfo->list_lock, flags);
	ret = scnprintf(buf, PAGE_SIZE, "%d out of %d pages available\n",
					pinfo->index, pinfo->pgnr);
	spin_unlock_irqrestore(&pinfo->list_lock, flags);

	if (ret > 0)
		ret++; /* Add trailing zero, not included by scnprintf */

	return ret;
};

static ssize_t dynamic_pool_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	ssize_t ret = 0;

	struct hmm_dynamic_pool_info *pinfo = dynamic_pool.pool_info;
	unsigned long flags;

	if (!pinfo || !pinfo->initialized)
		return 0;

	spin_lock_irqsave(&pinfo->list_lock, flags);
	ret = scnprintf(buf, PAGE_SIZE, "%d (max %d) pages available\n",
					pinfo->pgnr, pinfo->pool_size);
	spin_unlock_irqrestore(&pinfo->list_lock, flags);

	if (ret > 0)
		ret++; /* Add trailing zero, not included by scnprintf */

	return ret;
};

static DEVICE_ATTR(active_bo, S_IRUGO, active_bo_show, NULL);
static DEVICE_ATTR(free_bo, S_IRUGO, free_bo_show, NULL);
static DEVICE_ATTR(reserved_pool, S_IRUGO, reserved_pool_show, NULL);
static DEVICE_ATTR(dynamic_pool, S_IRUGO, dynamic_pool_show, NULL);

static struct attribute *sysfs_attrs_ctrl[] = {
	&dev_attr_active_bo.attr,
	&dev_attr_free_bo.attr,
	&dev_attr_reserved_pool.attr,
	&dev_attr_dynamic_pool.attr,
	NULL
};

static struct attribute_group atomisp_attribute_group[] = {
	{.attrs = sysfs_attrs_ctrl },
};

int hmm_init(void)
{
	int ret;

	ret = hmm_bo_device_init(&bo_device, &sh_mmu_mrfld,
				 ISP_VM_START, ISP_VM_SIZE);
	if (ret)
		dev_err(atomisp_dev, "hmm_bo_device_init failed.\n");

	/*
	 * As hmm use NULL to indicate invalid ISP virtual address,
	 * and ISP_VM_START is defined to 0 too, so we allocate
	 * one piece of dummy memory, which should return value 0,
	 * at the beginning, to avoid hmm_alloc return 0 in the
	 * further allocation.
	 */
	dummy_ptr = hmm_alloc(1, HMM_BO_PRIVATE, 0, 0, HMM_UNCACHED);

	if (!ret) {
		ret = sysfs_create_group(&atomisp_dev->kobj,
				atomisp_attribute_group);
		if (ret)
			dev_err(atomisp_dev,
				"%s Failed to create sysfs\n", __func__);
	}

	return ret;
}

void hmm_cleanup(void)
{
	sysfs_remove_group(&atomisp_dev->kobj, atomisp_attribute_group);

	/*
	 * free dummy memory first
	 */
	hmm_free(dummy_ptr);
	dummy_ptr = 0;

	hmm_bo_device_exit(&bo_device);
}

ia_css_ptr hmm_alloc(size_t bytes, enum hmm_bo_type type,
		int from_highmem, void *userptr, bool cached)
{
	unsigned int pgnr;
	struct hmm_buffer_object *bo;
	int ret;

	/*Get page number from size*/
	pgnr = size_to_pgnr_ceil(bytes);

	/*Buffer object structure init*/
	bo = hmm_bo_alloc(&bo_device, pgnr);
	if (!bo) {
		dev_err(atomisp_dev, "hmm_bo_create failed.\n");
		goto create_bo_err;
	}

	/*Allocate pages for memory*/
	ret = hmm_bo_alloc_pages(bo, type, from_highmem, userptr, cached);
	if (ret) {
		dev_err(atomisp_dev,
			    "hmm_bo_alloc_pages failed.\n");
		goto alloc_page_err;
	}

	/*Combind the virtual address and pages togather*/
	ret = hmm_bo_bind(bo);
	if (ret) {
		dev_err(atomisp_dev, "hmm_bo_bind failed.\n");
		goto bind_err;
	}

	hmm_mem_stat.tol_cnt += pgnr;

	return bo->start;

bind_err:
	hmm_bo_free_pages(bo);
alloc_page_err:
	hmm_bo_unref(bo);
create_bo_err:
	return 0;
}

void hmm_free(ia_css_ptr virt)
{
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_start(&bo_device, (unsigned int)virt);

	if (!bo) {
		dev_err(atomisp_dev,
			    "can not find buffer object start with "
			    "address 0x%x\n", (unsigned int)virt);
		return;
	}

	hmm_mem_stat.tol_cnt -= bo->pgnr;

	hmm_bo_unbind(bo);

	hmm_bo_free_pages(bo);

	hmm_bo_unref(bo);
}

static inline int hmm_check_bo(struct hmm_buffer_object *bo, unsigned int ptr)
{
	if (!bo) {
		dev_err(atomisp_dev,
			    "can not find buffer object contains "
			    "address 0x%x\n", ptr);
		return -EINVAL;
	}

	if (!hmm_bo_page_allocated(bo)) {
		dev_err(atomisp_dev,
			    "buffer object has no page allocated.\n");
		return -EINVAL;
	}

	if (!hmm_bo_allocated(bo)) {
		dev_err(atomisp_dev,
			    "buffer object has no virtual address"
			    " space allocated.\n");
		return -EINVAL;
	}

	return 0;
}

/*Read function in ISP memory management*/
static int load_and_flush_by_kmap(ia_css_ptr virt, void *data, unsigned int bytes)
{
	struct hmm_buffer_object *bo;
	unsigned int idx, offset, len;
	char *src, *des;
	int ret;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	ret = hmm_check_bo(bo, virt);
	if (ret)
		return ret;

	des = (char *)data;
	while (bytes) {
		idx = (virt - bo->start) >> PAGE_SHIFT;
		offset = (virt - bo->start) - (idx << PAGE_SHIFT);

		src = (char *)kmap(bo->page_obj[idx].page);
		if (!src) {
			dev_err(atomisp_dev,
				    "kmap buffer object page failed: "
				    "pg_idx = %d\n", idx);
			return -EINVAL;
		}

		src += offset;

		if ((bytes + offset) >= PAGE_SIZE) {
			len = PAGE_SIZE - offset;
			bytes -= len;
		} else {
			len = bytes;
			bytes = 0;
		}

		virt += len;	/* update virt for next loop */

		if (des) {
			memcpy(des, src, len);
			des += len;
		}

		clflush_cache_range(src, len);

		kunmap(bo->page_obj[idx].page);
	}

	return 0;
}

/*Read function in ISP memory management*/
static int load_and_flush(ia_css_ptr virt, void *data, unsigned int bytes)
{
	struct hmm_buffer_object *bo;
	int ret;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	ret = hmm_check_bo(bo, virt);
	if (ret)
		return ret;

	if (bo->status & HMM_BO_VMAPED || bo->status & HMM_BO_VMAPED_CACHED) {
		void *src = bo->vmap_addr;

		src += (virt - bo->start);
		memcpy(data, src, bytes);
		if (bo->status & HMM_BO_VMAPED_CACHED)
			clflush_cache_range(src, bytes);
	} else {
		void *vptr;

		vptr = hmm_bo_vmap(bo, true);
		if (!vptr)
			return load_and_flush_by_kmap(virt, data, bytes);
		else
			vptr = vptr + (virt - bo->start);

		memcpy(data, vptr, bytes);
		clflush_cache_range(vptr, bytes);
		hmm_bo_vunmap(bo);
	}

	return 0;
}

/*Read function in ISP memory management*/
int hmm_load(ia_css_ptr virt, void *data, unsigned int bytes)
{
	if (!data) {
		dev_err(atomisp_dev,
			 "hmm_load NULL argument\n");
		return -EINVAL;
	}
	return load_and_flush(virt, data, bytes);
}

/*Flush hmm data from the data cache*/
int hmm_flush(ia_css_ptr virt, unsigned int bytes)
{
	return load_and_flush(virt, NULL, bytes);
}

/*Write function in ISP memory management*/
int hmm_store(ia_css_ptr virt, const void *data, unsigned int bytes)
{
	struct hmm_buffer_object *bo;
	unsigned int idx, offset, len;
	char *src, *des;
	int ret;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	ret = hmm_check_bo(bo, virt);
	if (ret)
		return ret;

	if (bo->status & HMM_BO_VMAPED || bo->status & HMM_BO_VMAPED_CACHED) {
		void *dst = bo->vmap_addr;

		dst += (virt - bo->start);
		memcpy(dst, data, bytes);
		if (bo->status & HMM_BO_VMAPED_CACHED)
			clflush_cache_range(dst, bytes);
	} else {
		void *vptr;

		vptr = hmm_bo_vmap(bo, true);
		if (vptr) {
			vptr = vptr + (virt - bo->start);

			memcpy(vptr, data, bytes);
			clflush_cache_range(vptr, bytes);
			hmm_bo_vunmap(bo);
			return 0;
		}
	}

	src = (char *)data;
	while (bytes) {
		idx = (virt - bo->start) >> PAGE_SHIFT;
		offset = (virt - bo->start) - (idx << PAGE_SHIFT);

		if (in_atomic())
			des = (char *)kmap_atomic(bo->page_obj[idx].page);
		else
			des = (char *)kmap(bo->page_obj[idx].page);

		if (!des) {
			dev_err(atomisp_dev,
				    "kmap buffer object page failed: "
				    "pg_idx = %d\n", idx);
			return -EINVAL;
		}

		des += offset;

		if ((bytes + offset) >= PAGE_SIZE) {
			len = PAGE_SIZE - offset;
			bytes -= len;
		} else {
			len = bytes;
			bytes = 0;
		}

		virt += len;

		memcpy(des, src, len);

		src += len;

		clflush_cache_range(des, len);

		if (in_atomic())
			/*
			 * Note: kunmap_atomic requires return addr from
			 * kmap_atomic, not the page. See linux/highmem.h
			 */
			kunmap_atomic(des - offset);
		else
			kunmap(bo->page_obj[idx].page);
	}

	return 0;
}

/*memset function in ISP memory management*/
int hmm_set(ia_css_ptr virt, int c, unsigned int bytes)
{
	struct hmm_buffer_object *bo;
	unsigned int idx, offset, len;
	char *des;
	int ret;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	ret = hmm_check_bo(bo, virt);
	if (ret)
		return ret;

	if (bo->status & HMM_BO_VMAPED || bo->status & HMM_BO_VMAPED_CACHED) {
		void *dst = bo->vmap_addr;

		dst += (virt - bo->start);
		memset(dst, c, bytes);

		if (bo->status & HMM_BO_VMAPED_CACHED)
			clflush_cache_range(dst, bytes);
	} else {
		void *vptr;

		vptr = hmm_bo_vmap(bo, true);
		if (vptr) {
			vptr = vptr + (virt - bo->start);
			memset(vptr, c, bytes);
			clflush_cache_range(vptr, bytes);
			hmm_bo_vunmap(bo);
			return 0;
		}
	}

	while (bytes) {
		idx = (virt - bo->start) >> PAGE_SHIFT;
		offset = (virt - bo->start) - (idx << PAGE_SHIFT);

		des = (char *)kmap(bo->page_obj[idx].page);
		if (!des) {
			dev_err(atomisp_dev,
				    "kmap buffer object page failed: "
				    "pg_idx = %d\n", idx);
			return -EINVAL;
		}
		des += offset;

		if ((bytes + offset) >= PAGE_SIZE) {
			len = PAGE_SIZE - offset;
			bytes -= len;
		} else {
			len = bytes;
			bytes = 0;
		}

		virt += len;

		memset(des, c, len);

		clflush_cache_range(des, len);

		kunmap(bo->page_obj[idx].page);
	}

	return 0;
}

/*Virtual address to physical address convert*/
phys_addr_t hmm_virt_to_phys(ia_css_ptr virt)
{
	unsigned int idx, offset;
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	if (!bo) {
		dev_err(atomisp_dev,
			"can not find buffer object contains address 0x%x\n",
			virt);
		return -1;
	}

	idx = (virt - bo->start) >> PAGE_SHIFT;
	offset = (virt - bo->start) - (idx << PAGE_SHIFT);

	return page_to_phys(bo->page_obj[idx].page) + offset;
}

int hmm_mmap(struct vm_area_struct *vma, ia_css_ptr virt)
{
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_start(&bo_device, virt);
	if (!bo) {
		dev_err(atomisp_dev,
			"can not find buffer object start with address 0x%x\n",
			virt);
		return -EINVAL;
	}

	return hmm_bo_mmap(vma, bo);
}

/*Map ISP virtual address into IA virtual address*/
void *hmm_vmap(ia_css_ptr virt, bool cached)
{
	struct hmm_buffer_object *bo;
	void *ptr;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	if (!bo) {
		dev_err(atomisp_dev,
			    "can not find buffer object contains address 0x%x\n",
			    virt);
		return NULL;
	}

	ptr = hmm_bo_vmap(bo, cached);
	if (ptr)
		return ptr + (virt - bo->start);
	else
		return NULL;
}

/* Flush the memory which is mapped as cached memory through hmm_vmap */
void hmm_flush_vmap(ia_css_ptr virt)
{
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	if (!bo) {
		dev_warn(atomisp_dev,
			    "can not find buffer object contains address 0x%x\n",
			    virt);
		return;
	}

	hmm_bo_flush_vmap(bo);
}

void hmm_vunmap(ia_css_ptr virt)
{
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_in_range(&bo_device, virt);
	if (!bo) {
		dev_warn(atomisp_dev,
			"can not find buffer object contains address 0x%x\n",
			virt);
		return;
	}

	return hmm_bo_vunmap(bo);
}

int hmm_pool_register(unsigned int pool_size,
			enum hmm_pool_type pool_type)
{
	switch (pool_type) {
	case HMM_POOL_TYPE_RESERVED:
		reserved_pool.pops = &reserved_pops;
		return reserved_pool.pops->pool_init(&reserved_pool.pool_info,
							pool_size);
	case HMM_POOL_TYPE_DYNAMIC:
		dynamic_pool.pops = &dynamic_pops;
		return dynamic_pool.pops->pool_init(&dynamic_pool.pool_info,
							pool_size);
	default:
		dev_err(atomisp_dev, "invalid pool type.\n");
		return -EINVAL;
	}
}

void hmm_pool_unregister(enum hmm_pool_type pool_type)
{
	switch (pool_type) {
	case HMM_POOL_TYPE_RESERVED:
		if (reserved_pool.pops && reserved_pool.pops->pool_exit)
			reserved_pool.pops->pool_exit(&reserved_pool.pool_info);
		break;
	case HMM_POOL_TYPE_DYNAMIC:
		if (dynamic_pool.pops && dynamic_pool.pops->pool_exit)
			dynamic_pool.pops->pool_exit(&dynamic_pool.pool_info);
		break;
	default:
		dev_err(atomisp_dev, "invalid pool type.\n");
		break;
	}

	return;
}

void *hmm_isp_vaddr_to_host_vaddr(ia_css_ptr ptr, bool cached)
{
	return hmm_vmap(ptr, cached);
	/* vmunmap will be done in hmm_bo_release() */
}

ia_css_ptr hmm_host_vaddr_to_hrt_vaddr(const void *ptr)
{
	struct hmm_buffer_object *bo;

	bo = hmm_bo_device_search_vmap_start(&bo_device, ptr);
	if (bo)
		return bo->start;

	dev_err(atomisp_dev,
		"can not find buffer object whose kernel virtual address is %p\n",
		ptr);
	return 0;
}

void hmm_show_mem_stat(const char *func, const int line)
{
	trace_printk("tol_cnt=%d usr_size=%d res_size=%d res_cnt=%d sys_size=%d  dyc_thr=%d dyc_size=%d.\n",
			hmm_mem_stat.tol_cnt,
			hmm_mem_stat.usr_size, hmm_mem_stat.res_size,
			hmm_mem_stat.res_cnt, hmm_mem_stat.sys_size,
			hmm_mem_stat.dyc_thr, hmm_mem_stat.dyc_size);
}

void hmm_init_mem_stat(int res_pgnr, int dyc_en, int dyc_pgnr)
{
	hmm_mem_stat.res_size = res_pgnr;
	/* If reserved mem pool is not enabled, set its "mem stat" values as -1. */
	if (0 == hmm_mem_stat.res_size) {
		hmm_mem_stat.res_size = -1;
		hmm_mem_stat.res_cnt = -1;
	}

	/* If dynamic memory pool is not enabled, set its "mem stat" values as -1. */
	if (!dyc_en) {
		hmm_mem_stat.dyc_size = -1;
		hmm_mem_stat.dyc_thr = -1;
	} else {
		hmm_mem_stat.dyc_size = 0;
		hmm_mem_stat.dyc_thr = dyc_pgnr;
	}
	hmm_mem_stat.usr_size = 0;
	hmm_mem_stat.sys_size = 0;
	hmm_mem_stat.tol_cnt = 0;
}
