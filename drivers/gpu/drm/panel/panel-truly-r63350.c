// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

static const char * const regulator_names[] = {
	"iovcc",
	"avdd",
	"avee",
};

enum panel_vendor {
	NOT_INITIALIZE = -1,
	TRULY,
	AUO,
};

struct cmd_set {
	const u8 *payload;
	size_t size;
	int wait_ms;
};

struct truly_data {
	const struct cmd_set *panel_oncmds;
	unsigned int num_oncmds;
	const struct cmd_set *panel_offcmds;
	unsigned int num_offcmds;
};

struct truly_panel {
	struct drm_panel panel;
	struct device *dev;
	struct mipi_dsi_device *dsi;
	const struct truly_data *data;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;
};

/* Panel vendor/type provided by bootloader */
static enum panel_vendor vendor_from_bl = NOT_INITIALIZE;

static int __init panel_setup(char *str)
{
	if (strstr(str, "truly_r63350"))
		vendor_from_bl = TRULY;
	else if (strstr(str, "auo_r63350"))
		vendor_from_bl = AUO;

	return 1;
}
__setup("mdss_mdp.panel=", panel_setup);

static inline struct truly_panel *panel_to_truly(struct drm_panel *panel)
{
	return container_of(panel, struct truly_panel, panel);
}

static int truly_r63350_power_off(struct truly_panel *truly)
{
	gpiod_set_value(truly->reset_gpio, 1);
	return regulator_bulk_disable(ARRAY_SIZE(truly->supplies),
				      truly->supplies);
}

static int truly_r63350_power_on(struct truly_panel *truly)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(truly->supplies),
				    truly->supplies);
	if (ret)
		return ret;

	if (vendor_from_bl != NOT_INITIALIZE) {
		/*
		 * If bootloader already configures the panel, we are
		 * done and skip panel reset below.
		 */
		return 0;
	}

	/* Reset panel */
	gpiod_set_value(truly->reset_gpio, 0);
	usleep_range(20000, 30000);

	gpiod_set_value(truly->reset_gpio, 1);
	usleep_range(10000, 20000);

	gpiod_set_value(truly->reset_gpio, 0);
	usleep_range(20000, 30000);

	return 0;
}

static int truly_r63350_unprepare(struct drm_panel *panel)
{
	struct truly_panel *truly = panel_to_truly(panel);
	const struct truly_data *data = truly->data;
	const struct cmd_set *cmds = data->panel_offcmds;
	unsigned int num_cmds = data->num_offcmds;
	struct mipi_dsi_device *dsi = truly->dsi;
	struct device *dev = truly->dev;
	int ret;
	int i;

	if (!truly->prepared)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "set_display_off cmd failed: %d\n", ret);
		return ret;
	}

	/* 120ms delay required here as per DCS spec */
	msleep(120);

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "enter_sleep cmd failed: %d\n", ret);
		return ret;
	}

	/* Panel-off magic commands */
	for (i = 0; i < num_cmds; i++) {
		ret = mipi_dsi_dcs_write_buffer(dsi, cmds[i].payload,
						cmds[i].size);
		if (ret < 0) {
			DRM_DEV_ERROR(dev, "off cmd tx%d failed: %d\n", i, ret);
			return ret;
		}

		if (cmds[i].wait_ms)
			msleep(cmds[i].wait_ms);
		else
			usleep_range(80, 100);
	}

	ret = truly_r63350_power_off(truly);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "power_off failed: %d\n", ret);
		return ret;
	}

	truly->prepared = false;

	return 0;
}

static int truly_r63350_prepare(struct drm_panel *panel)
{
	struct truly_panel *truly = panel_to_truly(panel);
	const struct truly_data *data = truly->data;
	const struct cmd_set *cmds = data->panel_oncmds;
	unsigned int num_cmds = data->num_oncmds;
	struct mipi_dsi_device *dsi = truly->dsi;
	struct device *dev = truly->dev;
	int ret;
	int i;

	if (truly->prepared)
		return 0;

	ret = truly_r63350_power_on(truly);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "failed to power on: %d\n", ret);
		return ret;
	}

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0)
		return ret;

	usleep_range(10000, 20000);

	/* Panel-on magic commands */
	for (i = 0; i < num_cmds; i++) {
		ret = mipi_dsi_dcs_write_buffer(dsi, cmds[i].payload,
						cmds[i].size);
		if (ret < 0) {
			DRM_DEV_ERROR(dev, "on cmd tx%d failed: %d\n", i, ret);
			goto power_off;
		}

		if (cmds[i].wait_ms)
			msleep(cmds[i].wait_ms);
		else
			usleep_range(80, 100);
	}

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "exit_sleep_mode cmd failed: %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending exit sleep DCS command */
	msleep(120);

	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "set_display_on cmd failed: %d\n", ret);
		goto power_off;
	}

	/* Per DSI spec wait 120ms after sending set_display_on DCS command */
	msleep(120);

	truly->prepared = true;

	return 0;

