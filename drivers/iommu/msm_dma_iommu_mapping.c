// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2020 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_map {
	struct device *dev;
	struct msm_iommu_data *data;
	struct list_head data_node;
	struct list_head dev_node;
	struct scatterlist *sgl;
	enum dma_data_direction dir;
	unsigned long attrs;
	int nents;
	int refcount;
};

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_data *data,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &data->map_list, data_node) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static void msm_iommu_map_free(struct msm_iommu_map *map)
{
	struct sg_table table = {
		.sgl = map->sgl,
		.nents = map->nents,
		.orig_nents = map->nents
	};

	dma_unmap_sg_attrs(map->dev, map->sgl, map->nents, map->dir,
			   map->attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(&table);
	list_del(&map->data_node);
	list_del(&map->dev_node);
	kfree(map);
}

static struct scatterlist *clone_sgl(struct scatterlist *sgl, int nents)
{
	struct scatterlist *d, *s;
	struct sg_table table;

	sg_alloc_table(&table, nents, GFP_KERNEL | __GFP_NOFAIL);
	for (d = table.sgl, s = sgl;
	     nents > SG_MAX_SINGLE_ALLOC; nents -= SG_MAX_SINGLE_ALLOC - 1,
	     d = sg_chain_ptr(&d[SG_MAX_SINGLE_ALLOC - 1]),
	     s = sg_chain_ptr(&s[SG_MAX_SINGLE_ALLOC - 1]))
		memcpy(d, s, (SG_MAX_SINGLE_ALLOC - 1) * sizeof(*d));

	if (nents)
		memcpy(d, s, nents * sizeof(*d));

	return table.sgl;
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
			 enum dma_data_direction dir, struct dma_buf *dmabuf,
			 unsigned long attrs)
{
	struct msm_iommu_data *data = dmabuf->priv;
	struct msm_iommu_map *map;

	mutex_lock(&dev->iommu_map_lock);
	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (map) {
		struct scatterlist *d = sgl, *s = map->sgl;

		map->refcount++;
		do {
			d->dma_address = s->dma_address;
			d->dma_length = s->dma_length;
		} while ((s = sg_next(s)) && s->dma_length && (d = sg_next(d)));
		if (is_device_dma_coherent(dev))
			dmb(ish);
	} else {
		if (dma_map_sg_attrs(dev, sgl, nents, dir, attrs)) {
			map = kmalloc(sizeof(*map), GFP_KERNEL | __GFP_NOFAIL);
			map->sgl = clone_sgl(sgl, nents);
			map->data = data;
			map->dev = dev;
			map->dir = dir;
			map->nents = nents;
			map->refcount = 2;
			map->attrs = attrs;
			list_add(&map->data_node, &data->map_list);
			list_add(&map->dev_node, &dev->iommu_map_list);
		} else {
			nents = 0;
		}
	}
	mutex_unlock(&data->lock);
	mutex_unlock(&dev->iommu_map_lock);

	return nents;
}

void msm_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    struct dma_buf *dmabuf, unsigned long attrs)
{
	struct msm_iommu_data *data = dmabuf->priv;
	struct msm_iommu_map *map;

	mutex_lock(&dev->iommu_map_lock);
	mutex_lock(&data->lock);
	map = msm_iommu_map_lookup(data, dev);
	if (map && !--map->refcount)
		msm_iommu_map_free(map);
	mutex_unlock(&data->lock);
	mutex_unlock(&dev->iommu_map_lock);
}

void msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_map *map, *tmp;

	mutex_lock(&dev->iommu_map_lock);
	list_for_each_entry_safe(map, tmp, &dev->iommu_map_list, dev_node) {
		struct msm_iommu_data *data = map->data;

		mutex_lock(&data->lock);
		msm_iommu_map_free(map);
		mutex_unlock(&data->lock);
	}
	mutex_unlock(&dev->iommu_map_lock);
}

void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	struct msm_iommu_map *map, *tmp;
	int retry = 0;

	do {
		mutex_lock(&data->lock);
		list_for_each_entry_safe(map, tmp, &data->map_list, data_node) {
			struct device *dev = map->dev;

			if (!mutex_trylock(&dev->iommu_map_lock)) {
				retry = 1;
				break;
			}

			msm_iommu_map_free(map);
			mutex_unlock(&dev->iommu_map_lock);
		}
		mutex_unlock(&data->lock);
	} while (retry--);
}
