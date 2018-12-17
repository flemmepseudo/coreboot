/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2007-2010 coresystems GmbH
 * Copyright (C) 2011 Google Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <console/console.h>
#include <arch/io.h>
#include <device/pci_def.h>
#include <elog.h>
#include "haswell.h"

static void haswell_setup_bars(void)
{
	printk(BIOS_DEBUG, "Setting up static northbridge registers...");
	/* Set up all hardcoded northbridge BARs */
	pci_write_config32(PCI_DEV(0, 0x00, 0), EPBAR, DEFAULT_EPBAR | 1);
	pci_write_config32(PCI_DEV(0, 0x00, 0), EPBAR + 4, (0LL+DEFAULT_EPBAR) >> 32);
	pci_write_config32(PCI_DEV(0, 0x00, 0), MCHBAR, DEFAULT_MCHBAR | 1);
	pci_write_config32(PCI_DEV(0, 0x00, 0), MCHBAR + 4, (0LL+DEFAULT_MCHBAR) >> 32);
	pci_write_config32(PCI_DEV(0, 0x00, 0), DMIBAR, (uintptr_t)DEFAULT_DMIBAR | 1);
	pci_write_config32(PCI_DEV(0, 0x00, 0), DMIBAR + 4, (0LL+(uintptr_t)DEFAULT_DMIBAR) >> 32);

	/* Set C0000-FFFFF to access RAM on both reads and writes */
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM0, 0x30);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM1, 0x33);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM2, 0x33);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM3, 0x33);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM4, 0x33);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM5, 0x33);
	pci_write_config8(PCI_DEV(0, 0x00, 0), PAM6, 0x33);

	printk(BIOS_DEBUG, " done.\n");
}

static void haswell_setup_graphics(void)
{
	bool igd_enabled;
	u16 ggc;
	u8 reg8;

	printk(BIOS_DEBUG, "Initializing Graphics...\n");

	igd_enabled = !!(pci_read_config32(PCI_DEV(0, 0, 0), DEVEN)
			 & DEVEN_D2EN);

	ggc = pci_read_config16(PCI_DEV(0, 0, 0), GGC);
	ggc &= ~0x3f8;
	if (igd_enabled) {
		ggc |= GGC_GTT_2MB | GGC_IGD_MEM_IN_32MB_UNITS(1);
		ggc &= ~GGC_DISABLE_VGA_IO_DECODE;
	} else {
		ggc |= GGC_GTT_0MB | GGC_IGD_MEM_IN_32MB_UNITS(0) |
		       GGC_DISABLE_VGA_IO_DECODE;
	}
	pci_write_config16(PCI_DEV(0, 0, 0), GGC, ggc);

	if (!igd_enabled) {
		printk(BIOS_DEBUG, "IGD is disabled.\n");
		return;
	}

	/* Enable 256MB aperture */
	reg8 = pci_read_config8(PCI_DEV(0, 2, 0), MSAC);
	reg8 &= ~0x06;
	reg8 |= 0x02;
	pci_write_config8(PCI_DEV(0, 2, 0), MSAC, reg8);
}

static void haswell_setup_misc(void)
{
	u32 reg32;

	/* Erratum workarounds */
	reg32 = MCHBAR32(0x5f00);
	reg32 |= (1 << 9)|(1 << 10);
	MCHBAR32(0x5f00) = reg32;

	/* Enable SA Clock Gating */
	reg32 = MCHBAR32(0x5f00);
	MCHBAR32(0x5f00) = reg32 | 1;

	/* GPU RC6 workaround for sighting 366252 */
	reg32 = MCHBAR32(0x5d14);
	reg32 |= (1UL << 31);
	MCHBAR32(0x5d14) = reg32;

	/* VLW */
	reg32 = MCHBAR32(0x6120);
	reg32 &= ~(1 << 0);
	MCHBAR32(0x6120) = reg32;

	reg32 = MCHBAR32(0x5418);
	reg32 |= (1 << 4) | (1 << 5);
	MCHBAR32(0x5418) = reg32;
}

static void haswell_setup_iommu(void)
{
	const u32 capid0_a = pci_read_config32(PCI_DEV(0, 0, 0), CAPID0_A);

	if (capid0_a & VTD_DISABLE)
		return;

	/* setup BARs: zeroize top 32 bits; set enable bit */
	MCHBAR32(GFXVTBAR + 4) = GFXVT_BASE_ADDRESS >> 32;
	MCHBAR32(GFXVTBAR) = GFXVT_BASE_ADDRESS | 1;
	MCHBAR32(VTVC0BAR + 4) = VTVC0_BASE_ADDRESS >> 32;
	MCHBAR32(VTVC0BAR) = VTVC0_BASE_ADDRESS | 1;

	/* set L3HIT2PEND_DIS, lock GFXVTBAR policy cfg registers */
	u32 reg32;
	reg32 = read32((void *)(GFXVT_BASE_ADDRESS + ARCHDIS));
	write32((void *)(GFXVT_BASE_ADDRESS + ARCHDIS),
			reg32 | DMAR_LCKDN | L3HIT2PEND_DIS);
	/* clear SPCAPCTRL */
	reg32 = read32((void *)(VTVC0_BASE_ADDRESS + ARCHDIS)) & ~SPCAPCTRL;
	/* set GLBIOTLBINV, GLBCTXTINV; lock VTVC0BAR policy cfg registers */
	write32((void *)(VTVC0_BASE_ADDRESS + ARCHDIS),
			reg32 | DMAR_LCKDN | GLBIOTLBINV | GLBCTXTINV);
}

static void start_peg_link_training(void)
{
	u32 tmp;
	u32 deven;

	deven = pci_read_config32(PCI_DEV(0, 0, 0), DEVEN);

	if (deven & DEVEN_D1F0EN) {
		tmp = pci_read_config32(PCI_DEV(0, 1, 0), 0xC24) & ~(1 << 16);
		pci_write_config32(PCI_DEV(0, 1, 0), 0xC24, tmp | (1 << 5));
	}

	if (deven & DEVEN_D1F1EN) {
		tmp = pci_read_config32(PCI_DEV(0, 1, 1), 0xC24) & ~(1 << 16);
		pci_write_config32(PCI_DEV(0, 1, 1), 0xC24, tmp | (1 << 5));
	}

	if (deven & DEVEN_D1F2EN) {
		tmp = pci_read_config32(PCI_DEV(0, 1, 2), 0xC24) & ~(1 << 16);
		pci_write_config32(PCI_DEV(0, 1, 2), 0xC24, tmp | (1 << 5));
	}
}

void haswell_early_initialization(int chipset_type)
{
	/* Setup all BARs required for early PCIe and raminit */
	haswell_setup_bars();

	/* Setup IOMMU BARs */
	haswell_setup_iommu();

	haswell_setup_graphics();

	haswell_setup_misc();

	start_peg_link_training();
}
