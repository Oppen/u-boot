// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2015 Google, Inc
 *
 * (C) Copyright 2008-2014 Rockchip Electronics
 * Peter, Software Engineering, <superpeter.cai@gmail.com>.
 */

#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <i2c.h>
#include <log.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/i2c.h>
#include <asm/arch-rockchip/periph.h>
#include <dm/pinctrl.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/sizes.h>

/* i2c timerout */
#define I2C_TIMEOUT_MS		100
#define I2C_RETRY_COUNT		3

/* rk i2c fifo max transfer bytes */
#define RK_I2C_FIFO_SIZE	32

struct rk_i2c {
	struct clk clk;
	struct i2c_regs *regs;
	unsigned int speed;
};

enum {
	RK_I2C_LEGACY,
	RK_I2C_NEW,
};

/**
 * @controller_type: i2c controller type
 * @grf_i2c_sel_reg: GRF offset of the RKI2C controller-select register, or 0 if
 *	this SoC has no such select. Some parts wire each channel to either the
 *	legacy Synopsys DW-I2C block or Rockchip's own RKI2C, chosen by a
 *	hiword-masked GRF bit that no bootloader stage sets.
 * @grf_i2c_sel_shift: bit position of channel 0's select; channel n is
 *	@grf_i2c_sel_shift + n. 1 selects RKI2C.
 */
struct rk_i2c_soc_data {
	int controller_type;
	int grf_i2c_sel_reg;
	int grf_i2c_sel_shift;
};

static inline void rk_i2c_get_div(int div, int *divh, int *divl)
{
	*divl = div / 2;
	if (div % 2 == 0)
		*divh = div / 2;
	else
		*divh = DIV_ROUND_UP(div, 2);
}

/*
 * SCL Divisor = 8 * (CLKDIVL+1 + CLKDIVH+1)
 * SCL = PCLK / SCLK Divisor
 * i2c_rate = PCLK
 */
static int rk_i2c_set_clk(struct rk_i2c *i2c, uint32_t scl_rate)
{
	ulong i2c_rate;
	int div, divl, divh;

	/* First get i2c rate from pclk */
	i2c_rate = clk_get_rate(&i2c->clk);

	/*
	 * clk_get_rate() returns a negative errno cast to ulong when the clock
	 * driver does not model this id, so it MUST be tested -- unchecked, an
	 * -ENOENT arrives as 0xfffffffe and the divider below comes out around
	 * 2683, dropping SCL to roughly 3 kHz off a 66 MHz PCLK. Measured exactly
	 * that way on an rk3026 on 2026-07-30: every transfer crawled and `i2c
	 * probe` returned an empty list while the pads and the controller select
	 * were independently verified correct.
	 *
	 * A rate of 0 is just as bad and is NOT caught by IS_ERR_VALUE: it makes
	 * div negative, the `if (div >= 0)` guard below then leaves CLKDIV at 0,
	 * and the controller runs at PCLK/16 -- about 4.1 MHz. That guard is not
	 * an error check; it silently converts a bad rate into a bad clock.
	 */
	if (IS_ERR_VALUE(i2c_rate) || !i2c_rate) {
		log_err("%s: bad parent clock rate %ld\n", __func__, i2c_rate);
		return -EINVAL;
	}

	div = DIV_ROUND_UP(i2c_rate, scl_rate * 8) - 2;
	divh = 0;
	divl = 0;
	if (div >= 0)
		rk_i2c_get_div(div, &divh, &divl);
	writel(I2C_CLKDIV_VAL(divl, divh), &i2c->regs->clkdiv);

	debug("rk_i2c_set_clk: i2c rate = %ld, scl rate = %d\n", i2c_rate,
	      scl_rate);
	debug("set i2c clk div = %d, divh = %d, divl = %d\n", div, divh, divl);
	debug("set clk(I2C_CLKDIV: 0x%08x)\n", readl(&i2c->regs->clkdiv));

	return 0;
}

