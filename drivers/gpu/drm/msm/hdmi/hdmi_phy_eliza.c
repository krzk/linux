// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gcd.h>
#include <linux/iopoll.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/types.h>

#include "hdmi.h"

#define REF_CLK_HZ				38400000 /* 38.4 MHz */
#define HDMI_PLL_MAX_FREQ			600000000 /* 600 MHz */
/* Unscrambled clock freq threshold */
#define HDMI_PLL_HIGH_FREQ			340000000 /* 340 Mhz */
#define HDMI_PLL_MIN_FREQ			25000000

#define HDMI_PHY_MAX_BIT_RATE			2500000000 /* 2.5 Gbps */

/* Number of TX lanes */
#define HDMI_PHY_TXN_CONTROL			4

#define MPLL(mul, qut, rem, den, clkd, vcof, pat, rxv, in, prop) \
	.multiplier = (mul), \
	.quotient = (qut), \
	.remainder = (rem), \
	.denominator = (den), \
	.tx_clk_div = (clkd), \
	.vco_freq = (vcof), \
	.clk_pat = (pat), \
	.rxvcosel = (rxv), \
	.cp_int_holder = (in), \
	.cp_prop_holder = (prop)

struct hdmi_eliza_phy_pll_reg_cfg {
	u32 vco_freq;
	u32 rxvcosel;
	u32 tx_clk_div;
	u32 cp_int_holder;
	u32 cp_prop_holder;
	u32 quotient;
	u32 remainder;
	u32 multiplier;
	u32 denominator;
	u32 clk_pat;
};

struct hdmi_pll_eliza {
	struct device *dev;
	struct clk_hw clk_hw;
	unsigned long rate;
	struct hdmi_eliza_phy_pll_reg_cfg cfg;
};


struct hdmi_pll_eliza_mpll {
	unsigned long rate;
	struct hdmi_eliza_phy_pll_reg_cfg cfg;
};

static const struct hdmi_pll_eliza_mpll mpll_lut[] = {
	{ 25200000, { MPLL(0xb0, 0xe525, 0, 1, 5, 2, 0, 3, 0xc, 0x11) }, },
	{ 27000000, { MPLL(0xc0, 0x8000, 0, 1, 5, 2, 0, 3, 0xc, 0x11) }, },
	{ 27027000, { MPLL(0xc0, 0x9ccc, 4, 1, 5, 2, 0, 3, 0xb, 0x12) }, },
	{ 54000000, { MPLL(0xc0, 0x8000, 0, 1, 4, 2, 0, 3, 0xb, 0x11) }, },
	{ 59400000, { MPLL(0xd6, 0xc000, 0, 1, 4, 2, 0, 3, 0xb, 0x12) }, },
	{ 72000000, { MPLL(0x76, 0x0000, 0, 1, 3, 3, 0, 4, 0xc, 0x10) }, },
	{ 74250000, { MPLL(0x7a, 0x5800, 0, 1, 3, 3, 0, 4, 0xb, 0x12) }, },
	{ 82500000, { MPLL(0x8a, 0xf000, 0, 1, 3, 3, 0, 4, 0xb, 0x11) }, },
	{ 90000000, { MPLL(0x9a, 0xc000, 0, 1, 3, 2, 0, 4, 0xd, 0x11) }, },
	{ 99000000, { MPLL(0xae, 0x2000, 0, 1, 3, 2, 0, 4, 0xc, 0x11) }, },
	{ 108000000, { MPLL(0xc0, 0x8000, 0, 1, 3, 2, 0, 3, 0xb, 0x11) }, },
	{ 118800000, { MPLL(0xd6, 0xc000, 0, 1, 3, 2, 0, 3, 0xb, 0x12) }, },
	{ 148500000, { MPLL(0x7a, 0x5800, 0, 1, 2, 3, 0, 4, 0xc, 0x11) }, },
	{ 165000000, { MPLL(0x8a, 0xf000, 0, 1, 2, 3, 0, 4, 0xb, 0x11) }, },
	{ 185625000, { MPLL(0xa0, 0xae00, 0, 1, 2, 2, 0, 4, 0xc, 0x11) }, },
	{ 198000000, { MPLL(0xae, 0x2000, 0, 1, 2, 2, 0, 4, 0xc, 0x11) }, },
	{ 297000000, { MPLL(0x7a, 0x5800, 0, 1, 1, 3, 0, 4, 0xc, 0x11) }, },
	{ 371250000, { MPLL(0xa0, 0xae00, 0, 1, 1, 2, 1, 4, 0xc, 0x11) }, },
	{ 396000000, { MPLL(0xae, 0x2000, 0, 1, 1, 2, 1, 4, 0xc, 0x11) }, },
	{ 495000000, { MPLL(0xe0, 0xe800, 0, 1, 1, 2, 1, 3, 0xb, 0x12) }, },
	{ 594000000, { MPLL(0x7a, 0x5800, 0, 1, 0, 3, 1, 4, 0xc, 0x11) }, },
};

