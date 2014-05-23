/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Thomas Abraham <thomas.ab@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This file contains the utility functions to register the CPU clocks
 * for Samsung platforms.
*/

#include <linux/errno.h>
#include "clk.h"

#define SRC_CPU			0x0
#define STAT_CPU		0x200
#define DIV_CPU0		0x300
#define DIV_CPU1		0x304
#define DIV_STAT_CPU0		0x400
#define DIV_STAT_CPU1		0x404

#define DIV_CPU0_RATIO0_MASK	0x7

#define MAX_DIV			8

#define EXYNOS4210_ARM_DIV1(div) ((div & 0x7) + 1)
#define EXYNOS4210_ARM_DIV2(div) (((div >> 28) & 0x7) + 1)

#define EXYNOS4210_DIV_CPU0(d5, d4, d3, d2, d1, d0)			\
		(((d5) << 24) | ((d4) << 20) | ((d3) << 16) | ((d2) << 12) | \
		 ((d1) << 8) | ((d0) <<  4))
#define EXYNOS4210_DIV_CPU1(d2, d1, d0)					\
		(((d2) << 8) | ((d1) << 4) | ((d0) << 0))

#define EXYNOS4210_DIV1_HPM_MASK	((0x7 << 0) | (0x7 << 4))
#define EXYNOS4210_MUX_HPM_MASK		(1 << 20)

/**
 * struct exynos4210_armclk_data: config data to setup exynos4210 cpu clocks.
 * @prate:	frequency of the parent clock.
 * @div0:	value to be programmed in the div_cpu0 register.
 * @div1:	value to be programmed in the div_cpu1 register.
 *
 * This structure holds the divider configuration data for divider clocks
 * belonging to the CMU_CPU clock domain. The parent frequency at which these
 * divider values are valid is specified in @prate.
 */
struct exynos4210_armclk_data {
	unsigned long	prate;
	unsigned int	div0;
	unsigned int	div1;
};

/**
 * struct exynos_cpuclk: information about clock supplied to a CPU core.
 * @hw:		handle between CCF and CPU clock.
 * @alt_parent:	alternate parent clock to use when switching the speed
 *		of the primary parent clock.
 * @ctrl_base:	base address of the clock controller.
 * @offset:	offset from the ctrl_base address where the CPU clock div/mux
 *		registers can be accessed.
 * @clk_nb:	clock notifier registered for changes in clock speed of the
 *		primary parent clock.
 * @lock:	register access lock.
 * @data:	optional data which the actual instantiation of this clock
 *		can use.
 */
struct exynos_cpuclk {
	struct clk_hw		hw;
	struct clk		*alt_parent;
	void __iomem		*ctrl_base;
	unsigned long		offset;
	struct notifier_block	clk_nb;
	spinlock_t		*lock;
	void			*data;
};

#define to_exynos_cpuclk_hw(hw) container_of(hw, struct exynos_cpuclk, hw)
#define to_exynos_cpuclk_nb(nb) container_of(nb, struct exynos_cpuclk, clk_nb)

/**
 * struct exynos_cpuclk_soc_data: soc specific data for cpu clocks.
 * @parser:	pointer to a function that can parse SoC specific data.
 * @ops:	clock operations to be used for this clock.
 * @offset:	optional offset from base of clock controller register base, to
 *		be used when accessing clock controller registers related to the
 *		CPU clock.
 * @clk_cb:	the clock notifier callback to be called for changes in the
 *		clock rate of the primary parent clock.
 *
 * This structure provides SoC specific data for ARM clocks. Based on
 * the compatible value of the clock controller node, the value of the
 * fields in this structure can be populated.
 */
struct exynos_cpuclk_soc_data {
	int (*parser)(struct device_node *, void **);
	const struct clk_ops *ops;
	unsigned int offset;
	int (*clk_cb)(struct notifier_block *, unsigned long, void *);
};

/* common round rate callback useable for all types of CPU clocks */
static long exynos_cpuclk_round_rate(struct clk_hw *hw,
			unsigned long drate, unsigned long *prate)
{
	struct clk *parent = __clk_get_parent(hw->clk);
	*prate = __clk_round_rate(parent, drate);
	return *prate;
}