static void rk_i2c_show_regs(struct i2c_regs *regs)
{
#ifdef DEBUG
	uint i;

	debug("i2c_con: 0x%08x\n", readl(&regs->con));
	debug("i2c_clkdiv: 0x%08x\n", readl(&regs->clkdiv));
	debug("i2c_mrxaddr: 0x%08x\n", readl(&regs->mrxaddr));
	debug("i2c_mrxraddR: 0x%08x\n", readl(&regs->mrxraddr));
	debug("i2c_mtxcnt: 0x%08x\n", readl(&regs->mtxcnt));
	debug("i2c_mrxcnt: 0x%08x\n", readl(&regs->mrxcnt));
	debug("i2c_ien: 0x%08x\n", readl(&regs->ien));
	debug("i2c_ipd: 0x%08x\n", readl(&regs->ipd));
	debug("i2c_fcnt: 0x%08x\n", readl(&regs->fcnt));
	for (i = 0; i < 8; i++)
		debug("i2c_txdata%d: 0x%08x\n", i, readl(&regs->txdata[i]));
	for (i = 0; i < 8; i++)
		debug("i2c_rxdata%d: 0x%08x\n", i, readl(&regs->rxdata[i]));
#endif
}

static int rk_i2c_send_start_bit(struct rk_i2c *i2c)
{
	struct i2c_regs *regs = i2c->regs;
	ulong start;

	debug("I2c Send Start bit.\n");
	writel(I2C_IPD_ALL_CLEAN, &regs->ipd);

	writel(I2C_CON_EN | I2C_CON_START, &regs->con);
	writel(I2C_STARTIEN, &regs->ien);

	start = get_timer(0);
	while (1) {
		if (readl(&regs->ipd) & I2C_STARTIPD) {
			writel(I2C_STARTIPD, &regs->ipd);
			break;
		}
		if (get_timer(start) > I2C_TIMEOUT_MS) {
			debug("I2C Send Start Bit Timeout\n");
			rk_i2c_show_regs(regs);
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static int rk_i2c_send_stop_bit(struct rk_i2c *i2c)
{
	struct i2c_regs *regs = i2c->regs;
	ulong start;

	debug("I2c Send Stop bit.\n");
	writel(I2C_IPD_ALL_CLEAN, &regs->ipd);

	writel(I2C_CON_EN | I2C_CON_STOP, &regs->con);
	writel(I2C_STOPIEN, &regs->ien);

	start = get_timer(0);
	while (1) {
		if (readl(&regs->ipd) & I2C_STOPIPD) {
			writel(I2C_STOPIPD, &regs->ipd);
			break;
		}
		if (get_timer(start) > I2C_TIMEOUT_MS) {
			debug("I2C Send Start Bit Timeout\n");
			rk_i2c_show_regs(regs);
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	return 0;
}

static inline void rk_i2c_disable(struct rk_i2c *i2c)
{
	writel(0, &i2c->regs->con);
}

static int rk_i2c_read(struct rk_i2c *i2c, uchar chip, uint reg, uint r_len,
		       uchar *buf, uint b_len)
{
	struct i2c_regs *regs = i2c->regs;
	uchar *pbuf = buf;
	uint bytes_remain_len = b_len;
	uint bytes_xferred = 0;
	uint words_xferred = 0;
	ulong start;
	uint con = 0;
	uint rxdata;
	uint i, j;
	int err;
	bool snd_chunk = false;

	debug("rk_i2c_read: chip = %d, reg = %d, r_len = %d, b_len = %d\n",
	      chip, reg, r_len, b_len);

	err = rk_i2c_send_start_bit(i2c);
	if (err)
		return err;

	writel(I2C_MRXADDR_SET(1, chip << 1 | 1), &regs->mrxaddr);
	if (r_len == 0) {
		writel(0, &regs->mrxraddr);
	} else if (r_len < 4) {
		writel(I2C_MRXRADDR_SET(r_len, reg), &regs->mrxraddr);
	} else {
		debug("I2C Read: addr len %d not supported\n", r_len);
		return -EIO;
	}

	while (bytes_remain_len) {
		if (bytes_remain_len > RK_I2C_FIFO_SIZE) {
			/*
			 * The hw can read up to 32 bytes at a time. If we need
			 * more than one chunk, send an ACK after the last byte
			 * of the current chunk.
			 */
			con = I2C_CON_EN;
			bytes_xferred = 32;
		} else {
			con = I2C_CON_EN | I2C_CON_LASTACK;
			bytes_xferred = bytes_remain_len;
		}
		words_xferred = DIV_ROUND_UP(bytes_xferred, 4);

		/*
		 * make sure we are in plain RX mode if we read a second chunk
		 */
		if (snd_chunk)
			con |= I2C_CON_MOD(I2C_MODE_RX);
		else
			con |= I2C_CON_MOD(I2C_MODE_TRX);

		writel(con, &regs->con);
		writel(bytes_xferred, &regs->mrxcnt);
		writel(I2C_MBRFIEN | I2C_NAKRCVIEN, &regs->ien);

		start = get_timer(0);
		while (1) {
			if (readl(&regs->ipd) & I2C_NAKRCVIPD) {
				writel(I2C_NAKRCVIPD, &regs->ipd);
				err = -EREMOTEIO;
			}
			if (readl(&regs->ipd) & I2C_MBRFIPD) {
				writel(I2C_MBRFIPD, &regs->ipd);
				break;
			}
			if (get_timer(start) > I2C_TIMEOUT_MS) {
				debug("I2C Read Data Timeout\n");
				err =  -ETIMEDOUT;
				rk_i2c_show_regs(regs);
				goto i2c_exit;
			}
			udelay(1);
		}

		for (i = 0; i < words_xferred; i++) {
			rxdata = readl(&regs->rxdata[i]);
			debug("I2c Read RXDATA[%d] = 0x%x\n", i, rxdata);
			for (j = 0; j < 4; j++) {
				if ((i * 4 + j) == bytes_xferred)
					break;
				*pbuf++ = (rxdata >> (j * 8)) & 0xff;
			}
		}

		bytes_remain_len -= bytes_xferred;
		snd_chunk = true;
		debug("I2C Read bytes_remain_len %d\n", bytes_remain_len);
	}

i2c_exit:
	return err;
}

static int rk_i2c_write(struct rk_i2c *i2c, uchar chip, uint reg, uint r_len,
			uchar *buf, uint b_len)
{
	struct i2c_regs *regs = i2c->regs;
	int err;
	uchar *pbuf = buf;
	uint bytes_remain_len = b_len + r_len + 1;
	uint bytes_xferred = 0;
	uint words_xferred = 0;
	ulong start;
	uint txdata;
	uint i, j;

	debug("rk_i2c_write: chip = %d, reg = %d, r_len = %d, b_len = %d\n",
	      chip, reg, r_len, b_len);
	err = rk_i2c_send_start_bit(i2c);
	if (err)
		return err;

	while (bytes_remain_len) {
		if (bytes_remain_len > RK_I2C_FIFO_SIZE)
			bytes_xferred = RK_I2C_FIFO_SIZE;
		else
			bytes_xferred = bytes_remain_len;
		words_xferred = DIV_ROUND_UP(bytes_xferred, 4);

		for (i = 0; i < words_xferred; i++) {
			txdata = 0;
			for (j = 0; j < 4; j++) {
				if ((i * 4 + j) == bytes_xferred)
					break;

				if (i == 0 && j == 0 && pbuf == buf) {
					txdata |= (chip << 1);
				} else if (i == 0 && j <= r_len && pbuf == buf) {
					txdata |= (reg &
						(0xff << ((j - 1) * 8))) << 8;
				} else {
					txdata |= (*pbuf++)<<(j * 8);
				}
			}
			writel(txdata, &regs->txdata[i]);
			debug("I2c Write TXDATA[%d] = 0x%08x\n", i, txdata);
		}

		writel(I2C_CON_EN | I2C_CON_MOD(I2C_MODE_TX), &regs->con);
		writel(bytes_xferred, &regs->mtxcnt);
		writel(I2C_MBTFIEN | I2C_NAKRCVIEN, &regs->ien);

		start = get_timer(0);
		while (1) {
			if (readl(&regs->ipd) & I2C_NAKRCVIPD) {
				writel(I2C_NAKRCVIPD, &regs->ipd);
				err = -EREMOTEIO;
			}
			if (readl(&regs->ipd) & I2C_MBTFIPD) {
				writel(I2C_MBTFIPD, &regs->ipd);
				break;
			}
			if (get_timer(start) > I2C_TIMEOUT_MS) {
				debug("I2C Write Data Timeout\n");
				err =  -ETIMEDOUT;
				rk_i2c_show_regs(regs);
				goto i2c_exit;
			}
			udelay(1);
		}

		bytes_remain_len -= bytes_xferred;
		debug("I2C Write bytes_remain_len %d\n", bytes_remain_len);
	}

i2c_exit:
	return err;
}

static int rockchip_i2c_xfer(struct udevice *bus, struct i2c_msg *msg,
			     int nmsgs)
{
	struct rk_i2c *i2c = dev_get_priv(bus);
	int ret = 0;

	debug("i2c_xfer: %d messages\n", nmsgs);
	for (; nmsgs > 0; nmsgs--, msg++) {
		debug("i2c_xfer: chip=0x%x, len=0x%x\n", msg->addr, msg->len);
		if (msg->flags & I2C_M_RD) {
			ret = rk_i2c_read(i2c, msg->addr, 0, 0, msg->buf,
					  msg->len);
		} else {
			ret = rk_i2c_write(i2c, msg->addr, 0, 0, msg->buf,
					   msg->len);
		}
		if (ret) {
			debug("i2c_write: error sending\n");
			ret = -EREMOTEIO;
			break;
		}

		/*
		 * The HW is actually not capable of REPEATED START. But we can
		 * get the intended effect by resetting its internal state
		 * and issuing an ordinary START.
		 *
		 * Do NOT disable the controller after the last message (before
		 * sending the STOP condition) as this triggers an illegal
		 * START condition followed by a STOP condition.
		 */
		if (nmsgs > 1)
			rk_i2c_disable(i2c);
	}

	rk_i2c_send_stop_bit(i2c);
	rk_i2c_disable(i2c);

	return ret;
}

int rockchip_i2c_set_bus_speed(struct udevice *bus, unsigned int speed)
{
	struct rk_i2c *i2c = dev_get_priv(bus);

	return rk_i2c_set_clk(i2c, speed);
}

static int rockchip_i2c_of_to_plat(struct udevice *bus)
{
	struct rk_i2c *priv = dev_get_priv(bus);
	int ret;

	ret = clk_get_by_index(bus, 0, &priv->clk);
	if (ret < 0) {
		debug("%s: Could not get clock for %s: %d\n", __func__,
		      bus->name, ret);
		return ret;
	}

	return 0;
}

static int rockchip_i2c_probe(struct udevice *bus)
{
	struct rk_i2c *priv = dev_get_priv(bus);
	struct rk_i2c_soc_data *soc_data;
	struct udevice *pinctrl;
	int bus_nr;
	int ret;

	priv->regs = dev_read_addr_ptr(bus);

	soc_data = (struct rk_i2c_soc_data*)dev_get_driver_data(bus);

	/*
	 * Select Rockchip's own RKI2C block for this channel. Needed on parts
	 * that mux each channel against the legacy Synopsys DW-I2C block, where
	 * no bootloader stage sets the bit and the SoC's own reset clears it --
	 * so without this the controller answers nothing after a warm reset that
	 * wipes the GRF, while appearing to work on a boot that inherited the
	 * bit from a previously running kernel.
	 */
	if (soc_data->grf_i2c_sel_reg) {
		void *grf = syscon_get_first_range(ROCKCHIP_SYSCON_GRF);

		if (IS_ERR(grf))
			return PTR_ERR(grf);

		ret = dev_read_alias_seq(bus, &bus_nr);
		if (ret < 0) {
			debug("%s: Could not get alias for %s: %d\n",
			      __func__, bus->name, ret);
			return ret;
		}

		rk_clrsetreg(grf + soc_data->grf_i2c_sel_reg,
			     BIT(soc_data->grf_i2c_sel_shift + bus_nr),
			     BIT(soc_data->grf_i2c_sel_shift + bus_nr));
	}

	if (soc_data->controller_type == RK_I2C_LEGACY) {
		ret = dev_read_alias_seq(bus, &bus_nr);
		if (ret < 0) {
			debug("%s: Could not get alias for %s: %d\n",
			 __func__, bus->name, ret);
			return ret;
		}

		ret = uclass_get_device(UCLASS_PINCTRL, 0, &pinctrl);
		if (ret) {
			debug("%s: Cannot find pinctrl device\n", __func__);
			return ret;
		}

		/* pinctrl will switch I2C to new type */
		ret = pinctrl_request_noflags(pinctrl, PERIPH_ID_I2C0 + bus_nr);
		if (ret) {
			debug("%s: Failed to switch I2C to new type %s: %d\n",
				__func__, bus->name, ret);
			return ret;
		}
	}

	return 0;
}

static const struct dm_i2c_ops rockchip_i2c_ops = {
	.xfer		= rockchip_i2c_xfer,
	.set_bus_speed	= rockchip_i2c_set_bus_speed,
};

/*
 * RK3026 (RK2928 class) has both blocks, at separate windows: the DW-I2C ones at
 * 0x20070000+ read all zeros while the RKI2C ones at 0x20072000+ carry live
 * traffic. It still needs the GRF select -- the vendor kernel's i2c_adap_sel()
 * writes GRF_SOC_CON1 bit (11 + channel) at probe, and no bootloader stage
 * touches that register. See docs/HARDWARE-CONSTANTS.md.
 *
 * RK_I2C_NEW because the LEGACY path's pinctrl_request_noflags() cannot work:
 * no rockchip pinctrl driver has implemented a .request op since 2017.
 */
static const struct rk_i2c_soc_data rk3026_soc_data = {
	.controller_type = RK_I2C_NEW,
	.grf_i2c_sel_reg = 0x144,
	.grf_i2c_sel_shift = 11,
};

static const struct rk_i2c_soc_data rk3066_soc_data = {
	.controller_type = RK_I2C_LEGACY,
};

static const struct rk_i2c_soc_data rk3188_soc_data = {
	.controller_type = RK_I2C_LEGACY,
};

static const struct rk_i2c_soc_data rk3228_soc_data = {
	.controller_type = RK_I2C_NEW,
};

static const struct rk_i2c_soc_data rk3288_soc_data = {
	.controller_type = RK_I2C_NEW,
};

static const struct rk_i2c_soc_data rk3328_soc_data = {
	.controller_type = RK_I2C_NEW,
};

static const struct rk_i2c_soc_data rk3399_soc_data = {
	.controller_type = RK_I2C_NEW,
};

static const struct udevice_id rockchip_i2c_ids[] = {
	/*
	 * First: U-Boot matches in THIS array's order, not in the order the node
	 * lists its compatibles. Our nodes carry
	 * "rockchip,rk3026-i2c", "rockchip,rk3288-i2c", and the rk3288 entry
	 * would otherwise win and skip the GRF select.
	 */
	{
		.compatible = "rockchip,rk3026-i2c",
		.data = (ulong)&rk3026_soc_data,
	},
	{
		.compatible = "rockchip,rk3066-i2c",
		.data = (ulong)&rk3066_soc_data,
	},
	{
		.compatible = "rockchip,rk3188-i2c",
		.data = (ulong)&rk3188_soc_data,
	},
	{
		.compatible = "rockchip,rk3228-i2c",
		.data = (ulong)&rk3228_soc_data,
	},
	{
		.compatible = "rockchip,rk3288-i2c",
		.data = (ulong)&rk3288_soc_data,
	},
	{
		.compatible = "rockchip,rk3328-i2c",
		.data = (ulong)&rk3328_soc_data,
	},
	{
		.compatible = "rockchip,rk3399-i2c",
		.data = (ulong)&rk3399_soc_data,
	},
	{ }
};

U_BOOT_DRIVER(rockchip_rk3066_i2c) = {
	.name	= "rockchip_rk3066_i2c",
	.id	= UCLASS_I2C,
	.of_match = rockchip_i2c_ids,
	.of_to_plat = rockchip_i2c_of_to_plat,
	.probe	= rockchip_i2c_probe,
	.priv_auto	= sizeof(struct rk_i2c),
	.ops	= &rockchip_i2c_ops,
};

DM_DRIVER_ALIAS(rockchip_rk3066_i2c, rockchip_rk3288_i2c)
