// SPDX-License-Identifier: GPL-2.0
//
// Freescale/NXP VF500/VF610 hardware CRC driver
//
// Copyright (c) 2018 Krzysztof Kozlowski <krzk@kernel.org>

#include <linux/bitrev.h>
#include <linux/clk.h>
#include <linux/crc32poly.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <crypto/internal/hash.h>

#include <asm/unaligned.h>

#define DRIVER_NAME             "vf-crc"
#define CHKSUM_DIGEST_SIZE	4
#define CHKSUM_BLOCK_SIZE       1

/* Register offsets */
#define CRC_DATA		0x00
#define CRC_POLY		0x04
#define CRC_CTRL		0x08

/* CTRL bit fields */
/* Width of CRC (0 - 16 bit, 1 - 32 bit) */
#define CRC_CTRL_TCRC		BIT(24)
/* Write CRC Data register as Seed (0 - data, 1 - seed) */
#define CRC_CTRL_WAS		BIT(25)
/* Final XOR on checksum */
#define CRC_CTRL_FXOR		BIT(26)

#define CRC_INIT_DEFAULT        0x0

struct vf_crc {
	struct clk		*clk;
	struct device		*dev;
	void __iomem		*iobase;

	/* Lock protecting access to HW registers. */
	struct mutex		lock;
};

struct vf_crc_desc_ctx {
	struct vf_crc		*crc;
	/*
	 * Current state of computed CRC (used for re-init on subsequent
	 * requests).
	 */
	u32			state;
};

struct vf_crc_tfm_ctx {
	unsigned int		align;
	u32			ctrl_init;
	bool			is_16_bit;
	u32			key;
	u32			poly;
};

static struct vf_crc *vf_crc_data;

static int vf_crc_cra_init32(struct crypto_tfm *tfm)
{
	struct vf_crc_tfm_ctx *mctx = crypto_tfm_ctx(tfm);

	mctx->align = sizeof(u32);
	/* 32 bit, no XOR */
	mctx->ctrl_init = CRC_CTRL_TCRC;
	mctx->is_16_bit = false;
	mctx->key = CRC_INIT_DEFAULT;
	mctx->poly = CRC32_POLY_BE;

	return 0;
}

static int vf_crc_cra_init16(struct crypto_tfm *tfm)
{
	struct vf_crc_tfm_ctx *mctx = crypto_tfm_ctx(tfm);

	mctx->align = sizeof(u16);
	/* 16 bit, no XOR */
	mctx->ctrl_init = 0;
	mctx->is_16_bit = true;
	mctx->key = CRC_INIT_DEFAULT;
	mctx->poly = CRC16_POLY_BE;

	return 0;
}

static int vf_crc_setkey(struct crypto_shash *tfm, const u8 *key,
			 unsigned int keylen)
{
	struct vf_crc_tfm_ctx *mctx = crypto_shash_ctx(tfm);

	if (keylen != mctx->align) {
		crypto_shash_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}

	if (mctx->is_16_bit)
		mctx->key = bitrev16(get_unaligned_le16(key));
	else
		mctx->key = bitrev32(get_unaligned_le32(key));

	return 0;
}

static int vf_crc_init(struct shash_desc *desc)
{
	struct vf_crc_desc_ctx *desc_ctx = shash_desc_ctx(desc);
	struct vf_crc_tfm_ctx *mctx = crypto_shash_ctx(desc->tfm);

	desc_ctx->crc = vf_crc_data;
	desc_ctx->state = mctx->key;

	return 0;
}

static void vf_crc_initialize_regs(struct vf_crc_tfm_ctx *mctx,
				   struct vf_crc_desc_ctx *desc_ctx)
{
	struct vf_crc *crc = desc_ctx->crc;

	/* Init and write-as-seed (next data write will be the seed) */
	writel(mctx->ctrl_init, crc->iobase + CRC_CTRL);
	writel(mctx->poly, crc->iobase + CRC_POLY);
	writel(mctx->ctrl_init | CRC_CTRL_WAS, crc->iobase + CRC_CTRL);

	/* Initialize engine with either key or state from previous rounds */
	writel(desc_ctx->state, crc->iobase + CRC_DATA);

	/* Clear write-as-seed */
	writel(mctx->ctrl_init, crc->iobase + CRC_CTRL);
}

static void vf_crc_write_bytes(void __iomem *addr, const u8 *data,
			       unsigned int len)
{
	unsigned int i;
	u8 value;

	for (i = 0; i < len; i++) {
		value = bitrev8(data[i]);
		writeb(value, addr);
	}
}

static int vf_crc_update_prepare(struct vf_crc_tfm_ctx *mctx,
				 struct vf_crc_desc_ctx *desc_ctx)
{
	struct vf_crc *crc = desc_ctx->crc;
	int ret;

	ret = clk_enable(crc->clk);
	if (ret) {
		dev_err(crc->dev, "Failed to enable clock\n");
		return ret;
	}

	mutex_lock(&crc->lock);

	vf_crc_initialize_regs(mctx, desc_ctx);

	return 0;
}

static void vf_crc_update_unprepare(struct vf_crc_tfm_ctx *mctx,
				    struct vf_crc_desc_ctx *desc_ctx)
{
	struct vf_crc *crc = desc_ctx->crc;

	if (mctx->is_16_bit)
		desc_ctx->state = readw(crc->iobase + CRC_DATA);
	else
		desc_ctx->state = readl(crc->iobase + CRC_DATA);

	mutex_unlock(&crc->lock);

	clk_disable(crc->clk);
}