/* common recalc rate callback useable for all types of CPU clocks */
static unsigned long exynos_cpuclk_recalc_rate(struct clk_hw *hw,
			unsigned long parent_rate)
{
	return parent_rate;
}

/*
 * Calculates the divider value to be set for deriving drate from prate.
 * Divider value is actual divider value - 1.
 */
static unsigned long _calc_div(unsigned long prate, unsigned long drate)
{
	unsigned long div = DIV_ROUND_UP(prate, drate) - 1;

	WARN_ON(div >= MAX_DIV);
	return div;
}

/* helper function to register a cpu clock */
static int __init exynos_cpuclk_register(struct samsung_clk_provider *ctx,
		unsigned int lookup_id, const char *name, const char *parent,
		const char *alt_parent, struct device_node *np,
		const struct exynos_cpuclk_soc_data *soc_data)
{
	struct exynos_cpuclk *cpuclk;
	struct clk_init_data init;
	struct clk *clk;
	int ret;

	cpuclk = kzalloc(sizeof(*cpuclk), GFP_KERNEL);
	if (!cpuclk) {
		pr_err("%s: could not allocate memory for %s clock\n",
					__func__, name);
		return -ENOMEM;
	}

	init.name = name;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = &parent;
	init.num_parents = 1;

	cpuclk->hw.init = &init;
	cpuclk->ctrl_base = ctx->reg_base;
	cpuclk->lock = &ctx->lock;

	ret = soc_data->parser(np, &cpuclk->data);
	if (ret) {
		pr_err("%s: error %d in parsing %s clock data",
				__func__, ret, name);
		ret = -EINVAL;
		goto free_cpuclk;
	}
	cpuclk->offset = soc_data->offset;
	init.ops = soc_data->ops;

	cpuclk->clk_nb.notifier_call = soc_data->clk_cb;
	if (clk_notifier_register(__clk_lookup(parent), &cpuclk->clk_nb)) {
		pr_err("%s: failed to register clock notifier for %s\n",
				__func__, name);
		goto free_cpuclk_data;
	}

	cpuclk->alt_parent = __clk_lookup(alt_parent);
	if (!cpuclk->alt_parent) {
		pr_err("%s: could not lookup alternate parent %s\n",
				__func__, alt_parent);
		ret = -EINVAL;
		goto unregister_clk_nb;
	}

	clk = clk_register(NULL, &cpuclk->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: could not register cpuclk %s\n", __func__,	name);
		ret = PTR_ERR(clk);
		goto unregister_clk_nb;
	}

	samsung_clk_add_lookup(ctx, clk, lookup_id);
	return 0;

unregister_clk_nb:
	clk_notifier_unregister(__clk_lookup(parent), &cpuclk->clk_nb);
free_cpuclk_data:
	kfree(cpuclk->data);
free_cpuclk:
	kfree(cpuclk);
	return ret;
}

static void exynos4210_set_armclk_div(void __iomem *base, unsigned long div)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(10);
	unsigned long div0;

	WARN_ON(div >= MAX_DIV);

	div0 = readl(base + DIV_CPU0);
	div0 = (div0 & ~DIV_CPU0_RATIO0_MASK) | div;
	writel(div0, base + DIV_CPU0);
	while (time_before(jiffies, timeout))
		if (!readl(base + DIV_STAT_CPU0))
			return;
	pr_err("%s: timeout in divider stablization\n", __func__);
}

static int exynos4210_armclk_pre_rate_change(struct clk_notifier_data *ndata,
			struct exynos_cpuclk *armclk, void __iomem *base)
{
	struct exynos4210_armclk_data *armclk_data = armclk->data;
	unsigned long alt_prate = clk_get_rate(armclk->alt_parent);
	unsigned long alt_div = 0, div0, div1, tdiv0, mux_reg;
	unsigned long timeout, flags;

	/* find out the divider values to use for clock data */
	while (armclk_data->prate != ndata->new_rate) {
		if (armclk_data->prate == 0)
			return -EINVAL;
		armclk_data++;
	}