#define hw_clk_to_pll(x) container_of(x, struct hdmi_pll_eliza, clk_hw)

static struct hdmi_phy *pll_get_phy(struct hdmi_pll_eliza *pll)
{
	return dev_get_drvdata(pll->dev);
}

static const struct hdmi_eliza_phy_pll_reg_cfg *
hdmi_pll_eliza_mpll_lut_decoder(unsigned long rate)
{
	const struct hdmi_eliza_phy_pll_reg_cfg *cfg = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(mpll_lut); i++) {
		if (rate == mpll_lut[i].rate) {
			cfg = &mpll_lut[i].cfg;
			break;
		}
	}

	return cfg;
}

static int hdmi_eliza_pll_calculator(struct hdmi_pll_eliza *pll,
				     unsigned long rate,
				     unsigned long parent_rate,
				     struct hdmi_eliza_phy_pll_reg_cfg *cfg)
{
	const s64 bit_rate_multiplier = 1000;
	const s64 bit_rate_divider = 1000;
	const s64 cp_int_param[3] = {4524, -679588, 30860643};
	const s64 cp_prop_param[3] = {8023, -1160252, 51717042};
	const u64 gmult = 1e4;
	s64 expon, base_rate, bit_rate;
	s64 ref_clk_int, ref_clk_div;
	u64 aux0, aux1, aux2, vco_freq;
	u64 utemp, utemp2, multiplier, f_rx_vco;
	s64 stemp, stemp2, stemp3;
	s32 choice1, ref_ana_mpll_div;
	u32 precision3 = 1e3;
	u32 precision6 = 1e6;

	if (rate > HDMI_PLL_HIGH_FREQ)
		cfg->clk_pat = 1;
	else
		cfg->clk_pat = 0;

	/*
	 * base_rate = clock_khz  * bit_rate_multiplier / 1000
	 * Division by 1000, nullifies the conversion from KHz to Hz.
	 */
	base_rate = rate / 1000 * gmult;

	/* bit_rate = clock_khz * bit_rate_multiplier/bit_rate_divider. */
	utemp = DIV_ROUND_UP_ULL(bit_rate_multiplier, bit_rate_divider);

	bit_rate = base_rate * utemp;

	/*
	 * REF_CLK_HZ: 38.4 MHz
	 * min_ref_clk: 50
	 * choice1 = floor(log2(REF_CLK_HZ/min_ref_clk))
	 * choice1 = floor(-0.38)
	 */
	choice1 = -1;
	ref_ana_mpll_div = max_t(s32, choice1, 0);

	/* ref_clk_div = 2**ref_ana_mpll_div; */
	ref_clk_div = 1UL << ref_ana_mpll_div;
	ref_clk_int = DIV_ROUND_UP(REF_CLK_HZ, ref_clk_div);

	/* expon = floor(log2(4.999999e9/base_rate)) */
	utemp = 4.999999e9;

	do_div(utemp, base_rate);
	expon = fls64(utemp) - 1;
	if (expon >= 0)
		vco_freq = base_rate * (1UL << expon);
	else
		vco_freq = base_rate >> 1;

