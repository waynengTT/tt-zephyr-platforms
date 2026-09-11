/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>
#include "noc2axi.h"

/* Default PCIe BAR sizes, in MiB. Shared between pcie.c and the recovery
 * chip-info backend (chip_info_static.c) so the synthesized recovery defaults
 * stay in sync with the values pcie.c programs.
 */
#define PCIE_BAR0_SIZE_DEFAULT_MB 512
#define PCIE_BAR2_SIZE_DEFAULT_MB 1
#define PCIE_BAR4_SIZE_DEFAULT_MB 32768

/* Gen3 EQ tuning defaults. These are the values libpciesd hardcoded into
 * GEN3_EQ_CONTROL_OFF before SYS-2216 moved them into the fw table, and are
 * what CntlInitV2() still substitutes, so they are also the right fallback
 * for the synthesized recovery table.
 */
#define PCIE_GEN3_EQ_PSET_REQ_VEC_DEFAULT 0x3E0 /* request presets 5-9 */
#define PCIE_GEN3_EQ_FB_MODE_DEFAULT      1     /* figure of merit, not direction */

typedef enum {
	EndPoint = 0,
	RootComplex = 1,
} PCIeDeviceType;

typedef enum {
	PCIeInitOk = 0,
	PCIeSerdesFWLoadTimeout = 1,
	PCIeLinkTrainTimeout = 2,
} PCIeInitStatus;

#define PCIE_INST0_LOGICAL_X 2
#define PCIE_INST1_LOGICAL_X 11
#define PCIE_LOGICAL_Y       0
#define PCIE_DBI_REG_TLB     14

static inline void WriteDbiReg(const uint32_t addr, const uint32_t data)
{
	const uint8_t noc_id = 0;

	NOC2AXIWrite32(noc_id, PCIE_DBI_REG_TLB, addr, data);
}

static inline uint32_t ReadDbiReg(const uint32_t addr)
{
	const uint8_t noc_id = 0;

	return NOC2AXIRead32(noc_id, PCIE_DBI_REG_TLB, addr);
}
#endif
