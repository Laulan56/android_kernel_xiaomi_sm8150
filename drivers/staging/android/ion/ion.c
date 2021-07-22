// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 Google, Inc.
 * Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "ion_secure_util.h"
#include "ion_system_secure_heap.h"

struct ion_dma_buf_attachment {
	struct ion_dma_buf_attachment *next;
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	struct rw_semaphore map_rwsem;
	bool dma_mapped;
};

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static const struct file_operations ion_fops = {
	.unlocked_ioctl = ion_ioctl,
	.compat_ioctl = ion_ioctl
};

static struct ion_device ion_dev = {
	.heaps = PLIST_HEAD_INIT(ion_dev.heaps),
	.dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "ion",
		.fops = &ion_fops
	}
};

static void ion_buffer_free_work(struct work_struct *work)
{
	struct ion_buffer *buffer = container_of(work, typeof(*buffer), free);
	struct ion_dma_buf_attachment *a, *next;
	struct ion_heap *heap = buffer->heap;

	msm_dma_buf_freed(&buffer->iommu_data);
	for (a = buffer->attachments; a; a = next) {
		next = a->next;
		sg_free_table(&a->table);
		kfree(a);
	}
	if (buffer->kmap_refcount)
		heap->ops->unmap_kernel(heap, buffer);
	heap->ops->free(buffer);
	kfree(buffer);
}

static struct ion_buffer *ion_buffer_create(struct ion_heap *heap, size_t len,
					    unsigned int flags)
{
	struct ion_buffer *buffer;
	int ret;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	*buffer = (typeof(*buffer)){
		.flags = flags,
		.heap = heap,
		.size = len,
		.kmap_lock = __MUTEX_INITIALIZER(buffer->kmap_lock),
		.free = __WORK_INITIALIZER(buffer->free, ion_buffer_free_work),
		.map_freelist = LIST_HEAD_INIT(buffer->map_freelist),
		.freelist_lock = __SPIN_LOCK_INITIALIZER(buffer->freelist_lock),
		.iommu_data = {
			.map_list = LIST_HEAD_INIT(buffer->iommu_data.map_list),
			.lock = __MUTEX_INITIALIZER(buffer->iommu_data.lock)
		}
	};

	ret = heap->ops->allocate(heap, buffer, len, flags);
	if (ret) {
		if (ret == -EINTR || !(heap->flags & ION_HEAP_FLAG_DEFER_FREE))
			goto free_buffer;

		drain_workqueue(heap->wq);
		if (heap->ops->allocate(heap, buffer, len, flags))
			goto free_buffer;
	}

	return buffer;

free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a = attachment->priv;
	int count, map_attrs = attachment->dma_map_attrs;

	if (!(buffer->flags & ION_FLAG_CACHED) ||
	    !hlos_accessible_buffer(buffer))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	down_write(&a->map_rwsem);
	if (map_attrs & DMA_ATTR_DELAYED_UNMAP)
		count = msm_dma_map_sg_attrs(attachment->dev, a->table.sgl,
					     a->table.nents, dir, dmabuf,
					     map_attrs);
	else
		count = dma_map_sg_attrs(attachment->dev, a->table.sgl,
					 a->table.nents, dir, map_attrs);
	if (count)
		a->dma_mapped = true;
	up_write(&a->map_rwsem);

	return count ? &a->table : ERR_PTR(-ENOMEM);
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a = attachment->priv;
	int map_attrs = attachment->dma_map_attrs;

	if (!(buffer->flags & ION_FLAG_CACHED) ||
	    !hlos_accessible_buffer(buffer))
		map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	down_write(&a->map_rwsem);
	if (map_attrs & DMA_ATTR_DELAYED_UNMAP)
		msm_dma_unmap_sg_attrs(attachment->dev, table->sgl,
				       table->nents, dir, dmabuf, map_attrs);
	else
		dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
				   dir, map_attrs);
	a->dma_mapped = false;
	up_write(&a->map_rwsem);
}

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_heap *heap = buffer->heap;

	if (!buffer->heap->ops->map_user)
		return -EINVAL;

	if (!(buffer->flags & ION_FLAG_CACHED))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return heap->ops->map_user(heap, buffer, vma);
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_heap *heap = buffer->heap;

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		queue_work(heap->wq, &buffer->free);
	else
		ion_buffer_free_work(&buffer->free);
}

