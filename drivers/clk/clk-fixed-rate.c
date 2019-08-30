/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Mike Turquette, Linaro Ltd <mturquette@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Fixed rate clock implementation
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/clkdev.h>

/*
 * DOC: basic fixed-rate clock that cannot gate
 *
 * Traits of this clock:
 * prepare - clk_(un)prepare only ensures parents are prepared
 * enable - clk_enable only ensures parents are enabled
 * rate - rate is always a fixed value.  No clk_set_rate support
 * parent - fixed parent.  No clk_set_parent support
 */

#define to_clk_fixed_rate(_hw) container_of(_hw, struct clk_fixed_rate, hw)

static unsigned long clk_fixed_rate_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	return to_clk_fixed_rate(hw)->fixed_rate;
}

const struct clk_ops clk_fixed_rate_ops = {
	.recalc_rate = clk_fixed_rate_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_fixed_rate_ops);

/**
 * clk_register_fixed_rate - register fixed-rate clock with the clock framework
 * @dev: device that is registering this clock
 * @name: name of this clock
 * @parent_name: name of clock's parent
 * @flags: framework-specific flags
 * @fixed_rate: non-adjustable clock rate
 */
struct clk *clk_register_fixed_rate(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags,
		unsigned long fixed_rate)
{
	struct clk_fixed_rate *fixed;
	struct clk *clk;
	struct clk_init_data init;

	/* allocate fixed-rate clock */
	fixed = kzalloc(sizeof(struct clk_fixed_rate), GFP_KERNEL);
	if (!fixed) {
		pr_err("%s: could not allocate fixed clk\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	init.name = name;
	init.ops = &clk_fixed_rate_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = (parent_name ? &parent_name: NULL);
	init.num_parents = (parent_name ? 1 : 0);

	/* struct clk_fixed_rate assignments */
	fixed->fixed_rate = fixed_rate;
	fixed->hw.init = &init;

	/* register the clock */
	clk = clk_register(dev, &fixed->hw);

	if (IS_ERR(clk))
		kfree(fixed);

	return clk;
}
EXPORT_SYMBOL_GPL(clk_register_fixed_rate);

#ifdef CONFIG_OF
/**
 * of_fixed_clk_setup() - Setup function for simple fixed rate clock
 */
void of_fixed_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	u32 rate;

	if (of_property_read_u32(node, "clock-frequency", &rate))
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);

	clk = clk_register_fixed_rate(NULL, clk_name, NULL, CLK_IS_ROOT, rate);
	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
}
EXPORT_SYMBOL_GPL(of_fixed_clk_setup);
CLK_OF_DECLARE(fixed_clk, "fixed-clock", of_fixed_clk_setup);
#endif

#ifdef CONFIG_ARM64
#ifdef CONFIG_ACPI
static int fixed_clk_probe_acpi(struct platform_device *pdev)
{
	struct clk *clk = ERR_PTR(-ENODEV);
	unsigned long long rate = 0;
	acpi_status status;

	/* there is a corresponding FREQ method under fixed clock object */
	status = acpi_evaluate_integer(ACPI_HANDLE(&pdev->dev), "FREQ",
					NULL, &rate);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	clk = clk_register_fixed_rate(NULL, dev_name(&pdev->dev), NULL,
					CLK_IS_ROOT, rate);
	if (IS_ERR(clk))
		return -ENODEV;

	/*
	 * if we don't register the clk here, we can't get the clk
	 * for AMBA bus when CONFIG_OF=n
	 */
	return clk_register_clkdev(clk, NULL, dev_name(&pdev->dev));
}
#else
static inline int fixed_clk_probe_acpi(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif /* CONFIG_ACPI */

static int fixed_clk_probe(struct platform_device *pdev)
{
	if (pdev->dev.of_node)
		of_fixed_clk_setup(pdev->dev.of_node);
	else if (ACPI_HANDLE(&pdev->dev))
		return fixed_clk_probe_acpi(pdev);
	else
		return -ENODEV;

	return 0;
}

static const struct of_device_id fixed_clk_match[] = {
	{ .compatible = "fixed-clock" },
	{}
};

static const struct acpi_device_id fixed_clk_acpi_match[] = {
	{ "LNRO0008", 0 },
	{ },
};

static struct platform_driver fixed_clk_driver = {
	.driver = {
		.name = "fixed-clk",
		.owner = THIS_MODULE,
		.of_match_table = fixed_clk_match,
		.acpi_match_table = ACPI_PTR(fixed_clk_acpi_match),
	},
	.probe	= fixed_clk_probe,
};

static int __init fixed_clk_init(void)
{
	return platform_driver_register(&fixed_clk_driver);
}

/**
 * fixed clock will used for AMBA bus, UART and etc, so it should be
 * initialized early enough.
 */
postcore_initcall(fixed_clk_init);
#endif /* CONFIG_ARM64 */