	/* multiplier = 2*(floor(bit_rate) * 2**expon / ref_clk_int) -16) */
	multiplier = DIV_ROUND_UP_ULL(vco_freq, ref_clk_int);
	multiplier = 2 * (multiplier - 16 - 1);
	cfg->multiplier = multiplier;

	/* f_rx_vco = bit_rate * 2**floor(log2(6e9/bit_rate)); */
	utemp = 6e9;
	f_rx_vco = DIV_ROUND_UP_ULL(utemp, bit_rate);
	f_rx_vco = ilog2(f_rx_vco);
	f_rx_vco = bit_rate * (1UL << f_rx_vco);

	utemp = 4e9;
	if (f_rx_vco < utemp)
		f_rx_vco *= 2;

	utemp = 5e9;
	if (f_rx_vco > utemp)
		cfg->rxvcosel = 4;
	else
		cfg->rxvcosel = 3;

	/*
	 * aux0 = (bit_rate * 2**expon / ref_clk_int - (multiplier/2 + 16);
	 * aux0 = aux0 * 2** 16;
	 */
	if (expon >= 0)
		utemp = bit_rate * (1UL << expon);
	else
		utemp = bit_rate >> 1;

	/*
	 * Need to store the floating-point values as they are
	 * essential for precise computations; thus, increasing
	 * the precision by 10^6.
	 */

	utemp *= precision6;
	utemp = DIV_ROUND_UP_ULL(utemp, ref_clk_int);
	aux0 = utemp - (((multiplier >> 1) + 16)) * precision6;
	aux0 *= (1UL << 16);
	do_div(aux0, precision6);
	cfg->quotient = aux0;

	/* aux1 = ref_clk_int * bit_rate_divider */
	aux1 = ref_clk_int * bit_rate_divider;

	/*
	 * aux2 = bit_rate * bit_rate_divider * 2**expon;
	 * aux2 = aux2 - ((multiplier/2 + 16) * aux1 * 2**16);
	 * aux2 = aux2 - aux1 *aux0;
	 */

	if (expon >= 0)
		aux2 = bit_rate * (1UL << expon);
	else
		aux2 = bit_rate >> 1;

	aux2 *= bit_rate_divider;
	utemp = ((multiplier >> 1) + 16) * aux1;
	aux2 = ((aux2 - utemp) * (1UL << 16)) - (cfg->quotient * aux1);

	if (aux2 == 0) {
		cfg->remainder = 0;
		cfg->denominator = 1;
	} else {
		cfg->remainder = DIV_ROUND_UP_ULL(aux2, gcd(aux1, aux2));
		cfg->denominator = DIV_ROUND_UP_ULL(aux1, gcd(aux1, aux2));
	}

	/* tx_clk_div = log2(round(vco_freq/base_rate) * 2) */
	utemp = DIV_ROUND_UP_ULL(vco_freq, base_rate);
	cfg->tx_clk_div = ilog2(utemp * 2);

	/*
	 * FIXME: For rate 198000000, LUT has vco_freq 2 but calculator gives 3
	 * FIXME: For rate 198000000 and many others, LUT has cp_prop_holder 11,
	 * but calculator gives 12
	 */
	utemp = 3.389e9;
	utemp2 = 5e9;
	if (vco_freq <= utemp) {
		cfg->vco_freq = 2;
	} else if (vco_freq < utemp2) {
		cfg->vco_freq = 3;
	} else {
		DRM_DEV_ERROR(pll->dev, "bit_rate and vco_freq combination not supported: %llu",
			      vco_freq);
		return -EINVAL;
	}

	/* stemp = (REF_CLK_MHZ/2**ref_ana_mpll_div) */
	stemp = (REF_CLK_HZ / 1000000) * precision3;
	stemp = DIV_ROUND_CLOSEST(stemp, 1UL << ref_ana_mpll_div);

