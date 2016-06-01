/*
 * Copyright (C) 2014 Linaro Ltd
 * Copyright (C) 2016 Samsung Electronics
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *         Krzysztof Kozlowski <k.kozlowski@samsung.com>
 *
 * License terms: GNU General Public License (GPL) version 2
 *
 *  MMC power sequence management
 */
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pwrseq.h>

#include <linux/mmc/host.h>

static DEFINE_MUTEX(pwrseq_list_mutex);
static LIST_HEAD(pwrseq_list);

int mmc_pwrseq_alloc(struct mmc_host *host)
{
	struct device_node *np;
	struct pwrseq *p;

	np = of_parse_phandle(host->parent->of_node, "mmc-pwrseq", 0);
	if (!np)
		return 0;

	mutex_lock(&pwrseq_list_mutex);
	list_for_each_entry(p, &pwrseq_list, pwrseq_node) {
		if (p->dev->of_node == np) {
			if (!try_module_get(p->owner))
				dev_err(host->parent,
					"increasing module refcount failed\n");
			else
				host->pwrseq = p;

			break;
		}
	}

	of_node_put(np);
	mutex_unlock(&pwrseq_list_mutex);

	if (!host->pwrseq)
		return -EPROBE_DEFER;

	dev_info(host->parent, "allocated mmc-pwrseq\n");

	return 0;
}
EXPORT_SYMBOL_GPL(mmc_pwrseq_alloc);

struct pwrseq *pwrseq_alloc(struct device *dev, const char *phandle_name)
{
	struct device_node *np;
	struct pwrseq *p, *ret = NULL;

	np = of_parse_phandle(dev->of_node, phandle_name, 0);
	if (!np)
		return NULL;

	mutex_lock(&pwrseq_list_mutex);
	list_for_each_entry(p, &pwrseq_list, pwrseq_node) {
		if (p->dev->of_node == np) {
			if (!try_module_get(p->owner))
				dev_err(dev,
					"increasing module refcount failed\n");
			else
				ret = p;

			break;
		}
	}

	of_node_put(np);
	mutex_unlock(&pwrseq_list_mutex);

	if (!ret) {
		dev_dbg(dev, "%s defer probe\n", phandle_name);
		return ERR_PTR(-EPROBE_DEFER);
	}

	dev_info(dev, "allocated usb-pwrseq\n");

	return ret;
}
EXPORT_SYMBOL_GPL(pwrseq_alloc);

void pwrseq_pre_power_on(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->pre_power_on)
		pwrseq->ops->pre_power_on(pwrseq);
}
EXPORT_SYMBOL_GPL(pwrseq_pre_power_on);

void pwrseq_post_power_on(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->post_power_on)
		pwrseq->ops->post_power_on(pwrseq);
}
EXPORT_SYMBOL_GPL(pwrseq_post_power_on);

void pwrseq_power_off(struct pwrseq *pwrseq)
{
	if (pwrseq && pwrseq->ops->power_off)
		pwrseq->ops->power_off(pwrseq);
}
EXPORT_SYMBOL_GPL(pwrseq_power_off);

void mmc_pwrseq_free(struct mmc_host *host)
{
	struct pwrseq *pwrseq = host->pwrseq;

	if (pwrseq) {
		module_put(pwrseq->owner);
		host->pwrseq = NULL;
	}
}
EXPORT_SYMBOL_GPL(mmc_pwrseq_free);

void pwrseq_free(const struct pwrseq *pwrseq)
{
	if (pwrseq)
		module_put(pwrseq->owner);
}
EXPORT_SYMBOL_GPL(pwrseq_free);

int pwrseq_register(struct pwrseq *pwrseq)
{
	if (!pwrseq || !pwrseq->ops || !pwrseq->dev)
		return -EINVAL;

	mutex_lock(&pwrseq_list_mutex);
	list_add(&pwrseq->pwrseq_node, &pwrseq_list);
	mutex_unlock(&pwrseq_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(pwrseq_register);

void pwrseq_unregister(struct pwrseq *pwrseq)
{
	if (pwrseq) {
		mutex_lock(&pwrseq_list_mutex);
		list_del(&pwrseq->pwrseq_node);
		mutex_unlock(&pwrseq_list_mutex);
	}
}
EXPORT_SYMBOL_GPL(pwrseq_unregister);
