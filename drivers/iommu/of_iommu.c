/*
 * OF helpers for IOMMU
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/export.h>
#include <linux/iommu.h>
#include <linux/limits.h>
#include <linux/of.h>
#include <linux/of_iommu.h>
#include <linux/slab.h>

/**
 * of_get_dma_window - Parse *dma-window property and returns 0 if found.
 *
 * @dn: device node
 * @prefix: prefix for property name if any
 * @index: index to start to parse
 * @busno: Returns busno if supported. Otherwise pass NULL
 * @addr: Returns address that DMA starts
 * @size: Returns the range that DMA can handle
 *
 * This supports different formats flexibly. "prefix" can be
 * configured if any. "busno" and "index" are optionally
 * specified. Set 0(or NULL) if not used.
 */
int of_get_dma_window(struct device_node *dn, const char *prefix, int index,
		      unsigned long *busno, dma_addr_t *addr, size_t *size)
{
	const __be32 *dma_window, *end;
	int bytes, cur_index = 0;
	char propname[NAME_MAX], addrname[NAME_MAX], sizename[NAME_MAX];

	if (!dn || !addr || !size)
		return -EINVAL;

	if (!prefix)
		prefix = "";

	snprintf(propname, sizeof(propname), "%sdma-window", prefix);
	snprintf(addrname, sizeof(addrname), "%s#dma-address-cells", prefix);
	snprintf(sizename, sizeof(sizename), "%s#dma-size-cells", prefix);

	dma_window = of_get_property(dn, propname, &bytes);
	if (!dma_window)
		return -ENODEV;
	end = dma_window + bytes / sizeof(*dma_window);

	while (dma_window < end) {
		u32 cells;
		const void *prop;

		/* busno is one cell if supported */
		if (busno)
			*busno = be32_to_cpup(dma_window++);

		prop = of_get_property(dn, addrname, NULL);
		if (!prop)
			prop = of_get_property(dn, "#address-cells", NULL);

		cells = prop ? be32_to_cpup(prop) : of_n_addr_cells(dn);
		if (!cells)
			return -EINVAL;
		*addr = of_read_number(dma_window, cells);
		dma_window += cells;

		prop = of_get_property(dn, sizename, NULL);
		cells = prop ? be32_to_cpup(prop) : of_n_size_cells(dn);
		if (!cells)
			return -EINVAL;
		*size = of_read_number(dma_window, cells);
		dma_window += cells;

		if (cur_index++ == index)
			break;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(of_get_dma_window);

struct iommu_dma_mapping *of_iommu_configure(struct device *dev)
{
	struct of_phandle_args iommu_spec;
	struct iommu_dma_mapping *mapping;
	struct device_node *np;
	struct iommu_data *iommu = NULL;
	int idx = 0;

	/*
	 * We don't currently walk up the tree looking for a parent IOMMU.
	 * See the `Notes:' section of
	 * Documentation/devicetree/bindings/iommu/iommu.txt
	 */
	while (!of_parse_phandle_with_args(dev->of_node, "iommus",
					   "#iommu-cells", idx,
					   &iommu_spec)) {
		struct iommu_data *data;

		np = iommu_spec.np;
		data = of_iommu_get_data(np);

		if (!data || !data->ops || !data->ops->of_xlate)
			goto err_put_node;

		if (!iommu) {
			iommu = data;
		} else if (iommu != data) {
			/* We don't currently support multi-IOMMU masters */
			pr_warn("Rejecting device %s with multiple IOMMU instances\n",
				dev_name(dev));
			goto err_put_node;
		}

		if (!data->ops->of_xlate(dev, &iommu_spec))
			goto err_put_node;

		of_node_put(np);
		idx++;
	}

	if (!iommu)
		return NULL;

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return NULL;

	kref_init(&mapping->kref);
	INIT_LIST_HEAD(&mapping->node);
	mapping->iommu = iommu;
	return mapping;

err_put_node:
	of_node_put(np);
	return NULL;
}

void of_iommu_deconfigure(struct kref *kref)
{
	struct iommu_dma_mapping *mapping, *curr, *next;

	mapping = container_of(kref, struct iommu_dma_mapping, kref);
	list_for_each_entry_safe(curr, next, &mapping->node, node) {
		list_del(&curr->node);
		kfree(curr);
	}
}

void __init of_iommu_init(void)
{
	struct device_node *np;
	const struct of_device_id *match, *matches = &__iommu_of_table;

	for_each_matching_node_and_match(np, matches, &match) {
		const of_iommu_init_fn init_fn = match->data;

		if (init_fn(np))
			pr_err("Failed to initialise IOMMU %s\n",
				of_node_full_name(np));
	}
}
