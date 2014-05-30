/*
 * Generic OPP helper interface for CPUFreq drivers
 *
 * Copyright (C) 2009-2014 Texas Instruments Incorporated.
 *	Nishanth Menon
 *	Romit Dasgupta
 *	Kevin Hilman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/pm_opp.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/of.h>

/**
 * dev_pm_opp_init_cpufreq_table() - create a cpufreq table for a device
 * @dev:	device for which we do this operation
 * @table:	Cpufreq table returned back to caller
 *
 * Generate a cpufreq table for a provided device- this assumes that the
 * opp list is already initialized and ready for usage.
 *
 * This function allocates required memory for the cpufreq table. It is
 * expected that the caller does the required maintenance such as freeing
 * the table as required.
 *
 * Returns -EINVAL for bad pointers, -ENODEV if the device is not found, -ENOMEM
 * if no memory available for the operation (table is not populated), returns 0
 * if successful and table is populated.
 *
 * WARNING: It is  important for the callers to ensure refreshing their copy of
 * the table if any of the mentioned functions have been invoked in the interim.
 *
 * Locking: The internal device_opp and opp structures are RCU protected.
 * Since we just use the regular accessor functions to access the internal data
 * structures, we use RCU read lock inside this function. As a result, users of
 * this function DONOT need to use explicit locks for invoking.
 */
int dev_pm_opp_init_cpufreq_table(struct device *dev,
				  struct cpufreq_frequency_table **table)
{
	struct dev_pm_opp *opp;
	struct cpufreq_frequency_table *freq_table = NULL;
	int i, max_opps, ret = 0;
	unsigned long rate;
#ifdef CONFIG_CPU_FREQ_BOOST_SW
	struct cpufreq_frequency_table *ft;
	int len, count;
	u32 *boost_freqs = NULL;
#endif

	rcu_read_lock();

	max_opps = dev_pm_opp_get_opp_count(dev);
	if (max_opps <= 0) {
		ret = max_opps ? max_opps : -ENODATA;
		goto out;
	}

	freq_table = kcalloc((max_opps + 1), sizeof(*freq_table), GFP_ATOMIC);
	if (!freq_table) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0, rate = 0; i < max_opps; i++, rate++) {
		/* find next rate */
		opp = dev_pm_opp_find_freq_ceil(dev, &rate);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			goto out;
		}
		freq_table[i].driver_data = i;
		freq_table[i].frequency = rate / 1000;
	}

	freq_table[i].driver_data = i;
	freq_table[i].frequency = CPUFREQ_TABLE_END;

	*table = &freq_table[0];

#ifdef CONFIG_CPU_FREQ_BOOST_SW
	if (!of_find_property(dev->of_node, "boost-frequencies", &len))
		goto out;

	if (!len || !IS_ALIGNED(len, sizeof(u32))) {
		dev_err(dev, "%s: invalid boost frequency\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	boost_freqs = kmalloc(len, GFP_KERNEL);
	if (!boost_freqs) {
		dev_err(dev, "%s: no memory for boost freq table\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	count = len / sizeof(u32);
	of_property_read_u32_array(dev->of_node, "boost-frequencies",
				   boost_freqs, count);

	for (i = 0; i < count; i++) {
		cpufreq_for_each_valid_entry(ft, *table) {
			if (boost_freqs[i] == ft->frequency) {
				ft->flags |= CPUFREQ_BOOST_FREQ;
				pr_debug("%s: marked %d as boost frequency\n",
					 __func__, boost_freqs[i]);
				break;
			}
		}

		if (ft->frequency == CPUFREQ_TABLE_END)
			dev_err(dev, "%s: invalid boost frequency %d\n",
				__func__, boost_freqs[i]);
	}

	kfree(boost_freqs);
#endif

out:
	rcu_read_unlock();
	if (ret)
		kfree(freq_table);

	return ret;
}
EXPORT_SYMBOL_GPL(dev_pm_opp_init_cpufreq_table);

/**
 * dev_pm_opp_free_cpufreq_table() - free the cpufreq table
 * @dev:	device for which we do this operation
 * @table:	table to free
 *
 * Free up the table allocated by dev_pm_opp_init_cpufreq_table
 */
void dev_pm_opp_free_cpufreq_table(struct device *dev,
				   struct cpufreq_frequency_table **table)
{
	if (!table)
		return;

	kfree(*table);
	*table = NULL;
}
EXPORT_SYMBOL_GPL(dev_pm_opp_free_cpufreq_table);