	/* stemp2 = cp_int_param[0] * stemp**2; */
	stemp2 = stemp * stemp * cp_int_param[0];
	stemp2 = DIV_ROUND_CLOSEST_ULL(stemp2, precision6);

	/* stemp3 = cp_int_param[1] * stemp; */
	stemp3 = DIV_ROUND_DOWN_ULL(stemp * cp_int_param[1], precision3);

	/*
	 * cp_int_holder = floor(cp_int_param[0]*(stemp)**2
	 *                      + cp_int_param[1] * stemp
	 *                      + cp_int_param[2])
	 */
	stemp3 += cp_int_param[2];
	stemp3 += stemp2;

	cfg->cp_int_holder = DIV_ROUND_CLOSEST_ULL(stemp3, precision6);

	/* stemp2 = cp_prop_param[0] * stemp**2; */
	stemp2 = stemp * stemp * cp_prop_param[0];
	stemp2 = DIV_ROUND_CLOSEST_ULL(stemp2, precision6);

	/* stemp3 = cp_prop_param[1] * stemp; */
	stemp3 = DIV_ROUND_DOWN_ULL(stemp * cp_prop_param[1], precision3);

	/*
	 * cp_int_holder = floor(cp_prop_param[0]*(stemp)**2
	 *                      + cp_prop_param[1] * stemp
	 *                      + cp_prop_param[2])
	 */
	stemp3 += stemp2 + cp_prop_param[2];

	cfg->cp_prop_holder = DIV_ROUND_CLOSEST_ULL(stemp3, precision6) - 1;

	return 0;
}

static int hdmi_eliza_pll_configure_pll(struct hdmi_pll_eliza *pll,
					unsigned long rate,
					unsigned long parent_rate)
{
	struct hdmi_phy *phy = pll_get_phy(pll);
	const struct hdmi_eliza_phy_pll_reg_cfg *cfg;
	struct hdmi_eliza_phy_pll_reg_cfg cfg_calc = {};
	int ret;
	u32 val;

	cfg = hdmi_pll_eliza_mpll_lut_decoder(rate);
	if (!cfg) {
		DRM_INFO("no valid mpll entry fall back to calculator");

		ret = hdmi_eliza_pll_calculator(pll, rate, parent_rate, &cfg_calc);
		if (ret) {
			DRM_ERROR("PLL calculation failed for rate %lu\n", rate);
			return ret;
		}
		cfg = &cfg_calc;
	}

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_CLOCK_PATTERN_CONFIG_0, BIT(0), cfg->clk_pat);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_2, GENMASK(6, 4), cfg->rxvcosel << 4);
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_1, GENMASK(7, 2), 0x11 << 2);

	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_0, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_10,
		       cfg->cp_int_holder & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_12,
		       cfg->cp_prop_holder & 0xff);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_0, BIT(0), BIT(0));
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_2,
		       cfg->multiplier & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_3,
		       (cfg->multiplier >> 8) & 0xff);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_0, BIT(2), BIT(2));
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_6, cfg->quotient & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_7,
		       (cfg->quotient >> 8) & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_8, cfg->remainder & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_9,
		       (cfg->remainder >> 8) & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_4, cfg->denominator & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_5,
		       (cfg->denominator >> 8) & 0xff);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_1,
		       cfg->tx_clk_div & 0x7);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_14, 0x82);

	val = hdmi_phy_read(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_0);
	val &= ~BIT(6);
	val &= ~(0x3 << 3);
	val |= (cfg->vco_freq & 0x3) << 3;
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_0, val);

	/*
	 * Ensure that vco configuration gets flushed to hardware before
	 * enabling the PLL
	 */
	wmb();

	pll->rate = rate;
	pll->cfg = *cfg;

	return 0;
}

static int hdmi_eliza_pll_set_clk_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct hdmi_pll_eliza *pll = hw_clk_to_pll(hw);

	return hdmi_eliza_pll_configure_pll(pll, rate, parent_rate);
};

