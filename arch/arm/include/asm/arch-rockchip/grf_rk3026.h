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
 * UART2 iomux: GPIO1B[3:2] → FUNC2 selects UART2_TX / UART2_RX
 * GRF_GPIO1B_IOMUX bits[7:4]: 0b1010 = UART2
 */
#define IOMUX_UART2		(0xf << (4 + 16) | (0xa << 4))

#endif /* _ASM_ARCH_GRF_RK3026_H */
