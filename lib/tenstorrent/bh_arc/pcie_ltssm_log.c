/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcie_ltssm_log.h"
#include "reg.h"
#include "status_reg.h"

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#define LTSSM_LOG_CAPACITY CONFIG_TT_BH_ARC_PCIE_LTSSM_LOG_ENTRIES

/*
 * The log lives in BSS so the linker places it in CSM-APP, which the host can
 * read over the NOC. Carving a fixed CSM address by hand would land in either
 * MCUBoot's region or the application's own data.
 */
static struct {
	struct ltssm_log_header header;
	struct ltssm_log_entry entries[LTSSM_LOG_CAPACITY];
} ltssm_log __aligned(32) __attribute__((section(".bss.pcie_ltssm_log")));

void ltssm_log_reset(void)
{
	ltssm_log.header = (struct ltssm_log_header){
		.magic = LTSSM_LOG_MAGIC,
		.version = LTSSM_LOG_VERSION,
		.entry_size = sizeof(struct ltssm_log_entry),
		.capacity = LTSSM_LOG_CAPACITY,
		.count = 0,
		.dropped = 0,
		.tick_hz = LTSSM_LOG_TICK_HZ,
	};

	WriteReg(LTSSM_LOG_ADDR_REG_ADDR, (uint32_t)&ltssm_log);
	WriteReg(LTSSM_LOG_SIZE_REG_ADDR, sizeof(ltssm_log));
}

void ltssm_log_record(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint32_t timestamp)
{
	if (ltssm_log.header.count >= LTSSM_LOG_CAPACITY) {
		ltssm_log.header.dropped++;
		return;
	}

	uint32_t info = (state << LTSSM_INFO_STATE_SHIFT) & LTSSM_INFO_STATE_MASK;

	if (link_up) {
		info |= LTSSM_INFO_LINK_UP;
	}
	if (rdlh_link_up) {
		info |= LTSSM_INFO_RDLH_LINK_UP;
	}
	if (pcie_inst != 0) {
		info |= LTSSM_INFO_INST;
	}

	ltssm_log.entries[ltssm_log.header.count] = (struct ltssm_log_entry){
		.timestamp = timestamp,
		.info = info,
	};

	/*
	 * Publish the entry before the count that makes it visible, so a host
	 * polling concurrently never reads a half-written record.
	 */
	compiler_barrier();
	ltssm_log.header.count++;
}

bool ltssm_log_full(void)
{
	return ltssm_log.header.count >= LTSSM_LOG_CAPACITY;
}
