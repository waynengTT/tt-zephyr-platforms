/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcie_ltssm_logger.h"
#include "reg.h"

#include <string.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/smc_msg.h>

/**
 * SMC Message handler to read LTSSM logger control register
 * Request:
 *   arg0: PCIe instance (0 or 1)
 * Response:
 *   arg0: Control register value
 *   arg1: Success (0) or error code
 */
static uint8_t GetLtssmLogControl(const union request *req, struct response *rsp)
{
	uint8_t pcie_inst = req->arg0;

	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		rsp->arg1 = 1; /* Invalid instance */
		return 0;
	}

	rsp->arg0 = ltssm_logger_get_control(pcie_inst);
	rsp->arg1 = 0; /* Success */

	return 0;
}
REGISTER_MESSAGE(TT_SMC_MSG_GET_LTSSM_LOG_CONTROL, GetLtssmLogControl);

/**
 * SMC Message handler to read LTSSM log entries
 * Request:
 *   arg0: PCIe instance (0 or 1)
 *   arg1: Starting index
 *   arg2: Number of entries to read (max 8)
 * Response:
 *   arg0-arg7: Log entries (up to 8)
 *   return: Number of entries read
 */
static uint8_t GetLtssmLogEntries(const union request *req, struct response *rsp)
{
	uint8_t pcie_inst = req->arg0;
	uint8_t start_index = req->arg1;
	uint8_t count = req->arg2;

	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		return 0; /* Invalid instance */
	}

	if (count > 8) {
		count = 8; /* Limit to response size */
	}

	if (start_index >= LTSSM_LOG_BUFFER_SIZE_COMPACT) {
		return 0; /* Invalid start index */
	}

	/* Read entries */
	uint8_t entries_read = 0;
	uint32_t *rsp_args = (uint32_t *)rsp;

	for (uint8_t i = 0; i < count && (start_index + i) < LTSSM_LOG_BUFFER_SIZE_COMPACT; i++) {
		rsp_args[i] = ltssm_logger_get_entry(pcie_inst, start_index + i);
		entries_read++;
	}

	return entries_read;
}
REGISTER_MESSAGE(TT_SMC_MSG_GET_LTSSM_LOG_ENTRIES, GetLtssmLogEntries);

/**
 * SMC Message handler to control LTSSM logger
 * Request:
 *   arg0: PCIe instance (0 or 1)
 *   arg1: Command (0=disable, 1=enable, 2=clear)
 * Response:
 *   arg0: Success (0) or error code
 */
static uint8_t ControlLtssmLogger(const union request *req, struct response *rsp)
{
	uint8_t pcie_inst = req->arg0;
	uint8_t command = req->arg1;

	if (pcie_inst >= LTSSM_LOG_INSTANCES) {
		rsp->arg0 = 1; /* Invalid instance */
		return 0;
	}

	switch (command) {
	case 0: /* Disable */
		ltssm_logger_enable(pcie_inst, false);
		break;
	case 1: /* Enable */
		ltssm_logger_enable(pcie_inst, true);
		break;
	case 2: /* Clear */
		ltssm_logger_clear(pcie_inst);
		break;
	default:
		rsp->arg0 = 2; /* Invalid command */
		return 0;
	}

	rsp->arg0 = 0; /* Success */
	return 0;
}
REGISTER_MESSAGE(TT_SMC_MSG_CONTROL_LTSSM_LOGGER, ControlLtssmLogger);
