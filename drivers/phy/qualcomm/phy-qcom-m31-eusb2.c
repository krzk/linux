// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define USB_PHY_UTMI_CTRL0		(0x3c)

#define USB_PHY_UTMI_CTRL5		(0x50)

#define USB_PHY_HS_PHY_CTRL_COMMON0	(0x54)
#define FSEL				(0x7 << 4)
#define FSEL_38_4_MHZ_VAL		(0x6 << 4)

#define USB_PHY_HS_PHY_CTRL2		(0x64)

#define USB_PHY_CFG0			(0x94)
#define USB_PHY_CFG1			(0x154)

#define USB_PHY_FSEL_SEL		(0xb8)

#define USB_PHY_XCFGI_39_32		(0x16c)
#define USB_PHY_XCFGI_71_64		(0x17c)
#define USB_PHY_XCFGI_31_24		(0x168)
#define USB_PHY_XCFGI_7_0		(0x15c)

#define M31_EUSB_PHY_INIT_CFG(o, b, v)	\
{				\
	.off = o,		\
	.mask = b,		\
	.val = v,		\
}

struct m31_phy_tbl_entry {
	u32 off;
	u32 mask;
	u32 val;
};

struct m31_eusb2_priv_data {
	const struct m31_phy_tbl_entry	*setup_seq;
	unsigned int			setup_seq_nregs;
	const struct m31_phy_tbl_entry	*override_seq;
	unsigned int			override_seq_nregs;
	const struct m31_phy_tbl_entry	*reset_seq;
	unsigned int			reset_seq_nregs;
	unsigned int			fsel;
};

static const struct m31_phy_tbl_entry m31_eusb2_setup_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG0, BIT(1), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL5, BIT(1), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG1, BIT(0), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_FSEL_SEL, BIT(0), 1),
};

static const struct m31_phy_tbl_entry m31_eusb_phy_override_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_39_32, GENMASK(3, 2), 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_71_64, GENMASK(3, 0), 7),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_31_24, GENMASK(2, 0), 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_XCFGI_7_0, GENMASK(1, 0), 0),
};

static const struct m31_phy_tbl_entry m31_eusb_phy_reset_tbl[] = {
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, BIT(3), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, BIT(2), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL0, BIT(0), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL_COMMON0, BIT(1), 1),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL_COMMON0, BIT(2), 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_UTMI_CTRL5, BIT(1), 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_HS_PHY_CTRL2, BIT(3), 0),
	M31_EUSB_PHY_INIT_CFG(USB_PHY_CFG0, BIT(1), 0),
};

struct m31eusb2_phy {
	struct phy			*phy;
	void __iomem			*base;
	const struct m31_eusb2_priv_data	*data;

	struct regulator		*vreg;
	struct clk			*clk;
	struct reset_control		*reset;

	struct phy *repeater;
};

static void msm_m31_eusb2_write_readback(void __iomem *base, u32 offset,
					const u32 mask, u32 val)
{
	u32 write_val, tmp = readl_relaxed(base + offset);

	tmp &= ~mask;
	write_val = tmp | val;

	writel_relaxed(write_val, base + offset);

	tmp = readl_relaxed(base + offset);
	tmp &= mask;

	if (tmp != val)
		pr_err("write: %x to offset: %x FAILED\n", val, offset);
}

static void m31eusb2_phy_write_sequence(struct m31eusb2_phy *phy,
					const struct m31_phy_tbl_entry *tbl,
					int num)
{
	int i;

	for (i = 0 ; i < num; i++, tbl++) {
		dev_dbg(&phy->phy->dev, "Offset:%x BitMask:%x Value:%x",
					tbl->off, tbl->mask, tbl->val);

		msm_m31_eusb2_write_readback(phy->base,
					tbl->off, tbl->mask,
					tbl->val << __ffs(tbl->mask));
	}
}