static void *ion_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_heap *heap = buffer->heap;
	void *vaddr;

	if (!heap->ops->map_kernel)
		return ERR_PTR(-ENODEV);

	mutex_lock(&buffer->kmap_lock);
	if (buffer->kmap_refcount) {
		vaddr = buffer->vaddr;
		buffer->kmap_refcount++;
	} else {
		vaddr = heap->ops->map_kernel(heap, buffer);
		if (IS_ERR_OR_NULL(vaddr)) {
			vaddr = ERR_PTR(-EINVAL);
		} else {
			buffer->vaddr = vaddr;
			buffer->kmap_refcount++;
		}
	}
	mutex_unlock(&buffer->kmap_lock);

	return vaddr;
}

static void ion_dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_heap *heap = buffer->heap;

	mutex_lock(&buffer->kmap_lock);
	if (!--buffer->kmap_refcount)
		heap->ops->unmap_kernel(heap, buffer);
	mutex_unlock(&buffer->kmap_lock);
}

static void *ion_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	void *vaddr;

	vaddr = ion_dma_buf_vmap(dmabuf);
	if (IS_ERR(vaddr))
		return vaddr;

	return vaddr + offset * PAGE_SIZE;
}

static void ion_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	ion_dma_buf_vunmap(dmabuf, NULL);
}

static int ion_dup_sg_table(struct sg_table *dst, struct sg_table *src)
{
	unsigned int nents = src->nents;
	struct scatterlist *d, *s;

	if (sg_alloc_table(dst, nents, GFP_KERNEL))
		return -ENOMEM;

	for (d = dst->sgl, s = src->sgl;
	     nents > SG_MAX_SINGLE_ALLOC; nents -= SG_MAX_SINGLE_ALLOC - 1,
	     d = sg_chain_ptr(&d[SG_MAX_SINGLE_ALLOC - 1]),
	     s = sg_chain_ptr(&s[SG_MAX_SINGLE_ALLOC - 1]))
		memcpy(d, s, (SG_MAX_SINGLE_ALLOC - 1) * sizeof(*d));

	if (nents)
		memcpy(d, s, nents * sizeof(*d));

	return 0;
}

static int ion_dma_buf_attach(struct dma_buf *dmabuf, struct device *dev,
			      struct dma_buf_attachment *attachment)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a;

	spin_lock(&buffer->freelist_lock);
	list_for_each_entry(a, &buffer->map_freelist, list) {
		if (a->dev == dev) {
			list_del(&a->list);
			spin_unlock(&buffer->freelist_lock);
			attachment->priv = a;
			return 0;
		}
	}
	spin_unlock(&buffer->freelist_lock);

	a = kmalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	if (ion_dup_sg_table(&a->table, buffer->sg_table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->dev = dev;
	a->dma_mapped = false;
	a->map_rwsem = (struct rw_semaphore)__RWSEM_INITIALIZER(a->map_rwsem);
	attachment->priv = a;
	a->next = buffer->attachments;
	buffer->attachments = a;

	return 0;
}

static void ion_dma_buf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a = attachment->priv;

	spin_lock(&buffer->freelist_lock);
	list_add(&a->list, &buffer->map_freelist);
	spin_unlock(&buffer->freelist_lock);
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction dir)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (!(buffer->flags & ION_FLAG_CACHED))
		return 0;

	for (a = buffer->attachments; a; a = a->next) {
		if (down_read_trylock(&a->map_rwsem)) {
			if (a->dma_mapped)
				dma_sync_sg_for_cpu(a->dev, a->table.sgl,
						    a->table.nents, dir);
			up_read(&a->map_rwsem);
		}
	}

	return 0;
}

