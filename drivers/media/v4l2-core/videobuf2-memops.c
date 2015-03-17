/*
 * videobuf2-memops.c - generic memory handling routines for videobuf2
 *
 * Copyright (C) 2010 Samsung Electronics
 *
 * Author: Pawel Osciak <pawel@osciak.com>
 *	   Marek Szyprowski <m.szyprowski@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/file.h>

#include <media/videobuf2-core.h>
#include <media/videobuf2-memops.h>

/**
 * vb2_create_pfnvec() - map virtual addresses to pfns
 * @start:	Virtual user address where we start mapping
 * @length:	Length of a range to map
 * @write:	Should we map for writing into the area
 *
 * This function allocates and fills in a vector with pfns corresponding to
 * virtual address range passed in arguments. If pfns have corresponding pages,
 * page references are also grabbed to pin pages in memory. The function
 * returns pointer to the vector on success and error pointer in case of
 * failure. Returned vector needs to be freed via vb2_destroy_pfnvec().
 */
struct pinned_pfns *vb2_create_pfnvec(unsigned long start, unsigned long length,
				      bool write)
{
	int ret;
	unsigned long first, last;
	unsigned long nr;
	struct pinned_pfns *pfns;

	first = start >> PAGE_SHIFT;
	last = (start + length - 1) >> PAGE_SHIFT;
	nr = last - first + 1;
	pfns = pfns_vector_create(nr);
	if (!pfns)
		return ERR_PTR(-ENOMEM);
	ret = get_vaddr_pfns(start, nr, write, 1, pfns);
	if (ret < 0)
		goto out_destroy;
	/* We accept only complete set of PFNs */
	if (ret != nr) {
		ret = -EFAULT;
		goto out_release;
	}
	return pfns;
out_release:
	put_vaddr_pfns(pfns);
out_destroy:
	pfns_vector_destroy(pfns);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(vb2_create_pfnvec);

/**
 * vb2_destroy_pfnvec() - release vector of mapped pfns
 * @pfns:	vector of pfns to release
 *
 * This releases references to all pages in the vector @pfns (if corresponding
 * pfns are backed by pages) and frees the passed vector.
 */
void vb2_destroy_pfnvec(struct pinned_pfns *pfns)
{
	put_vaddr_pfns(pfns);
	pfns_vector_destroy(pfns);
}
EXPORT_SYMBOL(vb2_destroy_pfnvec);

/**
 * vb2_common_vm_open() - increase refcount of the vma
 * @vma:	virtual memory region for the mapping
 *
 * This function adds another user to the provided vma. It expects
 * struct vb2_vmarea_handler pointer in vma->vm_private_data.
 */
static void vb2_common_vm_open(struct vm_area_struct *vma)
{
	struct vb2_vmarea_handler *h = vma->vm_private_data;

	pr_debug("%s: %p, refcount: %d, vma: %08lx-%08lx\n",
	       __func__, h, atomic_read(h->refcount), vma->vm_start,
	       vma->vm_end);

	atomic_inc(h->refcount);
}

/**
 * vb2_common_vm_close() - decrease refcount of the vma
 * @vma:	virtual memory region for the mapping
 *
 * This function releases the user from the provided vma. It expects
 * struct vb2_vmarea_handler pointer in vma->vm_private_data.
 */
static void vb2_common_vm_close(struct vm_area_struct *vma)
{
	struct vb2_vmarea_handler *h = vma->vm_private_data;

	pr_debug("%s: %p, refcount: %d, vma: %08lx-%08lx\n",
	       __func__, h, atomic_read(h->refcount), vma->vm_start,
	       vma->vm_end);

	h->put(h->arg);
}

/**
 * vb2_common_vm_ops - common vm_ops used for tracking refcount of mmaped
 * video buffers
 */
const struct vm_operations_struct vb2_common_vm_ops = {
	.open = vb2_common_vm_open,
	.close = vb2_common_vm_close,
};
EXPORT_SYMBOL_GPL(vb2_common_vm_ops);

MODULE_DESCRIPTION("common memory handling routines for videobuf2");
MODULE_AUTHOR("Pawel Osciak <pawel@osciak.com>");
MODULE_LICENSE("GPL");
