/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_
#define TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_

#include "arc_dma.h"
#include "pcie.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CntlInitV2Param {
	uint64_t board_id;
	uint32_t vendor_id;
	uint8_t pcie_inst;
	uint8_t serdes_inst;
	uint8_t max_pcie_speed;
	uint8_t device_type;
	uint64_t region0_mask;
	uint64_t region2_mask;
	uint64_t region4_mask;
};

/*
 * CntlInitV2Param plus the Gen3 EQ tuning that libpciesd used to hardcode; see
 * SYS-2216. CntlInitV2() remains available and substitutes the old constants,
 * so it is exactly equivalent to passing PCIE_GEN3_EQ_*_DEFAULT here.
 *
 * This must match struct CntlInitV3Param in the syseng pciesd.c that
 * tt_blackhole_libpciesd.a is built from. It is passed by pointer to prebuilt
 * object code, so a layout disagreement corrupts memory silently rather than
 * failing to link -- hence the assertions below.
 */
struct CntlInitV3Param {
	uint64_t board_id;
	uint32_t vendor_id;
	uint8_t pcie_inst;
	uint8_t serdes_inst;
	uint8_t max_pcie_speed;
	uint8_t device_type;
	uint64_t region0_mask;
	uint64_t region2_mask;
	uint64_t region4_mask;
	uint32_t gen3_eq_pset_req_vec;
	uint8_t gen3_eq_fb_mode;
	uint32_t gen4_eq_pset_req_vec; /* 0: use gen3_eq_pset_req_vec */
	uint32_t gen5_eq_pset_req_vec; /* 0: use gen3_eq_pset_req_vec */
};

/* V3 only appends, so every field it shares with V2 must sit at the same
 * offset. Checked rather than assumed, because both are ABI to the blob.
 */
BUILD_ASSERT(offsetof(struct CntlInitV3Param, board_id) ==
	     offsetof(struct CntlInitV2Param, board_id));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, vendor_id) ==
	     offsetof(struct CntlInitV2Param, vendor_id));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, pcie_inst) ==
	     offsetof(struct CntlInitV2Param, pcie_inst));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, serdes_inst) ==
	     offsetof(struct CntlInitV2Param, serdes_inst));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, max_pcie_speed) ==
	     offsetof(struct CntlInitV2Param, max_pcie_speed));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, device_type) ==
	     offsetof(struct CntlInitV2Param, device_type));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, region0_mask) ==
	     offsetof(struct CntlInitV2Param, region0_mask));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, region2_mask) ==
	     offsetof(struct CntlInitV2Param, region2_mask));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, region4_mask) ==
	     offsetof(struct CntlInitV2Param, region4_mask));
BUILD_ASSERT(offsetof(struct CntlInitV3Param, gen3_eq_pset_req_vec) ==
	     sizeof(struct CntlInitV2Param));

/* Verify prototype of ArcDmaTransfer, because it's used by libpciesd.a. */
static __unused bool (*verify_ArcDmaTransfer)(const void *, void *, uint32_t) = ArcDmaTransfer;

/* The functions below are implemented in tt_blackhole_libpciesd.a */
PCIeInitStatus SerdesInit(uint8_t pcie_inst, PCIeDeviceType device_type,
			  uint8_t num_serdes_instance);
void ExitLoopback(void);
void EnterLoopback(void);
void CntlInit(uint8_t pcie_inst, uint8_t num_serdes_instance, uint8_t max_pcie_speed,
	      uint64_t board_id, uint32_t vendor_id);

void CntlInitV2(const struct CntlInitV2Param *param);

void CntlInitV3(const struct CntlInitV3Param *param);

#ifdef __cplusplus
}
#endif

#endif /* TTZP_LIB_TENSTORRENT_BH_ARC_PCIESD_H_ */