static int ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction dir)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (!(buffer->flags & ION_FLAG_CACHED))
		return 0;

	for (a = buffer->attachments; a; a = a->next) {
		if (down_read_trylock(&a->map_rwsem)) {
			if (a->dma_mapped)
				dma_sync_sg_for_device(a->dev, a->table.sgl,
						       a->table.nents, dir);
			up_read(&a->map_rwsem);
		}
	}

	return 0;
}

static void ion_sgl_sync_range(struct device *dev, struct scatterlist *sgl,
			       unsigned int nents, unsigned long offset,
			       unsigned long len, enum dma_data_direction dir,
			       bool for_cpu)
{
	dma_addr_t sg_dma_addr = sg_dma_address(sgl);
	unsigned long total = 0;
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		unsigned long sg_offset, sg_left, size;

		total += sg->length;
		if (total <= offset) {
			sg_dma_addr += sg->length;
			continue;
		}

		sg_left = total - offset;
		sg_offset = sg->length - sg_left;
		size = min(len, sg_left);
		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, sg_dma_addr,
						      sg_offset, size, dir);
		else
			dma_sync_single_range_for_device(dev, sg_dma_addr,
							 sg_offset, size, dir);
		len -= size;
		if (!len)
			break;

		offset += size;
		sg_dma_addr += sg->length;
	}
}

static int ion_dma_buf_begin_cpu_access_partial(struct dma_buf *dmabuf,
						enum dma_data_direction dir,
						unsigned int offset,
						unsigned int len)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (!(buffer->flags & ION_FLAG_CACHED))
		return 0;

	for (a = buffer->attachments; a; a = a->next) {
		if (a->table.nents > 1 && sg_next(a->table.sgl)->dma_length) {
			ret = -EINVAL;
			continue;
		}

		if (down_read_trylock(&a->map_rwsem)) {
			if (a->dma_mapped)
				ion_sgl_sync_range(a->dev, a->table.sgl,
						   a->table.nents, offset, len,
						   dir, true);
			up_read(&a->map_rwsem);
		}
	}

	return ret;
}

static int ion_dma_buf_end_cpu_access_partial(struct dma_buf *dmabuf,
					      enum dma_data_direction dir,
					      unsigned int offset,
					      unsigned int len)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	if (!hlos_accessible_buffer(buffer))
		return -EPERM;

	if (!(buffer->flags & ION_FLAG_CACHED))
		return 0;

	for (a = buffer->attachments; a; a = a->next) {
		if (a->table.nents > 1 && sg_next(a->table.sgl)->dma_length) {
			ret = -EINVAL;
			continue;
		}

		if (down_read_trylock(&a->map_rwsem)) {
			if (a->dma_mapped)
				ion_sgl_sync_range(a->dev, a->table.sgl,
						   a->table.nents, offset, len,
						   dir, false);
			up_read(&a->map_rwsem);
		}
	}

	return ret;
}

static int ion_dma_buf_get_flags(struct dma_buf *dmabuf, unsigned long *flags)
{
	struct ion_buffer *buffer = container_of(dmabuf->priv, typeof(*buffer),
						 iommu_data);

	*flags = buffer->flags;
	return 0;
}

static const struct dma_buf_ops ion_dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.attach = ion_dma_buf_attach,
	.detach = ion_dma_buf_detach,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.begin_cpu_access_partial = ion_dma_buf_begin_cpu_access_partial,
	.end_cpu_access_partial = ion_dma_buf_end_cpu_access_partial,
	.map_atomic = ion_dma_buf_kmap,
	.unmap_atomic = ion_dma_buf_kunmap,
	.map = ion_dma_buf_kmap,
	.unmap = ion_dma_buf_kunmap,
	.vmap = ion_dma_buf_vmap,
	.vunmap = ion_dma_buf_vunmap,
	.get_flags = ion_dma_buf_get_flags
};