power_off:
	truly_r63350_power_off(truly);
	return ret;
}

static const struct drm_display_mode truly_fhd_mode = {
	.clock = 144981,
	.hdisplay = 1080,
	.hsync_start = 1080 + 92,
	.hsync_end = 1080 + 92 + 20,
	.htotal = 1080 + 92 + 20 + 60,
	.vdisplay = 1920,
	.vsync_start = 1920 + 4,
	.vsync_end = 1920 + 4 + 1,
	.vtotal = 1920 + 4 + 1 + 5,
	.flags = 0,
};

static int truly_r63350_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct truly_panel *truly = panel_to_truly(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &truly_fhd_mode);
	if (!mode) {
		DRM_DEV_ERROR(truly->dev,
			      "failed to add display mode\n");
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 121;

	return 1;
}

static const struct drm_panel_funcs truly_r63350_drm_funcs = {
	.prepare = truly_r63350_prepare,
	.unprepare = truly_r63350_unprepare,
	.get_modes = truly_r63350_get_modes,
};

static int truly_r63350_panel_add(struct truly_panel *truly)
{
	struct device *dev = truly->dev;
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(truly->supplies); i++)
		truly->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(truly->supplies),
				      truly->supplies);
	if (ret) {
		dev_err(dev, "failed to get regulator: %d\n", ret);
		return ret;
	}

	truly->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(truly->reset_gpio)) {
		DRM_DEV_ERROR(dev, "failed to get reset gpio %ld\n",
			      PTR_ERR(truly->reset_gpio));
		return PTR_ERR(truly->reset_gpio);
	}

	drm_panel_init(&truly->panel, dev, &truly_r63350_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&truly->panel);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to find backlight: %d\n", ret);
		return ret;
	}

	drm_panel_add(&truly->panel);

	return 0;
}

static const u8 truly_oncmd0[] = {
	0xb0, 0x00,
};

static const u8 truly_oncmd1[] = {
	0xd6, 0x01,
};

