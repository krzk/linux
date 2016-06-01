/*
 * Copyright (C) 2014 Linaro Ltd
 * Copyright (C) 2016 Samsung Electronics
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *         Krzysztof Kozlowski <k.kozlowski@samsung.com>
 *
 * License terms: GNU General Public License (GPL) version 2
 *
 *  Simple MMC power sequence management
 */
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/of_regulator.h>
#include <linux/pwrseq.h>
#include <linux/delay.h>

static LIST_HEAD(mmc_pwrseq_devs);

struct mmc_pwrseq_dev {
	struct platform_device *pdev;
	struct list_head entry;
};

struct mmc_pwrseq_simple {
	struct pwrseq pwrseq;
	bool clk_enabled;
	struct clk *ext_clk;
	struct gpio_descs *reset_gpios;
	unsigned int regulator_count;
	struct regulator_bulk_data *regulators;
};

#define to_pwrseq_simple(p) container_of(p, struct mmc_pwrseq_simple, pwrseq)

static void mmc_pwrseq_simple_set_gpios_value(struct mmc_pwrseq_simple *pwrseq,
					      int value)
{
	struct gpio_descs *reset_gpios = pwrseq->reset_gpios;

	if (!IS_ERR(reset_gpios)) {
		int i;
		int values[reset_gpios->ndescs];

		for (i = 0; i < reset_gpios->ndescs; i++)
			values[i] = value;

		gpiod_set_array_value_cansleep(
			reset_gpios->ndescs, reset_gpios->desc, values);
	}
}

static void mmc_pwrseq_simple_pre_power_on(struct pwrseq *_pwrseq)
{
	struct mmc_pwrseq_simple *pwrseq = to_pwrseq_simple(_pwrseq);

	if (!IS_ERR(pwrseq->ext_clk) && !pwrseq->clk_enabled) {
		clk_prepare_enable(pwrseq->ext_clk);
		pwrseq->clk_enabled = true;
	}

	mmc_pwrseq_simple_set_gpios_value(pwrseq, 1);
}

static void mmc_pwrseq_simple_post_power_on(struct pwrseq *_pwrseq)
{
	struct mmc_pwrseq_simple *pwrseq = to_pwrseq_simple(_pwrseq);

	if (pwrseq->regulators) {
		int err;

		err = regulator_bulk_enable(pwrseq->regulator_count,
					    pwrseq->regulators);
		WARN_ON_ONCE(err);
	}

	mmc_pwrseq_simple_set_gpios_value(pwrseq, 0);
}

static void mmc_pwrseq_simple_power_off(struct pwrseq *_pwrseq)
{
	struct mmc_pwrseq_simple *pwrseq = to_pwrseq_simple(_pwrseq);

	mmc_pwrseq_simple_set_gpios_value(pwrseq, 1);

	if (!IS_ERR(pwrseq->ext_clk) && pwrseq->clk_enabled) {
		clk_disable_unprepare(pwrseq->ext_clk);
		pwrseq->clk_enabled = false;
	}

	if (pwrseq->regulators) {
		int err;

		err = regulator_bulk_disable(pwrseq->regulator_count,
					     pwrseq->regulators);
		WARN_ON_ONCE(err);
	}
}

static const struct pwrseq_ops mmc_pwrseq_simple_ops = {
	.pre_power_on = mmc_pwrseq_simple_pre_power_on,
	.post_power_on = mmc_pwrseq_simple_post_power_on,
	.power_off = mmc_pwrseq_simple_power_off,
};

static const struct of_device_id mmc_pwrseq_simple_of_match[] = {
	{ .compatible = "mmc-pwrseq-simple",},
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, mmc_pwrseq_simple_of_match);

