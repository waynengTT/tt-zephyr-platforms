/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PCIE_LTSSM_LOGGER_H
#define PCIE_LTSSM_LOGGER_H

#include <stdint.h>
#include <stdbool.h>

/* LTSSM State Definitions (from PCIe spec) */
typedef enum {
	LTSSM_DETECT_QUIET = 0x00,
	LTSSM_DETECT_ACT = 0x01,
	LTSSM_POLL_ACTIVE = 0x02,
	LTSSM_POLL_COMPLIANCE = 0x03,
	LTSSM_POLL_CONFIG = 0x04,
	LTSSM_PRE_DETECT_QUIET = 0x05,
	LTSSM_DETECT_WAIT = 0x06,
	LTSSM_CFG_LINKWD_START = 0x07,
	LTSSM_CFG_LINKWD_ACEPT = 0x08,
	LTSSM_CFG_LANENUM_WAIT = 0x09,
	LTSSM_CFG_LANENUM_ACEPT = 0x0A,
	LTSSM_CFG_COMPLETE = 0x0B,
	LTSSM_CFG_IDLE = 0x0C,
	LTSSM_RCVRY_LOCK = 0x0D,
	LTSSM_RCVRY_SPEED = 0x0E,
	LTSSM_RCVRY_RCVRCFG = 0x0F,
	LTSSM_RCVRY_IDLE = 0x10,
	LTSSM_RCVRY_EQ0 = 0x11,
	LTSSM_RCVRY_EQ1 = 0x12,
	LTSSM_RCVRY_EQ2 = 0x13,
	LTSSM_RCVRY_EQ3 = 0x14,
	LTSSM_L0 = 0x15,
	LTSSM_L0S = 0x16,
	LTSSM_L123_SEND_EIDLE = 0x17,
	LTSSM_L1_IDLE = 0x18,
	LTSSM_L2_IDLE = 0x19,
	LTSSM_L2_WAKE = 0x1A,
	LTSSM_DISABLED_ENTRY = 0x1B,
	LTSSM_DISABLED_IDLE = 0x1C,
	LTSSM_DISABLED = 0x1D,
	LTSSM_LPBK_ENTRY = 0x1E,
	LTSSM_LPBK_ACTIVE = 0x1F,
	LTSSM_LPBK_EXIT = 0x20,
	LTSSM_LPBK_EXIT_TIMEOUT = 0x21,
	LTSSM_HOT_RESET_ENTRY = 0x22,
	LTSSM_HOT_RESET = 0x23,
	LTSSM_UNKNOWN = 0x3F,
} LtssmState;

/* Rolling buffer configuration */
#define LTSSM_LOG_BUFFER_SIZE 32 /* Number of entries to keep (32 per instance) */
#define LTSSM_LOG_INSTANCES   2  /* PCIe0 and PCIe1 */

/* PCIe Link Speed encoding */
typedef enum {
	PCIE_SPEED_GEN1 = 1,  /* 2.5 GT/s */
	PCIE_SPEED_GEN2 = 2,  /* 5.0 GT/s */
	PCIE_SPEED_GEN3 = 3,  /* 8.0 GT/s */
	PCIE_SPEED_GEN4 = 4,  /* 16.0 GT/s */
	PCIE_SPEED_GEN5 = 5,  /* 32.0 GT/s */
	PCIE_SPEED_UNKNOWN = 0,
} PcieLinkSpeed;

/* LTSSM log entry structure (fits in 32 bits) */
typedef struct {
	uint32_t state : 6;       /* LTSSM state (0-63) */
	uint32_t link_up : 1;     /* Link up status */
	uint32_t rdlh_link_up : 1; /* RDLH link up */
	uint32_t link_speed : 3;  /* Current negotiated link speed (0-7) */
	uint32_t reserved : 1;    /* Reserved for future use */
	uint32_t timestamp : 20;  /* Timestamp in microseconds (wraps every ~1 second) */
} __attribute__((packed)) LtssmLogEntry;

typedef union {
	uint32_t val;
	LtssmLogEntry f;
} LtssmLogEntry_u;

/* LTSSM logger control structure (fits in 2x 32-bit registers) */
typedef struct {
	uint32_t write_index : 8;   /* Current write position (0-31) */
	uint32_t overflow_count : 8; /* Number of times buffer wrapped */
	uint32_t enabled : 1;       /* Logger enabled flag */
	uint32_t pcie_inst : 1;     /* PCIe instance (0 or 1) */
	uint32_t reserved : 14;     /* Reserved for future use */
	uint32_t last_timestamp;    /* Last logged timestamp (full 32-bit) */
} __attribute__((packed)) LtssmLoggerControl;

typedef union {
	uint64_t val;
	LtssmLoggerControl f;
	uint32_t words[2];
} LtssmLoggerControl_u;

