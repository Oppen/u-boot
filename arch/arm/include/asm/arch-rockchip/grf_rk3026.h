/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RK3026 General Register Files (GRF) layout
 *
 * Copyright (c) 2024, Mario Rugiero
 * Sources: arch/arm/mach-rk3026/include/mach/grf.h (vendor kernel)
 */

#ifndef _ASM_ARCH_GRF_RK3026_H
#define _ASM_ARCH_GRF_RK3026_H

#define RK3026_GRF_BASE		0x20008000

/*
 * Register-layout view of the GRF, needed by the DDR init code (soc_con2 and
 * os_reg[]).  Layout is identical to rk3036 (same RK2928-generation die) and
 * cross-checks against the vendor kernel's mach-rk3026/include/mach/grf.h:
 * SOC_CON0 0x140, DDRC_STAT 0x19c, OS_REG0 0x1c8.
 */
struct rk3026_grf {
	unsigned int reserved[0x2a];
	unsigned int gpio0a_iomux;
	unsigned int gpio0b_iomux;
	unsigned int gpio0c_iomux;
	unsigned int gpio0d_iomux;

	unsigned int gpio1a_iomux;
	unsigned int gpio1b_iomux;
	unsigned int gpio1c_iomux;
	unsigned int gpio1d_iomux;

	unsigned int gpio2a_iomux;
	unsigned int gpio2b_iomux;
	unsigned int gpio2c_iomux;
	unsigned int gpio2d_iomux;

	unsigned int reserved2[0x0a];
	unsigned int gpiods;
	unsigned int reserved3[0x05];
	unsigned int gpio0l_pull;
	unsigned int gpio0h_pull;
	unsigned int gpio1l_pull;
	unsigned int gpio1h_pull;
	unsigned int gpio2l_pull;
	unsigned int gpio2h_pull;
	unsigned int reserved4[4];
	unsigned int soc_con0;
	unsigned int soc_con1;
	unsigned int soc_con2;
	unsigned int soc_status0;
	unsigned int reserved5;
	unsigned int soc_con3;
	unsigned int reserved6;
	unsigned int dmac_con0;
	unsigned int dmac_con1;
	unsigned int dmac_con2;
	unsigned int reserved7[5];
	unsigned int uoc0_con5;
	unsigned int reserved8[4];
	unsigned int uoc1_con4;
	unsigned int uoc1_con5;
	unsigned int reserved9;
	unsigned int ddrc_stat;
	unsigned int uoc_con6;
	unsigned int soc_status1;
	unsigned int cpu_con0;
	unsigned int cpu_con1;
	unsigned int cpu_con2;
	unsigned int cpu_con3;
	unsigned int reserved10;
	unsigned int reserved11;
	unsigned int cpu_status0;
	unsigned int cpu_status1;
	unsigned int os_reg[8];
	unsigned int reserved12[6];
	unsigned int dll_con[4];
	unsigned int dll_status[4];
	unsigned int dfi_wrnum;
	unsigned int dfi_rdnum;
	unsigned int dfi_actnum;
	unsigned int dfi_timerval;
	unsigned int nfi_fifo[4];
	unsigned int reserved13[0x10];
	unsigned int usbphy0_con[8];
	unsigned int usbphy1_con[8];
	unsigned int reserved14[0x10];
	unsigned int chip_tag;
	unsigned int sdmmc_det_cnt;
};
check_member(rk3026_grf, sdmmc_det_cnt, 0x304);
/* both proven from the vendor DDR blob: 0x20008148 and 0x200081c8 */
check_member(rk3026_grf, soc_con2, 0x148);
check_member(rk3026_grf, os_reg[0], 0x1c8);


/* GPIO iomux configuration registers */
#define GRF_GPIO0A_IOMUX	0x00a8
#define GRF_GPIO0B_IOMUX	0x00ac
#define GRF_GPIO0C_IOMUX	0x00b0
#define GRF_GPIO0D_IOMUX	0x00b4
#define GRF_GPIO1A_IOMUX	0x00b8
#define GRF_GPIO1B_IOMUX	0x00bc
#define GRF_GPIO1C_IOMUX	0x00c0
#define GRF_GPIO1D_IOMUX	0x00c4
#define GRF_GPIO2A_IOMUX	0x00c8
#define GRF_GPIO2B_IOMUX	0x00cc
#define GRF_GPIO2C_IOMUX	0x00d0
#define GRF_GPIO2D_IOMUX	0x00d4
#define GRF_GPIO3C_IOMUX	0x00e0
#define GRF_GPIO3D_IOMUX	0x00e4

/* Pull resistor control */
#define GRF_GPIO0L_PULL		0x0118
#define GRF_GPIO0H_PULL		0x011c
#define GRF_GPIO1L_PULL		0x0120
#define GRF_GPIO1H_PULL		0x0124
#define GRF_GPIO2L_PULL		0x0128
#define GRF_GPIO2H_PULL		0x012c
#define GRF_GPIO3L_PULL		0x0130
#define GRF_GPIO3H_PULL		0x0134

/* SoC control */
#define GRF_SOC_CON0		0x0140
#define GRF_SOC_CON1		0x0144
#define GRF_SOC_CON2		0x0148
#define GRF_SOC_STATUS0		0x014c

/* USB OTG controller 0 — PHY at GRF offset 0x17c */
#define GRF_UOC0_CON0		0x017c

/* USB OTG controller 1 — PHY at GRF offset 0x190 */
#define GRF_UOC1_CON0		0x0190
#define GRF_UOC1_CON1		0x0194

/* USB PHY control bits (shared between UOC0 and UOC1) */
#define UOC_CON0_SUSPEND	BIT(0)	/* PHY suspend */
#define UOC_CON0_DISABLE	BIT(4)	/* PHY disable */
#define UOC_CON0_COMMON_ON	BIT(8)	/* common block power down */

/*
 * UART2 iomux: GPIO2_C6 = UART2_SIN, GPIO2_C7 = UART2_SOUT, both FUNC3.
 * Source: vendor kernel arch/arm/mach-rk3026/include/mach/iomux.h
 *	GPIO2_C6 = 0x2c60, LCDC0_D20, EBC_BORDER0, UART2_SIN
 *	GPIO2_C7 = 0x2c70, LCDC0_D21, EBC_BORDER1, UART2_SOUT
 * Fields are 2 bits per pin in a hiword-masked register (plat-rk/iomux.c:
 * v = mode << (pin * 2) | 3 << (pin * 2 + 16)), so pin6 = bits[13:12],
 * pin7 = bits[15:14].
 *
 * NOTE: FUNC2 on the same two pins is EBC_BORDER0/1 — the serial console and
 * the EBC border pins are mutually exclusive on this SoC.
 */
#define IOMUX_UART2		(0xf << (12 + 16) | (3 << 12) | (3 << 14))

#endif /* _ASM_ARCH_GRF_RK3026_H */