	/* For the selected PLL clock frequency, get the pre-defined divider
	 * values. If the clock for sclk_hpm is not sourced from apll, then
	 * the values for DIV_COPY and DIV_HPM dividers need not be set.
	 */
	div0 = armclk_data->div0;
	div1 = armclk_data->div1;
	if (readl(base + SRC_CPU) & EXYNOS4210_MUX_HPM_MASK) {
		div1 = readl(base + DIV_CPU1) & EXYNOS4210_DIV1_HPM_MASK;
		div1 |= ((armclk_data->div1) & ~EXYNOS4210_DIV1_HPM_MASK);
	}

	spin_lock_irqsave(armclk->lock, flags);

	/*
	 * if the new and old parent clock speed is less than the clock speed
	 * of the alternate parent, then it should be ensured that at no point
	 * the armclk speed is more than the old_prate until the dividers are
	 * set.
	 */
	tdiv0 = readl(base + DIV_CPU0);
	if (alt_prate > ndata->old_rate) {
		alt_div = _calc_div(alt_prate, ndata->old_rate);
		exynos4210_set_armclk_div(base, alt_div);
		div0 |= alt_div;
	}

	/* select sclk_mpll as the alternate parent */
	mux_reg = readl(base + SRC_CPU);
	writel(mux_reg | (1 << 16), base + SRC_CPU);

	timeout = jiffies + msecs_to_jiffies(10);
	while (time_before(jiffies, timeout))
		if (((readl(base + STAT_CPU) >> 16) & 0x7) == 2)
			break;

	if (((readl(base + STAT_CPU) >> 16) & 0x7) != 2)
		pr_err("%s: re-parenting to sclk_mpll failed\n", __func__);

	/* alternate parent is active now. set the dividers */
	writel(div0, base + DIV_CPU0);
	timeout = jiffies + msecs_to_jiffies(10);
	while (time_before(jiffies, timeout))
		if (!readl(base + DIV_STAT_CPU0))
			break;

	if (readl(base + DIV_STAT_CPU0))
		pr_err("%s: timeout in divider0 stablization\n", __func__);

	writel(div1, base + DIV_CPU1);
	timeout = jiffies + msecs_to_jiffies(10);
	while (time_before(jiffies, timeout))
		if (!readl(base + DIV_STAT_CPU1))
			break;
	if (readl(base + DIV_STAT_CPU1))
		pr_err("%s: timeout in divider1 stablization\n", __func__);

	spin_unlock_irqrestore(armclk->lock, flags);
	return 0;
}

static int exynos4210_armclk_post_rate_change(struct exynos_cpuclk *armclk,
			void __iomem *base)
{
	unsigned long mux_reg, flags;
	unsigned long timeout = jiffies + msecs_to_jiffies(10);

	spin_lock_irqsave(armclk->lock, flags);

	mux_reg = readl(base + SRC_CPU);
	writel(mux_reg & ~(1 << 16), base + SRC_CPU);
	while (time_before(jiffies, timeout))
		if (((readl(base + STAT_CPU) >> 16) & 0x7) == 1)
			break;
	if (((readl(base + STAT_CPU) >> 16) & 0x7) != 1)
		pr_err("%s: re-parenting to mout_apll failed\n", __func__);

	spin_unlock_irqrestore(armclk->lock, flags);
	return 0;
}

/*
 * This clock notifier is called when the frequency of the parent clock
 * of armclk is to be changed. This notifier handles the setting up all
 * the divider clocks, remux to temporary parent and handling the safe
 * frequency levels when using temporary parent.
 */
static int exynos4210_armclk_notifier_cb(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct clk_notifier_data *ndata = data;
	struct exynos_cpuclk *armclk = to_exynos_cpuclk_nb(nb);
	void __iomem *base =  armclk->ctrl_base + armclk->offset;
	int err = 0;

	if (event == PRE_RATE_CHANGE)
		err = exynos4210_armclk_pre_rate_change(ndata, armclk, base);
	else if (event == POST_RATE_CHANGE)
		err = exynos4210_armclk_post_rate_change(armclk, base);

	return notifier_from_errno(err);
}

static int exynos4210_armclk_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long prate)
{
	struct exynos_cpuclk *armclk = to_exynos_cpuclk_hw(hw);
	void __iomem *base = armclk->ctrl_base + armclk->offset;
	unsigned long flags;

	spin_lock_irqsave(armclk->lock, flags);
	exynos4210_set_armclk_div(base, 0);
	spin_unlock_irqrestore(armclk->lock, flags);
	return 0;
}