static int hdmi_eliza_pll_prepare(struct clk_hw *hw)
{
	struct hdmi_pll_eliza *pll = hw_clk_to_pll(hw);
	struct hdmi_phy *phy = pll_get_phy(pll);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_CLOCK_PATTERN_CONFIG_0, BIT(1), BIT(1));

	/*
	 * This Synopsys PLL does not have a PLL lock-like register, thus there
	 * is no way to tell when the PLL actually locked.
	 * Returning now, would lead to stuck pclk or iface RCG clocks at off
	 * (disp_cc_mdss_hdmi_pclk_clk status stuck at 'off'),
	 * thus obviously the professional solution is to delay.
	 * 8 ms was figured out during tests, not based on any datasheet.
	 */
	mdelay(8);

	return 0;
}

static int hdmi_eliza_pll_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	req->rate = clamp_t(unsigned long, req->rate, HDMI_PLL_MIN_FREQ, HDMI_PLL_MAX_FREQ);

	return 0;
}

static u64 hdmi_eliza_pll_vco_freq(u64 ref_clk_hz, u32 multiplier, u32 frac_quot,
				   u32 frac_rem, u32 frac_den)
{
	u64 num;
	u64 den;

	num = 32ULL * 65536ULL * frac_den +
		(u64)multiplier * 65536ULL * frac_den +
		2ULL * ((u64)frac_quot * frac_den + frac_rem);

	den = 2ULL * 65536ULL * frac_den;

	return mul_u64_u64_div_u64(ref_clk_hz, num, den);
}

static unsigned long hdmi_eliza_pll_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct hdmi_pll_eliza *pll = hw_clk_to_pll(hw);
	u64 rate;

	rate = hdmi_eliza_pll_vco_freq(REF_CLK_HZ, pll->cfg.multiplier,
				       pll->cfg.quotient, pll->cfg.remainder,
				       pll->cfg.denominator);

	/* rate = vco_freq * 2 / (10 * 2^tx_clk_div) */
	rate *= 2;
	do_div(rate, 10);
	rate >>= pll->cfg.tx_clk_div;

	return rate;
}


static void hdmi_eliza_pll_unprepare(struct clk_hw *hw)
{
	struct hdmi_pll_eliza *pll = hw_clk_to_pll(hw);
	struct hdmi_phy *phy = pll_get_phy(pll);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_CLOCK_PATTERN_CONFIG_0, BIT(1), 0);
}

static const struct clk_ops hdmi_eliza_pll_ops = {
	.set_rate = hdmi_eliza_pll_set_clk_rate,
	.determine_rate = hdmi_eliza_pll_determine_rate,
	.recalc_rate = hdmi_eliza_pll_recalc_rate,
	.prepare = hdmi_eliza_pll_prepare,
	.unprepare = hdmi_eliza_pll_unprepare,
};

static const struct clk_init_data pll_init = {
	.name = "hdmipll",
	.ops = &hdmi_eliza_pll_ops,
	.parent_data = (const struct clk_parent_data[]) {
		{ .fw_name = "xo", .name = "xo_board" },
	},
	.num_parents = 1,
};

int msm_hdmi_pll_eliza_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hdmi_pll_eliza *pll;
	struct hdmi_phy *phy;
	int ret;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;

	pll->dev = dev;
	pll->clk_hw.init = &pll_init;

	ret = devm_clk_hw_register(dev, &pll->clk_hw);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to register pll clock\n");
		return ret;
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &pll->clk_hw);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to register clk provider: %d\n", ret);
		return ret;
	}

	phy = dev_get_drvdata(dev);
	phy->pll = pll;

	return 0;
}

static const char * const hdmi_phy_eliza_clk_names[] = {
	"iface",
	"ref",
	"xo",
};

static void hdmi_eliza_phy_txn_write(struct hdmi_phy *phy, u32 reg, u32 value)
{
	const int phy_txn_offset = REG_HDMI_ELIZA_PHY_TX1_CONTROL_0 - REG_HDMI_ELIZA_PHY_TX0_CONTROL_0;
	int i;

	for (i = 0; i < HDMI_PHY_TXN_CONTROL; i++)
		hdmi_phy_write(phy, reg + i * phy_txn_offset, value);
}

