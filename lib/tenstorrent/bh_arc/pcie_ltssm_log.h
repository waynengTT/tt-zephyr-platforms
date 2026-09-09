/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCIE_LTSSM_LOG_H
#define PCIE_LTSSM_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * PCIe LTSSM training log.
 *
 * Records LTSSM state transitions observed while the link trains, into a BSS
 * buffer in CSM. The buffer address and size are published to
 * LTSSM_LOG_ADDR_REG_ADDR / LTSSM_LOG_SIZE_REG_ADDR so the host can find it
 * without hardcoding an address; see scripts/pcie/ltssm_dump.py in syseng.
 *
 * State values are the raw smlh_ltssm_state_sync encoding from the DesignWare
 * controller. Only the states this module needs are named here; the host is
 * responsible for decoding the rest.
 */
enum ltssm_state {
	LTSSM_STATE_DETECT_QUIET = 0x00,
	LTSSM_STATE_L0 = 0x11,
	/* Not a hardware value: marks "no state recorded yet". */
	LTSSM_STATE_NONE = 0x3F,
};

/* Bit layout of struct ltssm_log_entry::info */
#define LTSSM_INFO_STATE_MASK   0x0000003FU
#define LTSSM_INFO_STATE_SHIFT  0
#define LTSSM_INFO_LINK_UP      BIT(6)
#define LTSSM_INFO_RDLH_LINK_UP BIT(7)
#define LTSSM_INFO_INST         BIT(8)

struct ltssm_log_entry {
	/* Free-running refclk timestamp, in ticks of LTSSM_LOG_TICK_HZ. */
	uint32_t timestamp;
	uint32_t info;
};

#define LTSSM_LOG_MAGIC   0x4D53544CU /* "LTSM" */
#define LTSSM_LOG_VERSION 1U

/* Refclk is 50 MHz (20 ns period); see WAIT_1MS in timer.h. */
#define LTSSM_LOG_TICK_HZ 50000000U

/*
 * Header at the start of the published buffer, followed by @a capacity
 * entries. Everything the host needs to decode the log lives here, so the
 * firmware and the host script cannot disagree about geometry or time units.
 */
struct ltssm_log_header {
	uint32_t magic;
	uint32_t version;
	uint32_t entry_size;
	uint32_t capacity;
	uint32_t count;
	uint32_t dropped;
	uint32_t tick_hz;
	uint32_t reserved;
};

/**
 * @brief Discard any recorded transitions and publish the buffer location.
 *
 * Writes the buffer address and size to LTSSM_LOG_ADDR_REG_ADDR and
 * LTSSM_LOG_SIZE_REG_ADDR. Call once before recording.
 */
void ltssm_log_reset(void);

/**
 * @brief Append one LTSSM state observation.
 *
 * Entries are appended until the buffer is full, after which further calls
 * only increment the dropped counter. Callers are expected to suppress
 * unchanged states themselves.
 *
 * @param pcie_inst PCIe instance the observation came from (0 or 1)
 * @param state Raw smlh_ltssm_state_sync value
 * @param link_up Value of smlh_link_up_sync
 * @param rdlh_link_up Value of rdlh_link_up_sync
 * @param timestamp Refclk timestamp of the observation
 */
void ltssm_log_record(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint32_t timestamp);

/**
 * @brief Whether the buffer has no room left for further entries.
 */
bool ltssm_log_full(void);

#endif /* PCIE_LTSSM_LOG_H */