/* CSM (Common Shared Memory) allocation for LTSSM Logger:
 *
 * CSM is 512KB at 0x10000000-0x1007FFFF
 * Mostly unused except for Virtual UART access
 * Perfect for debug buffers that need large capacity
 *
 * MEMORY LAYOUT using CSM:
 * 0x10000000-0x10000003: PCIe0 Logger Control (1 register)
 * 0x10000004-0x10000007: PCIe1 Logger Control (1 register)
 * 0x10000008-0x10000107: PCIe0 Log Buffer (64 entries x 4 bytes = 256 bytes)
 * 0x10000108-0x10000207: PCIe1 Log Buffer (64 entries x 4 bytes = 256 bytes)
 * 0x10000208-0x1007FFFF: Available CSM space (~511KB remaining)
 *
 * Total used: ~520 bytes out of 512KB CSM
 * This allows 64-entry buffers for BOTH PCIe instances!
 */

/* Buffer size definitions - now we can support 64 entries each! */
#define LTSSM_LOG_BUFFER_SIZE_PCIE0 64  /* Full 64-entry buffer in CSM */
#define LTSSM_LOG_BUFFER_SIZE_PCIE1 64  /* Full 64-entry buffer in CSM */

/* For code compatibility, use max of the two */
#define LTSSM_LOG_BUFFER_SIZE_MAX 64

/* Legacy compact size for reference */
#define LTSSM_LOG_BUFFER_SIZE_COMPACT 16

#define PCIE0_LTSSM_LOG_CTRL_REG_ADDR  0x10000000 /* CSM base */
#define PCIE1_LTSSM_LOG_CTRL_REG_ADDR  0x10000004 /* CSM base + 4 */
#define PCIE0_LTSSM_LOG_BASE_ADDR      0x10000008 /* CSM base + 8, 64 entries */
#define PCIE1_LTSSM_LOG_BASE_ADDR      0x10000108 /* CSM base + 264, 64 entries */

#define LTSSM_LOG_CTRL_REG_ADDR(inst) \
	((inst) == 0 ? PCIE0_LTSSM_LOG_CTRL_REG_ADDR : PCIE1_LTSSM_LOG_CTRL_REG_ADDR)
#define LTSSM_LOG_BASE_ADDR(inst) \
	((inst) == 0 ? PCIE0_LTSSM_LOG_BASE_ADDR : PCIE1_LTSSM_LOG_BASE_ADDR)
#define LTSSM_LOG_BUFFER_SIZE(inst) \
	((inst) == 0 ? LTSSM_LOG_BUFFER_SIZE_PCIE0 : LTSSM_LOG_BUFFER_SIZE_PCIE1)

/* Compact control structure (fits in 32 bits) */
typedef struct {
	uint32_t write_index : 6;    /* Current write position (0-63, supports up to 64 entries) */
	uint32_t overflow_count : 8; /* Number of times buffer wrapped (0-255) */
	uint32_t enabled : 1;        /* Logger enabled flag */
	uint32_t pcie_inst : 1;      /* PCIe instance (0 or 1) */
	uint32_t entry_count : 6;    /* Number of valid entries (0-63) */
	uint32_t last_state : 6;     /* Last logged state for deduplication */
	uint32_t reserved : 4;       /* Reserved for future use */
} __attribute__((packed)) LtssmLoggerControlCompact;

typedef union {
	uint32_t val;
	LtssmLoggerControlCompact f;
} LtssmLoggerControlCompact_u;

/**
 * Initialize LTSSM logger for a PCIe instance
 * @param pcie_inst PCIe instance (0 or 1)
 */
void ltssm_logger_init(uint8_t pcie_inst);

/**
 * Log an LTSSM state transition (non-blocking, safe from ISR)
 * @param pcie_inst PCIe instance (0 or 1)
 * @param state Current LTSSM state
 * @param link_up Link up status
 * @param rdlh_link_up RDLH link up status
 * @param link_speed Current negotiated link speed (0-7, 0=unknown, 1=Gen1, 2=Gen2, etc)
 */
void ltssm_logger_log(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint8_t link_speed);

/**
 * Read the logger control structure
 * @param pcie_inst PCIe instance (0 or 1)
 * @return Control structure value
 */
uint32_t ltssm_logger_get_control(uint8_t pcie_inst);

/**
 * Read a log entry from the buffer
 * @param pcie_inst PCIe instance (0 or 1)
 * @param index Buffer index (0 to LTSSM_LOG_BUFFER_SIZE_COMPACT-1)
 * @return Log entry value
 */
uint32_t ltssm_logger_get_entry(uint8_t pcie_inst, uint8_t index);

/**
 * Enable or disable logging
 * @param pcie_inst PCIe instance (0 or 1)
 * @param enable true to enable, false to disable
 */
void ltssm_logger_enable(uint8_t pcie_inst, bool enable);

/**
 * Clear the log buffer
 * @param pcie_inst PCIe instance (0 or 1)
 */
void ltssm_logger_clear(uint8_t pcie_inst);

/**
 * Get human-readable state name
 * @param state LTSSM state value
 * @return String representation of state
 */
const char *ltssm_state_to_string(uint8_t state);

/**
 * Get human-readable speed name
 * @param speed PCIe link speed value (0-7)
 * @return String representation of speed (e.g., "Gen1 (2.5GT/s)")
 */
const char *pcie_speed_to_string(uint8_t speed);

#endif /* PCIE_LTSSM_LOGGER_H */
