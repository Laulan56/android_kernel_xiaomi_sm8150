/*
 * drivers/staging/android/ion/ion_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/rtmutex.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include "ion.h"

void *ion_heap_map_kernel(struct ion_heap *heap,
			  struct ion_buffer *buffer)
{
	struct scatterlist *sg;
	int i, j;
	void *vaddr;
	pgprot_t pgprot;
	struct sg_table *table = buffer->sg_table;
	int npages = PAGE_ALIGN(buffer->size) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;

	if (!pages)
		return ERR_PTR(-ENOMEM);

	if (buffer->flags & ION_FLAG_CACHED)
		pgprot = PAGE_KERNEL;
	else
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	for_each_sg(table->sgl, sg, table->nents, i) {
		int npages_this_entry = PAGE_ALIGN(sg->length) / PAGE_SIZE;
		struct page *page = sg_page(sg);

		BUG_ON(i >= npages);
		for (j = 0; j < npages_this_entry; j++)
			*(tmp++) = page++;
	}
	vaddr = vmap(pages, npages, VM_MAP, pgprot);
	vfree(pages);

	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	return vaddr;
}

void ion_heap_unmap_kernel(struct ion_heap *heap,
			   struct ion_buffer *buffer)
{
	vunmap(buffer->vaddr);
}

int ion_heap_map_user(struct ion_heap *heap, struct ion_buffer *buffer,
		      struct vm_area_struct *vma)
{
	struct sg_table *table = buffer->sg_table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	int i;
	int ret;

	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);
		if (ret)
			return ret;
		addr += len;
		if (addr >= vma->vm_end)
			return 0;
	}
	return 0;
}

static int ion_heap_clear_pages(struct page **pages, int num, pgprot_t pgprot)
{
	void *addr = vmap(pages, num, VM_MAP, pgprot);

	if (!addr)
		return -ENOMEM;
	memset(addr, 0, PAGE_SIZE * num);
	vunmap(addr);

	return 0;
}

static int ion_heap_sglist_zero(struct scatterlist *sgl, unsigned int nents,
				pgprot_t pgprot)
{
	int p = 0;
	int ret = 0;
	struct sg_page_iter piter;
	struct page *pages[32];

	for_each_sg_page(sgl, &piter, nents, 0) {
		pages[p++] = sg_page_iter_page(&piter);
		if (p == ARRAY_SIZE(pages)) {
			ret = ion_heap_clear_pages(pages, p, pgprot);
			if (ret)
				return ret;
			p = 0;
		}
	}
	if (p)
		ret = ion_heap_clear_pages(pages, p, pgprot);

	return ret;
}

int ion_heap_buffer_zero(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	pgprot_t pgprot;

	if (buffer->flags & ION_FLAG_CACHED)
		pgprot = PAGE_KERNEL;
	else
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	return ion_heap_sglist_zero(table->sgl, table->nents, pgprot);
}

int ion_heap_pages_zero(struct page *page, size_t size, pgprot_t pgprot)
{
	struct scatterlist sg;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, size, 0);
	return ion_heap_sglist_zero(&sg, 1, pgprot);
}

static unsigned long ion_heap_shrink_count(struct shrinker *shrinker,
					   struct shrink_control *sc)
{
	struct ion_heap *heap = container_of(shrinker, typeof(*heap), shrinker);

	if (heap->ops->shrink)
		return heap->ops->shrink(heap, sc->gfp_mask, 0);

	return 0;
}

static unsigned long ion_heap_shrink_scan(struct shrinker *shrinker,
					  struct shrink_control *sc)
{
	struct ion_heap *heap = container_of(shrinker, typeof(*heap), shrinker);

	if (heap->ops->shrink)
		return heap->ops->shrink(heap, sc->gfp_mask, sc->nr_to_scan);

	return 0;
}

void ion_heap_init_shrinker(struct ion_heap *heap)
{
	heap->shrinker.count_objects = ion_heap_shrink_count;
	heap->shrinker.scan_objects = ion_heap_shrink_scan;
	heap->shrinker.seeks = DEFAULT_SEEKS;
	heap->shrinker.batch = 0;
	register_shrinker(&heap->shrinker);
}

struct ion_heap *ion_heap_create(struct ion_platform_heap *heap_data)
{
	struct ion_heap *heap = NULL;
	int heap_type = heap_data->type;

	switch (heap_type) {
	case ION_HEAP_TYPE_SYSTEM_CONTIG:
		pr_err("%s: Heap type is disabled: %d\n", __func__,
		       heap_data->type);
		break;
	case ION_HEAP_TYPE_SYSTEM:
		heap = ion_system_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CARVEOUT:
		heap = ion_carveout_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_CHUNK:
		heap = ion_chunk_heap_create(heap_data);
		break;
#ifdef CONFIG_CMA
	case (enum ion_heap_type)ION_HEAP_TYPE_SECURE_DMA:
		heap = ion_secure_cma_heap_create(heap_data);
		break;
	case ION_HEAP_TYPE_DMA:
		heap = ion_cma_heap_create(heap_data);
		break;
	case (enum ion_heap_type)ION_HEAP_TYPE_HYP_CMA:
		heap = ion_cma_secure_heap_create(heap_data);
		break;
#endif
	case (enum ion_heap_type)ION_HEAP_TYPE_SYSTEM_SECURE:
		heap = ion_system_secure_heap_create(heap_data);
		break;
	case (enum ion_heap_type)ION_HEAP_TYPE_SECURE_CARVEOUT:
		heap = ion_secure_carveout_heap_create(heap_data);
		break;
	default:
		pr_err("%s: Invalid heap type %d\n", __func__,
		       heap_data->type);
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR_OR_NULL(heap)) {
		pr_err("%s: error creating heap %s type %d base %pa size %zu\n",
		       __func__, heap_data->name, heap_data->type,
		       &heap_data->base, heap_data->size);
		return ERR_PTR(-EINVAL);
	}

	heap->name = heap_data->name;
	heap->id = heap_data->id;
	heap->priv = heap_data->priv;
	return heap;
}
EXPORT_SYMBOL(ion_heap_create);
