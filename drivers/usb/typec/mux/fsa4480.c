// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Linaro Ltd.
 * Copyright (C) 2018-2020 The Linux Foundation
 */

#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

#define FSA4480_SWITCH_ENABLE	0x04
#define FSA4480_SWITCH_SELECT	0x05
#define FSA4480_SWITCH_STATUS1	0x07
#define FSA4480_SLOW_L		0x08
#define FSA4480_SLOW_R		0x09
#define FSA4480_SLOW_MIC	0x0a
#define FSA4480_SLOW_SENSE	0x0b
#define FSA4480_SLOW_GND	0x0c
#define FSA4480_DELAY_L_R	0x0d
#define FSA4480_DELAY_L_MIC	0x0e
#define FSA4480_DELAY_L_SENSE	0x0f
#define FSA4480_DELAY_L_AGND	0x10
#define FSA4480_FUNCTION_ENABLE	0x12
#define FSA4480_RESET		0x1e
#define FSA4480_MAX_REGISTER	0x1f

#define FSA4480_ENABLE_DEVICE	BIT(7)
#define FSA4480_ENABLE_SBU	GENMASK(6, 5)
#define FSA4480_ENABLE_USB	GENMASK(4, 3)

#define FSA4480_SEL_SBU_REVERSE	GENMASK(6, 5)
#define FSA4480_SEL_USB		0

struct fsa4480 {
	struct i2c_client *client;

	/* used to serialize concurrent change requests */
	struct mutex lock;

	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;

	struct regmap *regmap;

	u8 cur_enable;
	u8 cur_select;
};

static const struct regmap_config fsa4480_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FSA4480_MAX_REGISTER,
	/* Accesses only done under fsa4480->lock */
	.disable_locking = true,
};

/*
 * Audio on:
 * enable 0x4: 0x9f, which is all except SBU1_H and SBU2_H
 * 1: Sense to GSBUx switches
 * 1: MIC to SBUx switches
 * 1: AGND to SBUx switches
 *
 * control 0x5: 0x0, which is
 * 0: DN_L to L switch ON
 * 0: DP_R to R switch ON
 * 0: Sense to GSBU1 switch ON
 * 0: MIC to SBU2 switch ON
 * 0: AGND to SBU1 switch ON
 *
 * Audio off:
 * enable 0x4: 0x98, which disables:
 * 0: Sense to GSBUx switches
 * 0: MIC to SBUx switches
 * 0: AGND to SBUx switches
 *
 * control 0x5: 0x18 which is:
 * 1: DN_L to DN switch ON
 * 1: DP_R to DP switch ON
 * 0: Sense to GSBU1 switch ON
 * 0: MIC to SBU2 switch ON
 * 0: AGND to SBU1 switch ON
 */
static void dump_regs(struct fsa4480 *fsa)
{
	int val = 0xffffff;
	int addr;

	addr = 0x6;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);
	addr = 0x7;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);

	regmap_read(fsa->regmap, 0x11, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x11, val);
	regmap_read(fsa->regmap, 0x12, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x12, val);
	regmap_read(fsa->regmap, 0x13, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x13, val);

	addr = 0x14;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);
	addr = 0x15;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);
	addr = 0x16;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);

	regmap_read(fsa->regmap, 0x17, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x17, val);


	addr = 0x18;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);
	addr = 0x19;
	regmap_read(fsa->regmap, addr, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n", addr, val);

	regmap_read(fsa->regmap, 0x1a, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x1a, val);
	regmap_read(fsa->regmap, 0x1b, &val);
	pr_err("AAAA fsa [0x%x]=0x%x\n",
	       0x1b, val);
}

static int fsa4480_switch_set(struct typec_switch_dev *sw,
			      enum typec_orientation orientation)
{
	struct fsa4480 *fsa = typec_switch_get_drvdata(sw);
	u8 new_sel;

	pr_err("AAAA fsa %s:%d orient %u\n", __func__, __LINE__,
	       orientation);

	dump_regs(fsa);
	//WARN_ON(1);

	mutex_lock(&fsa->lock);
	new_sel = FSA4480_SEL_USB;
	if (orientation == TYPEC_ORIENTATION_REVERSE)
		new_sel |= FSA4480_SEL_SBU_REVERSE;

	if (new_sel == fsa->cur_select)
		goto out_unlock;

	if (fsa->cur_enable & FSA4480_ENABLE_SBU) {
		/* Disable SBU output while re-configuring the switch */
		regmap_write(fsa->regmap, FSA4480_SWITCH_ENABLE,
			     fsa->cur_enable & ~FSA4480_ENABLE_SBU);

		/* 35us to allow the SBU switch to turn off */
		usleep_range(35, 1000);
	}

	regmap_write(fsa->regmap, FSA4480_SWITCH_SELECT, new_sel);
	fsa->cur_select = new_sel;

