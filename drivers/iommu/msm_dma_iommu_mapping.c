// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#include <linux/dma-buf.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <asm/barrier.h>

struct msm_iommu_meta {
	struct msm_iommu_data *data;
	struct list_head lnode;
	struct list_head map_list;
	int refcount;
};

struct msm_iommu_map {
	struct device *dev;
	struct list_head lnode;
	struct scatterlist sgl;
	enum dma_data_direction dir;
	unsigned int nents;
	int refcount;
};

static LIST_HEAD(meta_list);
static DEFINE_SPINLOCK(meta_list_lock);
static DECLARE_RWSEM(unmap_all_rwsem);

static struct msm_iommu_map *msm_iommu_map_lookup(struct msm_iommu_meta *meta,
						  struct device *dev)
{
	struct msm_iommu_map *map;

	list_for_each_entry(map, &meta->map_list, lnode) {
		if (map->dev == dev)
			return map;
	}

	return NULL;
}

static void msm_iommu_map_free(struct msm_iommu_meta *meta,
			       struct msm_iommu_map *map)
{
	struct msm_iommu_data *data = meta->data;

	if (--meta->refcount) {
		list_del(&map->lnode);
	} else {
		spin_lock(&meta_list_lock);
		list_del(&meta->lnode);
		spin_unlock(&meta_list_lock);

		data->meta = NULL;
		kfree(meta);
	}

	dma_unmap_sg(map->dev, &map->sgl, map->nents, map->dir);
	kfree(map);
}

int msm_dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
			 enum dma_data_direction dir, struct dma_buf *dma_buf,
			 struct dma_attrs *attrs)
{
	static const gfp_t gfp_flags_nofail = GFP_KERNEL | __GFP_NOFAIL;
	int not_lazy = dma_get_attr(DMA_ATTR_NO_DELAYED_UNMAP, attrs);
	struct msm_iommu_data *data = dma_buf->priv;
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;

	mutex_lock(&data->lock);
	down_read(&unmap_all_rwsem);
	meta = data->meta;
	map = meta ? msm_iommu_map_lookup(meta, dev) : NULL;
	if (map) {
		map->refcount++;
		sg->dma_address = map->sgl.dma_address;
		sg->dma_length = map->sgl.dma_length;
		if (is_device_dma_coherent(dev))
			dmb(ish);
	} else {
		nents = dma_map_sg_attrs(dev, sg, nents, dir, attrs);
		if (nents) {
			map = kmalloc(sizeof(*map), gfp_flags_nofail);
			map->dev = dev;
			map->dir = dir;
			map->nents = nents;
			map->refcount = 2 - not_lazy;
			map->sgl.dma_address = sg->dma_address;
			map->sgl.dma_length = sg->dma_length;
			map->sgl.page_link = sg->page_link;
			map->sgl.offset = sg->offset;
			map->sgl.length = sg->length;

			if (meta) {
				meta->refcount++;
			} else {
				meta = kmalloc(sizeof(*meta), gfp_flags_nofail);
				meta->data = data;
				meta->refcount = 1;
				INIT_LIST_HEAD(&meta->map_list);
				data->meta = meta;

				spin_lock(&meta_list_lock);
				list_add(&meta->lnode, &meta_list);
				spin_unlock(&meta_list_lock);
			}
			list_add(&map->lnode, &meta->map_list);
		}
	}
	up_read(&unmap_all_rwsem);
	mutex_unlock(&data->lock);

	return nents;
}

void msm_dma_unmap_sg(struct device *dev, struct scatterlist *sgl, int nents,
		      enum dma_data_direction dir, struct dma_buf *dma_buf)
{
	struct msm_iommu_data *data = dma_buf->priv;
	struct msm_iommu_meta *meta;
	struct msm_iommu_map *map;

	mutex_lock(&data->lock);
	down_read(&unmap_all_rwsem);
	meta = data->meta;
	if (meta) {
		map = msm_iommu_map_lookup(meta, dev);
		if (map && !--map->refcount)
			msm_iommu_map_free(meta, map);
	}
	up_read(&unmap_all_rwsem);
	mutex_unlock(&data->lock);
}

int msm_dma_unmap_all_for_dev(struct device *dev)
{
	struct msm_iommu_meta *meta, *tmp_meta;
	struct msm_iommu_map *map;

	down_write(&unmap_all_rwsem);
	list_for_each_entry_safe(meta, tmp_meta, &meta_list, lnode) {
		map = msm_iommu_map_lookup(meta, dev);
		if (map)
			msm_iommu_map_free(meta, map);
	}
	up_write(&unmap_all_rwsem);

	return 0;
}

void msm_dma_buf_freed(struct msm_iommu_data *data)
{
	struct msm_iommu_map *map, *tmp_map;
	struct msm_iommu_meta *meta;

	mutex_lock(&data->lock);
	down_read(&unmap_all_rwsem);
	meta = data->meta;
	if (meta) {
		list_for_each_entry_safe(map, tmp_map, &meta->map_list, lnode)
			msm_iommu_map_free(meta, map);
	}
	up_read(&unmap_all_rwsem);
	mutex_unlock(&data->lock);
}
