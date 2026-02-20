// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/*
modprobe psy_test
echo psy-test > /sys/bus/platform/drivers/psy-test/unbind
echo psy-test > /sys/bus/platform/drivers/psy-test/bind
*/

struct psy_test_data {
	struct device *dev;
	struct workqueue_struct *wq;
	struct work_struct work;
};

static struct platform_device *psy_test_dev;

static void psy_test_destroy_wq(void *data)
{
	struct psy_test_data *priv = data;

	dev_err(priv->dev, "%s:%d AAA Destroy start\n", __func__, __LINE__);
	destroy_workqueue(priv->wq);
	dev_err(priv->dev, "%s:%d AAA Destroy end\n", __func__, __LINE__);
}

static void psy_test_work(struct work_struct *work)
{
	struct psy_test_data *priv = container_of(work, struct psy_test_data, work);
	dev_err(priv->dev, "%s:%d Work sleep start\n", __func__, __LINE__);
	msleep(5000);
	dev_err(priv->dev, "%s:%d Work sleep end\n", __func__, __LINE__);
};

static int psy_test_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct psy_test_data *priv;
	// int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	priv->wq = create_singlethread_workqueue(dev_name(dev));
	if (!priv->wq)
		return -ENOMEM;

#if 0
	ret = devm_add_action_or_reset(dev, psy_test_destroy_wq, priv);
	if (ret)
		return ret;

	ret = devm_work_autocancel(dev, &priv->work, psy_test_work);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to init work\n");

#else
	INIT_WORK(&priv->work, psy_test_work);
#endif
	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);

	if (!queue_work(priv->wq, &priv->work))
		dev_err(dev, "work has already queued\n");

	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);

	if (!queue_work(priv->wq, &priv->work))
		dev_err(dev, "work has already queued\n");

	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);

	return 0;
}

static void psy_test_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct psy_test_data *priv = platform_get_drvdata(pdev);

	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);

	if (!queue_work(priv->wq, &priv->work))
		dev_err(dev, "work has already queued\n");

	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);
#if 1
	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);
	psy_test_destroy_wq(priv);
	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);
	if (!queue_work(priv->wq, &priv->work))
		dev_err(dev, "work has already queued\n");
	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);
	cancel_work_sync(&priv->work);
	dev_err(dev, "%s:%d AAA\n", __func__, __LINE__);
#endif
}

static struct platform_driver psy_test_driver = {
	.probe = psy_test_probe,
	.remove = psy_test_remove,
	.driver = {
		.name = "psy-test",
	},
};

static int __init psy_kunit_init(void)
{
	int ret;

	pr_err("%s:%d AAA\n", __func__, __LINE__);

	ret = platform_driver_register(&psy_test_driver);
	if (ret)
		return ret;

	psy_test_dev = platform_device_alloc("psy-test", -1);
	if (!psy_test_dev)
		return -ENOMEM;

	ret = platform_device_add(psy_test_dev);
	if (ret)
		platform_device_put(psy_test_dev);

	pr_err("%s:%d AAA\n", __func__, __LINE__);

	return ret;
}

static void __exit psy_kunit_exit(void)
{
	pr_err("%s:%d AAA\n", __func__, __LINE__);

	if (psy_test_dev)
		platform_device_unregister(psy_test_dev);

	platform_driver_unregister(&psy_test_driver);

	pr_err("%s:%d AAA\n", __func__, __LINE__);
}

module_init(psy_kunit_init);
module_exit(psy_kunit_exit);

MODULE_DESCRIPTION("Power supply class test unit");
MODULE_AUTHOR("Krzysztof Kozlowski");
MODULE_LICENSE("GPL v2");