static const struct clk_ops exynos4210_armclk_clk_ops = {
	.recalc_rate = exynos_cpuclk_recalc_rate,
	.round_rate = exynos_cpuclk_round_rate,
	.set_rate = exynos4210_armclk_set_rate,
};

/*
 * parse divider configuration data from dt for all the cpu clock domain
 * clocks in exynos4210 and compatible SoC's.
 */
static int __init exynos4210_armclk_parser(struct device_node *np, void **data)
{
	struct exynos4210_armclk_data *tdata;
	u32 cfg[10], num_rows, row, col;
	struct property *prop;
	const __be32 *ptr = NULL;
	u32 cells;
	int len;

	if (of_property_read_u32(np, "samsung,armclk-cells", &cells))
		return -EINVAL;
	prop = of_find_property(np, "samsung,armclk-divider-table", &len);
	if (!prop)
		return -EINVAL;
	if ((len / sizeof(u32)) % cells)
		return -EINVAL;
	num_rows = (len / sizeof(u32)) / cells;

	/* allocate a zero terminated table */
	*data = kzalloc(sizeof(*tdata) * (num_rows + 1), GFP_KERNEL);
	if (!*data)
		return -ENOMEM;
	tdata = *data;

	for (row = 0; row < num_rows; row++, tdata++) {
		for (col = 0; col < cells; col++)
			ptr = of_prop_next_u32(prop, ptr, &cfg[col]);

		tdata->prate = cfg[0] * 1000;
		tdata->div0 = EXYNOS4210_DIV_CPU0(cfg[6], cfg[5], cfg[4],
						cfg[3], cfg[2], cfg[1]);
		tdata->div1 = cells == 10 ?
				EXYNOS4210_DIV_CPU1(cfg[9], cfg[8], cfg[7]) :
				EXYNOS4210_DIV_CPU1(0, cfg[8], cfg[7]);
	}
	tdata->prate = 0;
	return 0;
}

static const struct exynos_cpuclk_soc_data exynos4210_cpuclk_soc_data = {
	.parser = exynos4210_armclk_parser,
	.ops = &exynos4210_armclk_clk_ops,
	.offset = 0x14200,
	.clk_cb = exynos4210_armclk_notifier_cb,
};

static const struct exynos_cpuclk_soc_data exynos5250_cpuclk_soc_data = {
	.parser = exynos4210_armclk_parser,
	.ops = &exynos4210_armclk_clk_ops,
	.offset = 0x200,
	.clk_cb = exynos4210_armclk_notifier_cb,
};

static const struct of_device_id exynos_clock_ids_armclk[] = {
	{ .compatible = "samsung,exynos4210-clock",
			.data = &exynos4210_cpuclk_soc_data, },
	{ .compatible = "samsung,exynos4412-clock",
			.data = &exynos4210_cpuclk_soc_data, },
	{ .compatible = "samsung,exynos5250-clock",
			.data = &exynos5250_cpuclk_soc_data, },
	{ },
};

/**
 * exynos_register_arm_clock: register arm clock with ccf.
 * @lookup_id: armclk clock output id for the clock controller.
 * @parent_names: name of the parent clock for armclk.
 * @num_parents: number of parents in the @parent_names array.
 * @base: base address of the clock controller from which armclk is generated.
 * @np: device tree node pointer of the clock controller (optional).
 * @ops: clock ops for this clock (optional).
 * @lock: register access lock.
 */
int __init exynos_register_arm_clock(struct samsung_clk_provider *ctx,
		unsigned int lookup_id,	const char *parent,
		const char *alt_parent, struct device_node *np)
{
	const struct of_device_id *match;
	const struct exynos_cpuclk_soc_data *data = NULL;

	if (!np)
		return -EINVAL;

	match = of_match_node(exynos_clock_ids_armclk, np);
	if (!match)
		return -EINVAL;

	data = match->data;
	return exynos_cpuclk_register(ctx, lookup_id, "armclk", parent,
			alt_parent, np, data);
}