static int mmc_pwrseq_simple_probe(struct platform_device *pdev)
{
	struct mmc_pwrseq_simple *pwrseq;
	struct device *dev = &pdev->dev;
	int err;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->ext_clk = devm_clk_get(dev, "ext_clock");
	if (IS_ERR(pwrseq->ext_clk) && PTR_ERR(pwrseq->ext_clk) != -ENOENT)
		return PTR_ERR(pwrseq->ext_clk);

	err = devm_of_regulator_all_get(dev, &pwrseq->regulator_count,
					&pwrseq->regulators);
	if (err)
		return err;

	if (pwrseq->regulators) {
		/*
		 * Be sure that regulator is off, before the driver will start
		 * power sequence. It is likely that regulator is on by default
		 * and it without toggling it here, it would be disabled much
		 * later by the core.
		 */
		err = regulator_bulk_enable(pwrseq->regulator_count,
					    pwrseq->regulators);
		WARN_ON_ONCE(err);

		err = regulator_bulk_disable(pwrseq->regulator_count,
					     pwrseq->regulators);
		WARN_ON_ONCE(err);
	}

	pwrseq->reset_gpios = devm_gpiod_get_array(dev, "reset",
							GPIOD_OUT_HIGH);
	if (IS_ERR(pwrseq->reset_gpios) &&
	    PTR_ERR(pwrseq->reset_gpios) != -ENOENT &&
	    PTR_ERR(pwrseq->reset_gpios) != -ENOSYS) {
		/*
		 * Don't care about errors. If this pwrseq device was added
		 * to node with existing reset-gpios, then the GPIO reset will
		 * be handled by other device.
		 */
		dev_warn(dev, "Cannot get reset gpio: %ld\n",
			 PTR_ERR(pwrseq->reset_gpios));
	}

	pwrseq->pwrseq.dev = dev;
	pwrseq->pwrseq.ops = &mmc_pwrseq_simple_ops;
	pwrseq->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, pwrseq);

	return pwrseq_register(&pwrseq->pwrseq);
}

static int mmc_pwrseq_simple_remove(struct platform_device *pdev)
{
	struct mmc_pwrseq_simple *pwrseq = platform_get_drvdata(pdev);

	pwrseq_unregister(&pwrseq->pwrseq);

	if (pwrseq->regulators) {
		int err;

		err = regulator_bulk_disable(pwrseq->regulator_count,
					     pwrseq->regulators);
		WARN_ON_ONCE(err);
	}

	return 0;
}

static struct platform_driver mmc_pwrseq_simple_driver = {
	.probe = mmc_pwrseq_simple_probe,
	.remove = mmc_pwrseq_simple_remove,
	.driver = {
		.name = "pwrseq_simple",
		.of_match_table = mmc_pwrseq_simple_of_match,
	},
};

static int __init mmc_pwrseq_simple_driver_init(void)
{
	struct mmc_pwrseq_dev *pwrseq_dev;
	struct platform_device *pdev;
	struct device_node *np;

	for_each_node_with_property(np, "power-sequence") {
		pdev = platform_device_register_simple("pwrseq_simple",
						       PLATFORM_DEVID_AUTO,
						       NULL, 0);
		if (IS_ERR(pdev))
			continue;

		pwrseq_dev = kzalloc(sizeof(*pwrseq_dev), GFP_KERNEL);
		if (!pwrseq_dev)
			continue;

		of_node_get(np);
		pdev->dev.of_node = np;
		pwrseq_dev->pdev = pdev;
		list_add(&pwrseq_dev->entry, &mmc_pwrseq_devs);
	}

	return platform_driver_register(&mmc_pwrseq_simple_driver);
}
module_init(mmc_pwrseq_simple_driver_init);

static void __exit mmc_pwrseq_simple_driver_exit(void)
{
	struct mmc_pwrseq_dev *pwrseq_dev, *tmp;

	list_for_each_entry_safe(pwrseq_dev, tmp, &mmc_pwrseq_devs, entry) {
		list_del(&pwrseq_dev->entry);
		of_node_put(pwrseq_dev->pdev->dev.of_node);
		platform_device_unregister(pwrseq_dev->pdev);
		kfree(pwrseq_dev);
	}

	platform_driver_unregister(&mmc_pwrseq_simple_driver);
}
module_exit(mmc_pwrseq_simple_driver_exit);
MODULE_LICENSE("GPL v2");