static void hdmi_eliza_phy_txn_update(struct hdmi_phy *phy, u32 reg, u32 mask, u32 value)
{
	const int phy_txn_offset = REG_HDMI_ELIZA_PHY_TX1_CONTROL_0 - REG_HDMI_ELIZA_PHY_TX0_CONTROL_0;
	int i;

	for (i = 0; i < HDMI_PHY_TXN_CONTROL; i++)
		hdmi_phy_update_bits(phy, reg + i * phy_txn_offset, mask, value);
}

static void hdmi_eliza_phy_configure(struct hdmi_phy *phy)
{
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_8, BIT(2), BIT(2));
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_TX_COMMON_CONTROL_0, BIT(0), BIT(0));

	usleep_range(5, 5000);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_REFCLK_CONTROL_0, BIT(0), BIT(0));
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_REFCLK_CONTROL_1, GENMASK(2, 0), 0x1);
	/* Select MPLLB */
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_3, BIT(0), 0);

	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_11, 0x18);
	/*
	 * MPLLB_CONTROL_12: missing in programming sequence, but present in
	 * downstream
	 */
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_12, 0x12);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_13, 0x18);
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MPLLB_CONTROL_16, BIT(1), BIT(1));
	/*
	 * MISC_CONTROL_2: missing in programming sequence, but present in
	 * downstream
	 */
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_2, 0x4d);
}

static void hdmi_eliza_phy_tx_lane_config(struct hdmi_phy *phy,
					  unsigned long pixclock)
{
	const unsigned int bpp = 24;

	/* Set BIT(3) for MPLLB and clear BIT(0) */
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_8, BIT(0) | BIT(3), BIT(3));
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_9, BIT(0) | BIT(1) | BIT(2), 0);
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_10, BIT(4) | BIT(5), BIT(4) | BIT(5));

	hdmi_eliza_phy_txn_write(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_7, 0);
	hdmi_eliza_phy_txn_write(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_6, 0);
	hdmi_eliza_phy_txn_write(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_5, 0x3e);

	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_10, BIT(3), 0);

	if (pixclock * bpp >= HDMI_PHY_MAX_BIT_RATE)
		hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_TX_COMMON_CONTROL_0, BIT(2), 0);
	else
		hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_TX_COMMON_CONTROL_0, BIT(2), BIT(2));

	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_9, BIT(3) | BIT(4), BIT(3) | BIT(4));
}

static void hdmi_eliza_phy_disable_hstx_reset(struct hdmi_phy *phy)
{
	hdmi_eliza_phy_txn_write(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_0, 0);

	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_RX1_CONFIG_15, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_RX2_CONFIG_15, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_REXT_CONTROL_2, 0);
}

static int hdmi_eliza_phy_ready_poll(struct hdmi_phy *phy, u32 reg, u32 mask, u32 val)
{
	u32 state;
	const u32 sleep_us = 2000;
	const u32 timeout_us = 3000000;

	if (readl_poll_timeout_atomic((phy->mmio + reg), state,
				      ((state & mask) == val),
				      sleep_us, timeout_us)) {
		DRM_ERROR("poll timeout, reg: 0x%x status=0x%x", reg, state);

		return -ETIMEDOUT;
	}

	return 0;
}

static int hdmi_eliza_phy_tx_ready_poll(struct hdmi_phy *phy)
{
	const int phy_txn_offset = REG_HDMI_ELIZA_PHY_TX1_CONTROL_0 - REG_HDMI_ELIZA_PHY_TX0_CONTROL_0;
	int ret = 0;

	/* Poll until TXn_ACK==0, downstream is wrong here */
	for (int i = 0; i < HDMI_PHY_TXN_CONTROL; i++) {
		ret |= hdmi_eliza_phy_ready_poll(phy,
						 REG_HDMI_ELIZA_PHY_TX0_CONTROL_2 + i * phy_txn_offset,
						 BIT(0), 0x0);
		if (ret)
			break;
	}

	return ret;
}

