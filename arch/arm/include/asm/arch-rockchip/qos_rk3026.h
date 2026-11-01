/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RK3026 NoC QoS generators (the vendor 3.0 kernel's
 * mach-rk3026/include/mach/cpu_axi.h: CPU AXI bus at 0x10128000, one QoS
 * block per master port).
 */
#ifndef _ASM_ARCH_QOS_RK3026_H
#define _ASM_ARCH_QOS_RK3026_H

#define CPU_AXI_QOS_PRIORITY	0x08

#define CPU0_QOS		0x1012a000
#define CPU1W_QOS		0x1012a080
#define CPU1R_QOS		0x1012a100
#define PERI_QOS		0x1012c000

#endif