static int m31eusb2_phy_init(struct phy *uphy)
{
	struct m31eusb2_phy *phy = phy_get_drvdata(uphy);
	const struct m31_eusb2_priv_data *data = phy->data;
	int ret;

	ret = regulator_enable(phy->vreg);
	if (ret) {
		dev_err(&uphy->dev, "failed to enable regulator, %d\n", ret);
		return ret;
	}

	ret = phy_init(phy->repeater);
	if (ret) {
		dev_err(&uphy->dev, "repeater init failed. %d\n", ret);
		goto disable_vreg;
	}

	if (ret) {
		dev_err(&uphy->dev, "failed to enable cfg ahb clock, %d\n", ret);
		goto disable_repeater;
	}

	/* Perform phy reset */
	reset_control_assert(phy->reset);
	udelay(5);
	reset_control_deassert(phy->reset);

	m31eusb2_phy_write_sequence(phy, data->setup_seq, data->setup_seq_nregs);
	msm_m31_eusb2_write_readback(phy->base,
					USB_PHY_HS_PHY_CTRL_COMMON0, FSEL,
					data->fsel);
	m31eusb2_phy_write_sequence(phy, data->override_seq, data->override_seq_nregs);
	m31eusb2_phy_write_sequence(phy, data->reset_seq, data->reset_seq_nregs);

	return 0;

disable_repeater:
	phy_exit(phy->repeater);
disable_vreg:
	regulator_disable(phy->vreg);

	return 0;
}

static int m31eusb2_phy_exit(struct phy *uphy)
{
	struct m31eusb2_phy *phy = phy_get_drvdata(uphy);

	regulator_disable(phy->vreg);
	phy_exit(phy->repeater);

	return 0;
}

static const struct phy_ops m31eusb2_phy_gen_ops = {
	.init	= m31eusb2_phy_init,
	.exit	= m31eusb2_phy_exit,
	.owner		= THIS_MODULE,
};

static int m31eusb2_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	const struct m31_eusb2_priv_data *data;
	struct device *dev = &pdev->dev;
	struct m31eusb2_phy *phy;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	data = of_device_get_match_data(dev);
	if (IS_ERR(data))
		return -EINVAL;
	phy->data = data;

	phy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);

	phy->reset = devm_reset_control_get_exclusive_by_index(dev, 0);
	if (IS_ERR(phy->reset))
		return PTR_ERR(phy->reset);

	phy->clk = devm_clk_get(dev, NULL);

	phy->phy = devm_phy_create(dev, NULL, &m31eusb2_phy_gen_ops);
	if (IS_ERR(phy->phy))
		return dev_err_probe(dev, PTR_ERR(phy->phy),
				     "failed to create phy\n");

	phy->vreg = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vreg))
		return dev_err_probe(dev, PTR_ERR(phy->vreg),
				     "failed to get vreg\n");

	phy_set_drvdata(phy->phy, phy);

	phy->repeater = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(phy->repeater))
		return dev_err_probe(dev, PTR_ERR(phy->repeater),
				     "failed to get repeater\n");

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (!IS_ERR(phy_provider))
		dev_info(dev, "Registered M31 USB phy\n");

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct m31_eusb2_priv_data m31_eusb_v1_data = {
	.setup_seq = m31_eusb2_setup_tbl,
	.setup_seq_nregs = ARRAY_SIZE(m31_eusb2_setup_tbl),
	.override_seq = m31_eusb_phy_override_tbl,
	.override_seq_nregs = ARRAY_SIZE(m31_eusb_phy_override_tbl),
	.reset_seq = m31_eusb_phy_reset_tbl,
	.reset_seq_nregs = ARRAY_SIZE(m31_eusb_phy_reset_tbl),
	.fsel = FSEL_38_4_MHZ_VAL,
};

static const struct of_device_id m31eusb2_phy_id_table[] = {
	{ .compatible = "qcom,sm8750-m31-eusb2-phy", .data = &m31_eusb_v1_data },
	{ },
};
MODULE_DEVICE_TABLE(of, m31eusb2_phy_id_table);

static struct platform_driver m31eusb2_phy_driver = {
	.probe = m31eusb2_phy_probe,
	.driver = {
		.name = "qcom-m31eusb2-phy",
		.of_match_table = m31eusb2_phy_id_table,
	},
};

module_platform_driver(m31eusb2_phy_driver);

MODULE_DESCRIPTION("eUSB2 Qualcomm M31 HSPHY driver");
MODULE_LICENSE("GPL");