static int vf_crc_update(struct shash_desc *desc, const u8 *data,
			 unsigned int len)
{
	struct vf_crc_desc_ctx *desc_ctx = shash_desc_ctx(desc);
	struct vf_crc_tfm_ctx *mctx = crypto_shash_ctx(desc->tfm);
	unsigned int i, len_align;
	int ret;

	ret = vf_crc_update_prepare(mctx, desc_ctx);
	if (ret)
		return ret;

	len_align = ALIGN_DOWN(len, mctx->align);
	if (mctx->is_16_bit) {
		u16 value;

		for (i = 0; i < len_align; i += mctx->align) {
			value = bitrev16(get_unaligned_le16(data + i));
			writew(value, desc_ctx->crc->iobase + CRC_DATA);
		}
	} else {
		u32 value;

		for (i = 0; i < len_align; i += mctx->align) {
			value = bitrev32(get_unaligned_le32(data + i));
			writel(value, desc_ctx->crc->iobase + CRC_DATA);
		}
	}

	if (len != len_align)
		vf_crc_write_bytes(desc_ctx->crc->iobase + CRC_DATA,
				   &data[len_align], len - len_align);

	vf_crc_update_unprepare(mctx, desc_ctx);

	return 0;
}

static int vf_crc_final(struct shash_desc *desc, u8 *out)
{
	struct vf_crc_desc_ctx *desc_ctx = shash_desc_ctx(desc);
	struct vf_crc_tfm_ctx *mctx = crypto_shash_ctx(desc->tfm);

	if (mctx->is_16_bit)
		put_unaligned_le16(bitrev16(desc_ctx->state), out);
	else
		put_unaligned_le32(bitrev32(desc_ctx->state), out);

	return 0;
}

static struct shash_alg algs[] = {
	{
		.setkey         = vf_crc_setkey,
		.init           = vf_crc_init,
		.update         = vf_crc_update,
		.final          = vf_crc_final,
		.descsize       = sizeof(struct vf_crc_desc_ctx),
		.digestsize     = CHKSUM_DIGEST_SIZE,
		.base           = {
			.cra_name               = "crc32",
			.cra_driver_name        = DRIVER_NAME,
			.cra_priority           = 200,
			.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
			.cra_blocksize          = CHKSUM_BLOCK_SIZE,
			.cra_ctxsize            = sizeof(struct vf_crc_tfm_ctx),
			.cra_module             = THIS_MODULE,
			.cra_init               = vf_crc_cra_init32,
		}
	},
	{
		.setkey         = vf_crc_setkey,
		.init           = vf_crc_init,
		.update         = vf_crc_update,
		.final          = vf_crc_final,
		.descsize       = sizeof(struct vf_crc_desc_ctx),
		.digestsize     = (CHKSUM_DIGEST_SIZE / 2),
		.base           = {
			.cra_name               = "crc16",
			.cra_driver_name        = DRIVER_NAME,
			.cra_priority           = 200,
			.cra_flags		= CRYPTO_ALG_OPTIONAL_KEY,
			.cra_blocksize          = CHKSUM_BLOCK_SIZE,
			.cra_ctxsize            = sizeof(struct vf_crc_tfm_ctx),
			.cra_module             = THIS_MODULE,
			.cra_init               = vf_crc_cra_init16,
		}
	}
};

static int vf_crc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct vf_crc *crc;
	int ret;

	if (vf_crc_data) {
		dev_err(dev, "Device already registered (only one instance allowed)\n");
		return -EINVAL;
	}

	crc = devm_kzalloc(dev, sizeof(*crc), GFP_KERNEL);
	if (!crc)
		return -ENOMEM;

	crc->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	crc->iobase = devm_ioremap_resource(dev, res);
	if (IS_ERR(crc->iobase))
		return PTR_ERR(crc->iobase);

	crc->clk = devm_clk_get(dev, "crc");
	if (IS_ERR(crc->clk)) {
		dev_err(dev, "Could not get clock\n");
		return PTR_ERR(crc->clk);
	}

	vf_crc_data = crc;
	mutex_init(&crc->lock);

	ret = clk_prepare(crc->clk);
	if (ret) {
		dev_err(crc->dev, "Failed to prepare clock\n");
		goto err_clk;
	}

	ret = crypto_register_shashes(algs, ARRAY_SIZE(algs));
	if (ret) {
		dev_err(dev, "Failed to register crypto algorithms\n");
		goto err_hash;
	}

	dev_dbg(dev, "HW CRC accelerator initialized\n");

	return 0;

err_hash:
	clk_unprepare(crc->clk);
err_clk:
	vf_crc_data = NULL;

	return ret;
}

static int vf_crc_remove(struct platform_device *pdev)
{
	crypto_unregister_shashes(algs, ARRAY_SIZE(algs));
	clk_unprepare(vf_crc_data->clk);
	vf_crc_data = NULL;

	return 0;
}

static const struct of_device_id vf_crc_dt_match[] = {
	{ .compatible = "fsl,vf610-crc", },
	{},
};
MODULE_DEVICE_TABLE(of, vf_crc_dt_match);

static struct platform_driver vf_crc_driver = {
	.probe  = vf_crc_probe,
	.remove = vf_crc_remove,
	.driver = {
		.name           = DRIVER_NAME,
		.of_match_table = vf_crc_dt_match,
	},
};

module_platform_driver(vf_crc_driver);

MODULE_AUTHOR("Krzysztof Kozlowski <krzk@kernel.org>");
MODULE_DESCRIPTION("Freescale/NXP Vybrid CRC32 hardware driver");
MODULE_LICENSE("GPL v2");
