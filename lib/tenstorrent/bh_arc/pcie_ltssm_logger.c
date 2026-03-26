/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcie_ltssm_logger.h"
#include "reg.h"
#include "timer.h"

#include <string.h>
#include <zephyr/kernel.h>

/* State name lookup table */
static const char * const ltssm_state_names[] = {
	[LTSSM_DETECT_QUIET] = "DETECT.QUIET",
	[LTSSM_DETECT_ACT] = "DETECT.ACTIVE",
	[LTSSM_POLL_ACTIVE] = "POLL.ACTIVE",
	[LTSSM_POLL_COMPLIANCE] = "POLL.COMPLIANCE",
	[LTSSM_POLL_CONFIG] = "POLL.CONFIG",
	[LTSSM_PRE_DETECT_QUIET] = "PRE_DETECT.QUIET",
	[LTSSM_DETECT_WAIT] = "DETECT.WAIT",
	[LTSSM_CFG_LINKWD_START] = "CFG.LINKWIDTH.START",
	[LTSSM_CFG_LINKWD_ACEPT] = "CFG.LINKWIDTH.ACCEPT",
	[LTSSM_CFG_LANENUM_WAIT] = "CFG.LANENUM.WAIT",
	[LTSSM_CFG_LANENUM_ACEPT] = "CFG.LANENUM.ACCEPT",
	[LTSSM_CFG_COMPLETE] = "CFG.COMPLETE",
	[LTSSM_CFG_IDLE] = "CFG.IDLE",
	[LTSSM_RCVRY_LOCK] = "RECOVERY.LOCK",
	[LTSSM_RCVRY_SPEED] = "RECOVERY.SPEED",
	[LTSSM_RCVRY_RCVRCFG] = "RECOVERY.RCVRCFG",
	[LTSSM_RCVRY_IDLE] = "RECOVERY.IDLE",
	[LTSSM_RCVRY_EQ0] = "RECOVERY.EQ0",
	[LTSSM_RCVRY_EQ1] = "RECOVERY.EQ1",
	[LTSSM_RCVRY_EQ2] = "RECOVERY.EQ2",
	[LTSSM_RCVRY_EQ3] = "RECOVERY.EQ3",
	[LTSSM_L0] = "L0",
	[LTSSM_L0S] = "L0s",
	[LTSSM_L123_SEND_EIDLE] = "L123.SEND_EIDLE",
	[LTSSM_L1_IDLE] = "L1.IDLE",
	[LTSSM_L2_IDLE] = "L2.IDLE",
	[LTSSM_L2_WAKE] = "L2.WAKE",
	[LTSSM_DISABLED_ENTRY] = "DISABLED.ENTRY",
	[LTSSM_DISABLED_IDLE] = "DISABLED.IDLE",
	[LTSSM_DISABLED] = "DISABLED",
	[LTSSM_LPBK_ENTRY] = "LOOPBACK.ENTRY",
	[LTSSM_LPBK_ACTIVE] = "LOOPBACK.ACTIVE",
	[LTSSM_LPBK_EXIT] = "LOOPBACK.EXIT",
	[LTSSM_LPBK_EXIT_TIMEOUT] = "LOOPBACK.EXIT_TIMEOUT",
	[LTSSM_HOT_RESET_ENTRY] = "HOT_RESET.ENTRY",
	[LTSSM_HOT_RESET] = "HOT_RESET",
};

/* Speed name lookup table */
static const char * const pcie_speed_names[] = {
	[PCIE_SPEED_UNKNOWN] = "Unknown",
	[PCIE_SPEED_GEN1] = "Gen1 (2.5GT/s)",
	[PCIE_SPEED_GEN2] = "Gen2 (5.0GT/s)",
	[PCIE_SPEED_GEN3] = "Gen3 (8.0GT/s)",
	[PCIE_SPEED_GEN4] = "Gen4 (16.0GT/s)",
	[PCIE_SPEED_GEN5] = "Gen5 (32.0GT/s)",
};

void ltssm_logger_init(uint8_t pcie_inst)
{
	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return;
	}

	/* Clear control structure */
	LtssmLoggerControlCompact_u ctrl = {.val = 0};
	ctrl.f.pcie_inst = pcie_inst;
	ctrl.f.enabled = 1; /* Enable by default */
	ctrl.f.last_state = LTSSM_UNKNOWN;

	WriteReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst), ctrl.val);

	/* Clear buffer */
	uint32_t base_addr = LTSSM_LOG_BASE_ADDR(pcie_inst);
	uint32_t buffer_size = LTSSM_LOG_BUFFER_SIZE(pcie_inst);
	
	for (uint32_t i = 0; i < buffer_size; i++) {
		WriteReg(base_addr + (i * sizeof(uint32_t)), 0);
	}
}