static int hdmi_eliza_phy_sram_init(struct hdmi_phy *phy)
{
	int ret;

	ret = hdmi_eliza_phy_ready_poll(phy, REG_HDMI_ELIZA_PHY_SRAM_CONTROL_1, BIT(0), BIT(0));
	if (ret)
		return ret;

	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_LSB, 0x3);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_MSB, 0xe0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_LSB, 0x18);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_MSB, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0x3);
	ndelay(400);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_LSB, 0x5);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_MSB, 0x90);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_LSB, 0xc);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_MSB, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0x3);
	ndelay(400);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0x0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_0, 0x7);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_LSB, 0x1a);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ADDRESS_MSB, 0xe0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_LSB, 0xa0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_WRDATA_MSB, 0);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0x3);
	ndelay(400);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_CR_ACCESS_CMD, 0);

	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_SRAM_CONTROL_0,
			     BIT(1), BIT(1));

	return 0;
}

static void hdmi_eliza_phy_init(struct hdmi_phy *phy,
				   unsigned long pixclock)
{

	/* pa_iso_disable() */
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_3,
			     BIT(1) | BIT(2) | BIT(3), 0);

	/* sw_reset() */
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_RESET_CONTROL_0, 0x1);

	/* assert_tx_lane_reset() */
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_9, BIT(5), BIT(5));

	/* phy_init_config() */
	hdmi_eliza_phy_configure(phy);

	hdmi_eliza_phy_tx_lane_config(phy, pixclock);

	hdmi_eliza_pll_configure_pll(phy->pll, 396000000, 19200000);

	/* sram_mode() (with SRAM init) */
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_SRAM_CONTROL_0,
			     BIT(0) | BIT(1), 0);
	/* qcsram_init() */
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_QCSRAM_CONFIG_3, BIT(0));
	usleep_range(5, 5000);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_QCSRAM_CONFIG_3, 0);

	/* deassert_tx_lane_reset() */
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_9, BIT(5), 0);

	hdmi_eliza_phy_disable_hstx_reset(phy);
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_RESET_CONTROL_0, 0);

	if (hdmi_eliza_phy_sram_init(phy))
		return;

	if (hdmi_eliza_phy_tx_ready_poll(phy))
		return;
};

static void hdmi_eliza_phy_powerup(struct hdmi_phy *phy,
				   unsigned long pixclock)
{
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_TX_COMMON_CONTROL_0, BIT(1), BIT(1));
}

static void hdmi_eliza_phy_deinit(struct hdmi_phy *phy)
{
	/* sw_reset() */
	hdmi_phy_write(phy, REG_HDMI_ELIZA_PHY_RESET_CONTROL_0, 1);
	hdmi_eliza_phy_txn_write(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_0, 1);
	/* assert_tx_lane_reset() */
	hdmi_eliza_phy_txn_update(phy, REG_HDMI_ELIZA_PHY_TX0_CONTROL_9, BIT(5), BIT(5));
	/* pa_iso_enable() */
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_MISC_CONTROL_3,
			     BIT(1) | BIT(2) | BIT(3), BIT(1) | BIT(2) | BIT(3));
}

static void hdmi_eliza_phy_powerdown(struct hdmi_phy *phy)
{
	hdmi_phy_update_bits(phy, REG_HDMI_ELIZA_PHY_TX_COMMON_CONTROL_0, BIT(1), 0);
}

const struct hdmi_phy_cfg msm_hdmi_phy_eliza_cfg = {
	.type = MSM_HDMI_PHY_ELIZA,
	.init = hdmi_eliza_phy_init,
	.deinit = hdmi_eliza_phy_deinit,
	.powerup = hdmi_eliza_phy_powerup,
	.powerdown = hdmi_eliza_phy_powerdown,
	.clk_names = hdmi_phy_eliza_clk_names,
	.num_clks = ARRAY_SIZE(hdmi_phy_eliza_clk_names),
};
