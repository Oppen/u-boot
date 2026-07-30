// SPDX-License-Identifier: GPL-2.0+
/*
 * Pinctrl driver for Rockchip RK3026 (RK2928 class).
 *
 * Every constant here is device-tier, read off a live RK3026 and recorded in
 * docs/HARDWARE-CONSTANTS.md §6 and "GRF write convention":
 *
 *   iomux  GRF + 0xa8 .. 0xe4, 2 bits per pin, 4 groups per bank, 4 banks
 *   pull   GRF + 0x118 .. 0x134, 1 bit per pin, 16 pins per register
 *          1 = pull DISABLED
 *   both banks are masked-write: the field mask must be set in bits [31:16]
 *          or the write is a silent no-op
 *
 * Pull direction is not selectable on this pad class -- one bit selects the
 * pad's own built-in pull on or off -- so only PIN_CONFIG_BIAS_PULL_PIN_DEFAULT
 * and BIAS_DISABLE can be honoured.
 */

#include <dm.h>
#include <dm/pinctrl.h>
#include <regmap.h>
#include <syscon.h>
#include <linux/bitops.h>

#include "pinctrl-rockchip.h"

static int rk3026_set_mux(struct rockchip_pin_bank *bank, int pin, int mux)
{
	struct rockchip_pinctrl_priv *priv = bank->priv;
	int iomux_num = (pin / 8);
	struct regmap *regmap;
	int reg, ret, mask, mux_type;
	u8 bit;
	u32 data;

	regmap = (bank->iomux[iomux_num].type & IOMUX_SOURCE_PMU)
				? priv->regmap_pmu : priv->regmap_base;

	mux_type = bank->iomux[iomux_num].type;
	reg = bank->iomux[iomux_num].offset;
	reg += rockchip_get_mux_data(mux_type, pin, &bit, &mask);

	data = (mask << (bit + 16));
	data |= (mux & mask) << bit;
	ret = regmap_write(regmap, reg, data);

	return ret;
}

#define RK3026_PULL_OFFSET		0x118
#define RK3026_PULL_PINS_PER_REG	16
#define RK3026_PULL_BANK_STRIDE		8

static void rk3026_calc_pull_reg_and_bit(struct rockchip_pin_bank *bank,
					 int pin_num, struct regmap **regmap,
					 int *reg, u8 *bit)
{
	struct rockchip_pinctrl_priv *priv = bank->priv;

	*regmap = priv->regmap_base;
	*reg = RK3026_PULL_OFFSET;
	*reg += bank->bank_num * RK3026_PULL_BANK_STRIDE;
	*reg += (pin_num / RK3026_PULL_PINS_PER_REG) * 4;

	*bit = pin_num % RK3026_PULL_PINS_PER_REG;
};

static int rk3026_set_pull(struct rockchip_pin_bank *bank,
			   int pin_num, int pull)
{
	struct regmap *regmap;
	int reg, ret;
	u8 bit;
	u32 data;

	if (pull != PIN_CONFIG_BIAS_PULL_PIN_DEFAULT &&
	    pull != PIN_CONFIG_BIAS_DISABLE)
		return -ENOTSUPP;

	rk3026_calc_pull_reg_and_bit(bank, pin_num, &regmap, &reg, &bit);
	data = BIT(bit + 16);
	if (pull == PIN_CONFIG_BIAS_DISABLE)
		data |= BIT(bit);
	ret = regmap_write(regmap, reg, data);

	return ret;
}

static struct rockchip_pin_bank rk3026_pin_banks[] = {
	PIN_BANK(0, 32, "gpio0"),
	PIN_BANK(1, 32, "gpio1"),
	PIN_BANK(2, 32, "gpio2"),
	PIN_BANK(3, 32, "gpio3"),
};

static const struct rockchip_pin_ctrl rk3026_pin_ctrl = {
	.pin_banks		= rk3026_pin_banks,
	.nr_banks		= ARRAY_SIZE(rk3026_pin_banks),
	.grf_mux_offset		= 0xa8,
	.set_mux		= rk3026_set_mux,
	.set_pull		= rk3026_set_pull,
};

static const struct udevice_id rk3026_pinctrl_ids[] = {
	{
		.compatible = "rockchip,rk3026-pinctrl",
		.data = (ulong)&rk3026_pin_ctrl
	},
	{}
};

U_BOOT_DRIVER(pinctrl_rk3026) = {
	.name		= "pinctrl_rk3026",
	.id		= UCLASS_PINCTRL,
	.of_match	= rk3026_pinctrl_ids,
	.priv_auto	= sizeof(struct rockchip_pinctrl_priv),
	.ops		= &rockchip_pinctrl_ops,
#if CONFIG_IS_ENABLED(OF_REAL)
	.bind		= dm_scan_fdt_dev,
#endif
	.probe		= rockchip_pinctrl_probe,
};