void ltssm_logger_log(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint8_t link_speed)
{
	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return;
	}

	/* Read current control structure atomically */
	LtssmLoggerControlCompact_u ctrl;
	ctrl.val = ReadReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst));

	/* Check if logging is enabled */
	if (!ctrl.f.enabled) {
		return;
	}

	/* Skip duplicate consecutive states to save buffer space */
	if (state == ctrl.f.last_state && ctrl.f.entry_count > 0) {
		return;
	}

	/* Get timestamp (microseconds, wrap at 20 bits = ~1 second) */
	uint64_t timestamp_us = TimerTimestamp();
	uint32_t timestamp_20bit = (uint32_t)(timestamp_us & 0xFFFFF);

	/* Create log entry */
	LtssmLogEntry_u entry = {.val = 0};
	entry.f.state = state & 0x3F;
	entry.f.link_up = link_up ? 1 : 0;
	entry.f.rdlh_link_up = rdlh_link_up ? 1 : 0;
	entry.f.link_speed = link_speed & 0x7;
	entry.f.timestamp = timestamp_20bit;

	/* Write entry to buffer */
	uint32_t base_addr = LTSSM_LOG_BASE_ADDR(pcie_inst);
	uint32_t entry_addr = base_addr + (ctrl.f.write_index * sizeof(uint32_t));
	WriteReg(entry_addr, entry.val);

	/* Update control structure */
	ctrl.f.last_state = state & 0x3F;
	uint32_t buffer_size = LTSSM_LOG_BUFFER_SIZE(pcie_inst);
	ctrl.f.write_index = (ctrl.f.write_index + 1) % buffer_size;

	if (ctrl.f.entry_count < buffer_size) {
		ctrl.f.entry_count++;
	} else {
		/* Buffer wrapped */
		ctrl.f.overflow_count++;
	}

	/* Write control structure atomically */
	WriteReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst), ctrl.val);
}

uint32_t ltssm_logger_get_control(uint8_t pcie_inst)
{
	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return 0;
	}

	return ReadReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst));
}

uint32_t ltssm_logger_get_entry(uint8_t pcie_inst, uint8_t index)
{
	uint32_t buffer_size = LTSSM_LOG_BUFFER_SIZE(pcie_inst);
	
	if (pcie_inst >= LTSSM_LOG_INSTANCES || index >= buffer_size) {
		return 0;
	}

	uint32_t base_addr = LTSSM_LOG_BASE_ADDR(pcie_inst);
	uint32_t entry_addr = base_addr + (index * sizeof(uint32_t));

	return ReadReg(entry_addr);
}

void ltssm_logger_enable(uint8_t pcie_inst, bool enable)
{
	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return;
	}

	LtssmLoggerControlCompact_u ctrl;
	ctrl.val = ReadReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst));
	ctrl.f.enabled = enable ? 1 : 0;
	WriteReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst), ctrl.val);
}

void ltssm_logger_clear(uint8_t pcie_inst)
{
	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return;
	}

	/* Reset control structure but keep enabled state */
	LtssmLoggerControlCompact_u ctrl;
	ctrl.val = ReadReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst));

	bool was_enabled = ctrl.f.enabled;
	ctrl.val = 0;
	ctrl.f.pcie_inst = pcie_inst;
	ctrl.f.enabled = was_enabled ? 1 : 0;
	ctrl.f.last_state = LTSSM_UNKNOWN;

	WriteReg(LTSSM_LOG_CTRL_REG_ADDR(pcie_inst), ctrl.val);

	/* Clear buffer */
	uint32_t base_addr = LTSSM_LOG_BASE_ADDR(pcie_inst);
	uint32_t buffer_size = LTSSM_LOG_BUFFER_SIZE(pcie_inst);
	
	for (uint32_t i = 0; i < buffer_size; i++) {
		WriteReg(base_addr + (i * sizeof(uint32_t)), 0);
	}
}

const char *ltssm_state_to_string(uint8_t state)
{
	if (state < ARRAY_SIZE(ltssm_state_names) && ltssm_state_names[state] != NULL) {
		return ltssm_state_names[state];
	}
	return "UNKNOWN";
}

const char *pcie_speed_to_string(uint8_t speed)
{
	if (speed < ARRAY_SIZE(pcie_speed_names) && pcie_speed_names[speed] != NULL) {
		return pcie_speed_names[speed];
	}
	return "Unknown";
}