static const u8 truly_oncmd2[] = {
	0xb3, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 truly_oncmd3[] = {
	0xb4, 0x0c, 0x00,
};

static const u8 truly_oncmd4[] = {
	0xb6, 0x4b, 0xdb, 0x16,
};

static const u8 truly_oncmd5[] = {
	0xbe, 0x00, 0x04,
};

static const u8 truly_oncmd6[] = {
	0xc0, 0x66,
};

static const u8 truly_oncmd7[] = {
	0xc1, 0x04, 0x60, 0x00, 0x20, 0xa9, 0x30, 0x20,
	0x63, 0xf0, 0xff, 0xff, 0x9b, 0x7b, 0xcf, 0xb5,
	0xff, 0xff, 0x87, 0x8c, 0x41, 0x22, 0x54, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x33, 0x03,
	0x22, 0x00, 0xff,
};

static const u8 truly_oncmd8[] = {
	0xc2, 0x31, 0xf7, 0x80, 0x06, 0x04, 0x00, 0x00,
	0x08,
};

static const u8 truly_oncmd9[] = {
	0xc3, 0x00, 0x00, 0x00,
};

static const u8 truly_oncmd10[] = {
	0xc4, 0x70, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x00, 0x02,
};

static const u8 truly_oncmd11[] = {
	0xc5, 0x00,
};

static const u8 truly_oncmd12[] = {
	0xc6, 0xc8, 0x3c, 0x3c, 0x07, 0x01, 0x07, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x0e, 0x1a, 0x07, 0xc8,
};

static const u8 truly_oncmd13[] = {
	0xc7, 0x03, 0x15, 0x1f, 0x2a, 0x39, 0x46, 0x4e,
	0x5b, 0x3d, 0x45, 0x52, 0x5f, 0x68, 0x6d, 0x72,
	0x01, 0x15, 0x1f, 0x2a, 0x39, 0x46, 0x4e, 0x5b,
	0x3d, 0x45, 0x52, 0x5f, 0x68, 0x6d, 0x78,
};

static const u8 truly_oncmd14[] = {
	0xcb, 0xff, 0xe1, 0x87, 0xff, 0x00, 0x00, 0x00,
	0x00, 0xff, 0xe1, 0x87, 0xff, 0xe8, 0x00, 0x00,
};

static const u8 truly_oncmd15[] = {
	0xcc, 0x34,
};

static const u8 truly_oncmd16[] = {
	0xd0, 0x11, 0x00, 0x00, 0x56, 0xd5, 0x40, 0x19,
	0x19, 0x09, 0x00,
};

static const u8 truly_oncmd17[] = {
	0xd1, 0x00, 0x48, 0x16, 0x0f,
};

static const u8 truly_oncmd18[] = {
	0xd2, 0x5c, 0x00, 0x00,
};

static const u8 truly_oncmd19[] = {
	0xd3, 0x1b, 0x33, 0xbb, 0xbb, 0xb3, 0x33, 0x33,
	0x33, 0x33, 0x00, 0x01, 0x00, 0x00, 0xd8, 0xa0,
	0x0c, 0x4d, 0x4d, 0x33, 0x33, 0x72, 0x12, 0x8a,
	0x57, 0x3d, 0xbc,
};

static const u8 truly_oncmd20[] = {
	0xd5, 0x06, 0x00, 0x00, 0x01, 0x39, 0x01, 0x39,
};

static const u8 truly_oncmd21[] = {
	0xd8, 0x00, 0x00, 0x00,
};

static const u8 truly_oncmd22[] = {
	0xd9, 0x00, 0x00, 0x00,
};

static const u8 truly_oncmd23[] = {
	0xfd, 0x00, 0x00, 0x00, 0x30,
};

static const u8 truly_oncmd24[] = {
	0x35, 0x00,
};

static const u8 truly_oncmd25[] = {
	0x29,
};

static const u8 truly_oncmd26[] = {
	0x11,
};

static const struct cmd_set truly_oncmds[] = {
	{ truly_oncmd0, ARRAY_SIZE(truly_oncmd0), },
	{ truly_oncmd1, ARRAY_SIZE(truly_oncmd1), },
	{ truly_oncmd2, ARRAY_SIZE(truly_oncmd2), },
	{ truly_oncmd3, ARRAY_SIZE(truly_oncmd3), },
	{ truly_oncmd4, ARRAY_SIZE(truly_oncmd4), },
	{ truly_oncmd5, ARRAY_SIZE(truly_oncmd5), },
	{ truly_oncmd6, ARRAY_SIZE(truly_oncmd6), },
	{ truly_oncmd7, ARRAY_SIZE(truly_oncmd7), },
	{ truly_oncmd8, ARRAY_SIZE(truly_oncmd8), },
	{ truly_oncmd9, ARRAY_SIZE(truly_oncmd9), },
	{ truly_oncmd10, ARRAY_SIZE(truly_oncmd10), },
	{ truly_oncmd11, ARRAY_SIZE(truly_oncmd11), },
	{ truly_oncmd12, ARRAY_SIZE(truly_oncmd12), },
	{ truly_oncmd13, ARRAY_SIZE(truly_oncmd13), },
	{ truly_oncmd14, ARRAY_SIZE(truly_oncmd14), },
	{ truly_oncmd15, ARRAY_SIZE(truly_oncmd15), },
	{ truly_oncmd16, ARRAY_SIZE(truly_oncmd16), },
	{ truly_oncmd17, ARRAY_SIZE(truly_oncmd17), },
	{ truly_oncmd18, ARRAY_SIZE(truly_oncmd18), },
	{ truly_oncmd19, ARRAY_SIZE(truly_oncmd19), },
	{ truly_oncmd20, ARRAY_SIZE(truly_oncmd20), },
	{ truly_oncmd21, ARRAY_SIZE(truly_oncmd21), },
	{ truly_oncmd22, ARRAY_SIZE(truly_oncmd22), },
	{ truly_oncmd23, ARRAY_SIZE(truly_oncmd23), },
	{ truly_oncmd24, ARRAY_SIZE(truly_oncmd24), },
	{ truly_oncmd25, ARRAY_SIZE(truly_oncmd25), 50, },
	{ truly_oncmd26, ARRAY_SIZE(truly_oncmd26), 120, },
};

static const u8 truly_offcmd0[] = {
	0x28,
};

static const u8 truly_offcmd1[] = {
	0xb0, 0x04,
};

static const u8 truly_offcmd2[] = {
	0xd3, 0x13, 0x33, 0xbb, 0xb3, 0xb3, 0x33, 0x33,
	0x33, 0x33, 0x00, 0x01, 0x00, 0x00, 0xd8, 0xa0,
	0x0c, 0x4d, 0x4d, 0x33, 0x33, 0x72, 0x12, 0x8a,
	0x57, 0x3d, 0xbc,
};

static const u8 truly_offcmd3[] = {
	0x10,
};

static const u8 truly_offcmd4[] = {
	0xb0, 0x00,
};

static const u8 truly_offcmd5[] = {
	0xb1, 0x01,
};

static const struct cmd_set truly_offcmds[] = {
	{ truly_offcmd0, ARRAY_SIZE(truly_offcmd0), 20, },
	{ truly_offcmd1, ARRAY_SIZE(truly_offcmd1), },
	{ truly_offcmd2, ARRAY_SIZE(truly_offcmd2), 27, },
	{ truly_offcmd3, ARRAY_SIZE(truly_offcmd3), 120, },
	{ truly_offcmd4, ARRAY_SIZE(truly_offcmd4), },
	{ truly_offcmd5, ARRAY_SIZE(truly_offcmd5), },
};

static const struct truly_data truly_fhd_data = {
	.panel_oncmds = truly_oncmds,
	.num_oncmds = ARRAY_SIZE(truly_oncmds),
	.panel_offcmds = truly_offcmds,
	.num_offcmds = ARRAY_SIZE(truly_offcmds),
};

static const u8 auo_oncmd0[] = {
	0xb0, 0x04,
};

static const u8 auo_oncmd1[] = {
	0xd6, 0x01,
};

static const u8 auo_oncmd2[] = {
	0xb3, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const u8 auo_oncmd3[] = {
	0xb4, 0x0c, 0x00,
};

static const u8 auo_oncmd4[] = {
	0xb6, 0x4b, 0xdb, 0x00,
};

static const u8 auo_oncmd5[] = {
	0xc0, 0x66,
};

static const u8 auo_oncmd6[] = {
	0xc1, 0x04, 0x60, 0x00, 0x20, 0x29, 0x41, 0x22,
	0xfb, 0xf0, 0xff, 0xff, 0x9b, 0x7b, 0xcf, 0xb5,
	0xff, 0xff, 0x87, 0x8c, 0xc5, 0x11, 0x54, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x11, 0x02,
	0x21, 0x00, 0xff, 0x11,
};

static const u8 auo_oncmd7[] = {
	0xc2, 0x31, 0xf7, 0x80, 0x06, 0x04, 0x00, 0x00,
	0x08,
};

static const u8 auo_oncmd8[] = {
	0xc4, 0x70, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x00, 0x02,
};

static const u8 auo_oncmd9[] = {
	0xc6, 0xc8, 0x3c, 0x3c, 0x07, 0x01, 0x07, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x0e, 0x1a, 0x07, 0xc8,
};

static const u8 auo_oncmd10[] = {
	0xc7, 0x0a, 0x18, 0x20, 0x29, 0x37, 0x43, 0x4d,
	0x5b, 0x3f, 0x46, 0x52, 0x5f, 0x67, 0x70, 0x7c,
	0x0a, 0x18, 0x20, 0x29, 0x37, 0x43, 0x4d, 0x5b,
	0x3f, 0x46, 0x52, 0x5f, 0x67, 0x70, 0x7c,
};

static const u8 auo_oncmd11[] = {
	0xcb, 0x7f, 0xe1, 0x87, 0xff, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x00, 0x00,
};

static const u8 auo_oncmd12[] = {
	0xcc, 0x32,
};

static const u8 auo_oncmd13[] = {
	0xd0, 0x11, 0x00, 0x00, 0x56, 0xd7, 0x40, 0x19,
	0x19, 0x09, 0x00,
};

static const u8 auo_oncmd14[] = {
	0xd1, 0x00, 0x48, 0x16, 0x0f,
};

static const u8 auo_oncmd15[] = {
	0xd3, 0x1b, 0x33, 0xbb, 0xbb, 0xb3, 0x33, 0x33,
	0x33, 0x33, 0x00, 0x01, 0x00, 0x00, 0xd8, 0xa0,
	0x0c, 0x37, 0x37, 0x33, 0x33, 0x72, 0x12, 0x8a,
	0x57, 0x3d, 0xbc,
};

static const u8 auo_oncmd16[] = {
	0xd5, 0x06, 0x00, 0x00, 0x01, 0x35, 0x01, 0x35,
};

static const u8 auo_oncmd17[] = {
	0x29,
};

static const u8 auo_oncmd18[] = {
	0x11,
};

static const struct cmd_set auo_oncmds[] = {
	{ auo_oncmd0, ARRAY_SIZE(auo_oncmd0), },
	{ auo_oncmd1, ARRAY_SIZE(auo_oncmd1), },
	{ auo_oncmd2, ARRAY_SIZE(auo_oncmd2), },
	{ auo_oncmd3, ARRAY_SIZE(auo_oncmd3), },
	{ auo_oncmd4, ARRAY_SIZE(auo_oncmd4), },
	{ auo_oncmd5, ARRAY_SIZE(auo_oncmd5), },
	{ auo_oncmd6, ARRAY_SIZE(auo_oncmd6), },
	{ auo_oncmd7, ARRAY_SIZE(auo_oncmd7), },
	{ auo_oncmd8, ARRAY_SIZE(auo_oncmd8), },
	{ auo_oncmd9, ARRAY_SIZE(auo_oncmd9), },
	{ auo_oncmd10, ARRAY_SIZE(auo_oncmd10), },
	{ auo_oncmd11, ARRAY_SIZE(auo_oncmd11), },
	{ auo_oncmd12, ARRAY_SIZE(auo_oncmd12), },
	{ auo_oncmd13, ARRAY_SIZE(auo_oncmd13), },
	{ auo_oncmd14, ARRAY_SIZE(auo_oncmd14), },
	{ auo_oncmd15, ARRAY_SIZE(auo_oncmd15), },
	{ auo_oncmd16, ARRAY_SIZE(auo_oncmd16), },
	{ auo_oncmd17, ARRAY_SIZE(auo_oncmd17), 100, },
	{ auo_oncmd18, ARRAY_SIZE(auo_oncmd18), 120, },
};

static const u8 auo_offcmd0[] = {
	0x28,
};

static const u8 auo_offcmd1[] = {
	0xb0, 0x04,
};

static const struct cmd_set auo_offcmds[] = {
	{ auo_offcmd0, ARRAY_SIZE(auo_offcmd0), 10, },
	{ auo_offcmd1, ARRAY_SIZE(auo_offcmd1), 120, },
};

static const struct truly_data auo_fhd_data = {
	.panel_oncmds = auo_oncmds,
	.num_oncmds = ARRAY_SIZE(auo_oncmds),
	.panel_offcmds = auo_offcmds,
	.num_offcmds = ARRAY_SIZE(auo_offcmds),
};

static int truly_r63350_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct truly_panel *truly;
	int ret;

	truly = devm_kzalloc(dev, sizeof(*truly), GFP_KERNEL);
	if (!truly)
		return -ENOMEM;

	truly->data = of_device_get_match_data(dev);
	if (!truly->data)
		return -ENODEV;

	/* Override data if bootloader provides the panel type */
	if (vendor_from_bl == TRULY)
		truly->data = &truly_fhd_data;
	else if (vendor_from_bl == AUO)
		truly->data = &auo_fhd_data;

	truly->dev = dev;

	ret = truly_r63350_panel_add(truly);
	if (ret)
		return ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;
	truly->dsi = dsi;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to attach DSI device: %d\n", ret);
		goto rm_panel;
	}

	mipi_dsi_set_drvdata(dsi, truly);

	return 0;

rm_panel:
	drm_panel_remove(&truly->panel);
	return ret;
}

static void truly_r63350_remove(struct mipi_dsi_device *dsi)
{
	struct truly_panel *truly = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(truly->dsi);
	drm_panel_remove(&truly->panel);

	return;
}

static const struct of_device_id truly_r63350_of_match[] = {
	{ .compatible = "truly,r63350-fhd", .data = &truly_fhd_data, },
	{ .compatible = "auo,r63350-fhd", .data = &auo_fhd_data, },
	{ }
};
MODULE_DEVICE_TABLE(of, truly_r63350_of_match);

static struct mipi_dsi_driver truly_r63350_driver = {
	.driver = {
		.name = "panel-truly-r63350",
		.of_match_table = truly_r63350_of_match,
	},
	.probe = truly_r63350_probe,
	.remove = truly_r63350_remove,
};
module_mipi_dsi_driver(truly_r63350_driver);

MODULE_DESCRIPTION("Truly R63350 DSI Panel Driver");
MODULE_LICENSE("GPL v2");