	if (fsa->cur_enable & FSA4480_ENABLE_SBU) {
		regmap_write(fsa->regmap, FSA4480_SWITCH_ENABLE, fsa->cur_enable);

		/* 15us to allow the SBU switch to turn on again */
		usleep_range(15, 1000);
	}

out_unlock:
	mutex_unlock(&fsa->lock);

	return 0;
}

static int fsa4480_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct fsa4480 *fsa = typec_mux_get_drvdata(mux);
	u8 new_enable;

	mutex_lock(&fsa->lock);

	if (state->alt)
		pr_err("AAAA fsa %s:%d mode %lu, alt->svid: %d, alt->mode: %d\n",
		       __func__, __LINE__,
		       state->mode, state->alt->svid, state->alt->mode);
	else
		pr_err("AAAA fsa %s:%d mode %lu\n", __func__, __LINE__,
		       state->mode);

	dump_regs(fsa);

	WARN_ON(1);

	new_enable = FSA4480_ENABLE_DEVICE | FSA4480_ENABLE_USB;
	if (state->mode >= TYPEC_DP_STATE_A)
		new_enable |= FSA4480_ENABLE_SBU;

	if (new_enable == fsa->cur_enable)
		goto out_unlock;

	regmap_write(fsa->regmap, FSA4480_SWITCH_ENABLE, new_enable);
	fsa->cur_enable = new_enable;

	if (new_enable & FSA4480_ENABLE_SBU) {
		/* 15us to allow the SBU switch to turn off */
		usleep_range(15, 1000);
	}

out_unlock:
	mutex_unlock(&fsa->lock);

	return 0;
}

static int fsa4480_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	struct fsa4480 *fsa;

	fsa = devm_kzalloc(dev, sizeof(*fsa), GFP_KERNEL);
	if (!fsa)
		return -ENOMEM;

	fsa->client = client;
	mutex_init(&fsa->lock);

	fsa->regmap = devm_regmap_init_i2c(client, &fsa4480_regmap_config);
	if (IS_ERR(fsa->regmap))
		return dev_err_probe(dev, PTR_ERR(fsa->regmap), "failed to initialize regmap\n");

	fsa->cur_enable = FSA4480_ENABLE_DEVICE | FSA4480_ENABLE_USB;
	fsa->cur_select = 0;

	/* set default settings */
	regmap_write(fsa->regmap, FSA4480_SLOW_L, 0x00);
	regmap_write(fsa->regmap, FSA4480_SLOW_R, 0x00);
	regmap_write(fsa->regmap, FSA4480_SLOW_MIC, 0x00);
	regmap_write(fsa->regmap, FSA4480_SLOW_SENSE, 0x00);
	regmap_write(fsa->regmap, FSA4480_SLOW_GND, 0x00);
	regmap_write(fsa->regmap, FSA4480_DELAY_L_R, 0x00);
	regmap_write(fsa->regmap, FSA4480_DELAY_L_MIC, 0x00);
	regmap_write(fsa->regmap, FSA4480_DELAY_L_SENSE, 0x00);
	regmap_write(fsa->regmap, FSA4480_DELAY_L_AGND, 0x09);
	regmap_write(fsa->regmap, FSA4480_FUNCTION_ENABLE, 0x0f);
	regmap_write(fsa->regmap, FSA4480_SWITCH_SELECT, fsa->cur_select);
	regmap_write(fsa->regmap, FSA4480_SWITCH_ENABLE, fsa->cur_enable);

	sw_desc.drvdata = fsa;
	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.set = fsa4480_switch_set;

	fsa->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(fsa->sw))
		return dev_err_probe(dev, PTR_ERR(fsa->sw), "failed to register typec switch\n");

	mux_desc.drvdata = fsa;
	mux_desc.fwnode = dev_fwnode(dev);
	mux_desc.set = fsa4480_mux_set;

	fsa->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(fsa->mux)) {
		typec_switch_unregister(fsa->sw);
		return dev_err_probe(dev, PTR_ERR(fsa->mux), "failed to register typec mux\n");
	}

	i2c_set_clientdata(client, fsa);
	return 0;
}

static void fsa4480_remove(struct i2c_client *client)
{
	struct fsa4480 *fsa = i2c_get_clientdata(client);

	typec_mux_unregister(fsa->mux);
	typec_switch_unregister(fsa->sw);
}

static const struct i2c_device_id fsa4480_table[] = {
	{ "fsa4480" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fsa4480_table);

static const struct of_device_id fsa4480_of_table[] = {
	{ .compatible = "fcs,fsa4480" },
	{ }
};
MODULE_DEVICE_TABLE(of, fsa4480_of_table);

static struct i2c_driver fsa4480_driver = {
	.driver = {
		.name = "fsa4480",
		.of_match_table = fsa4480_of_table,
	},
	.probe_new	= fsa4480_probe,
	.remove		= fsa4480_remove,
	.id_table	= fsa4480_table,
};
module_i2c_driver(fsa4480_driver);

MODULE_DESCRIPTION("ON Semiconductor FSA4480 driver");
MODULE_LICENSE("GPL v2");