struct dma_buf *ion_alloc_dmabuf(size_t len, unsigned int heap_id_mask,
				 unsigned int flags)
{
	struct ion_device *idev = &ion_dev;
	struct dma_buf_export_info exp_info;
	struct ion_buffer *buffer = NULL;
	struct dma_buf *dmabuf;
	struct ion_heap *heap;

	len = PAGE_ALIGN(len);
	if (!len)
		return ERR_PTR(-EINVAL);

	plist_for_each_entry(heap, &idev->heaps, node) {
		if (BIT(heap->id) & heap_id_mask) {
			buffer = ion_buffer_create(heap, len, flags);
			if (!IS_ERR(buffer) || PTR_ERR(buffer) == -EINTR)
				break;
		}
	}

	if (!buffer)
		return ERR_PTR(-ENODEV);

	if (IS_ERR(buffer))
		return ERR_CAST(buffer);

	exp_info = (typeof(exp_info)){
		.ops = &ion_dma_buf_ops,
		.size = buffer->size,
		.flags = O_RDWR,
		.priv = &buffer->iommu_data
	};

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf))
		ion_buffer_free_work(&buffer->free);

	return dmabuf;
}

static int ion_alloc_fd(struct ion_allocation_data *a)
{
	struct dma_buf *dmabuf;
	int fd;

	dmabuf = ion_alloc_dmabuf(a->len, a->heap_id_mask, a->flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}

void ion_add_heap(struct ion_device *idev, struct ion_heap *heap)
{
	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE) {
		heap->wq = alloc_workqueue("%s", WQ_UNBOUND | WQ_MEM_RECLAIM |
					   WQ_CPU_INTENSIVE, 0, heap->name);
		BUG_ON(!heap->wq);
	}

	if (heap->ops->shrink)
		ion_heap_init_shrinker(heap);

	plist_node_init(&heap->node, -heap->id);
	plist_add(&heap->node, &idev->heaps);
}

static int ion_walk_heaps(int heap_id, int type, void *data,
			  int (*f)(struct ion_heap *heap, void *data))
{
	struct ion_device *idev = &ion_dev;
	struct ion_heap *heap;
	int ret = 0;

	plist_for_each_entry(heap, &idev->heaps, node) {
		if (heap->type == type && ION_HEAP(heap->id) == heap_id) {
			ret = f(heap, data);
			break;
		}
	}

	return ret;
}

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	union {
		struct ion_allocation_data allocation;
		struct ion_prefetch_data prefetch_data;
	} data;
	int fd, *output;

	if (_IOC_SIZE(cmd) > sizeof(data))
		return -EINVAL;

	if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	switch (cmd) {
	case ION_IOC_ALLOC:
		fd = ion_alloc_fd(&data.allocation);
		if (fd < 0)
			return fd;

		output = &fd;
		arg += offsetof(struct ion_allocation_data, fd);
		break;
	case ION_IOC_PREFETCH:
		return ion_walk_heaps(data.prefetch_data.heap_id,
				      ION_HEAP_TYPE_SYSTEM_SECURE,
				      &data.prefetch_data,
				      ion_system_secure_heap_prefetch);
	case ION_IOC_DRAIN:
		return ion_walk_heaps(data.prefetch_data.heap_id,
				      ION_HEAP_TYPE_SYSTEM_SECURE,
				      &data.prefetch_data,
				      ion_system_secure_heap_drain);
	default:
		return -ENOTTY;
	}

	if (copy_to_user((void __user *)arg, output, sizeof(*output)))
		return -EFAULT;

	return 0;
}

struct ion_device *ion_device_create(void)
{
	struct ion_device *idev = &ion_dev;
	int ret;

	ret = misc_register(&idev->dev);
	if (ret)
		return ERR_PTR(ret);

	return idev;
}
