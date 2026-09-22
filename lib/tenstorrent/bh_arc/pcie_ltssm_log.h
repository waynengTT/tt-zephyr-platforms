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
 * Records LTSSM state transitions into a BSS buffer in CSM. The buffer address
 * and size are published to LTSSM_LOG_ADDR_REG_ADDR / LTSSM_LOG_SIZE_REG_ADDR
 * so the host can find it without hardcoding an address; see
 * scripts/pcie/ltssm_dump/ in syseng.
 *
 * Blackhole is a PCIe endpoint: on switch-attached boards the link partner is
 * a downstream port trained by host BIOS, which happens long after PCIe init
 * runs. The recorder therefore samples indefinitely rather than closing a
 * one-shot window, and the buffer is a wrapping ring that retains the MOST
 * RECENT transitions.
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
#define LTSSM_INFO_REPEAT_SHIFT 9
#define LTSSM_INFO_REPEAT_MAX   0x007FFFFFU /* 23 bits, saturating */
/*
 * The part of info that identifies what was observed, as opposed to how many
 * times. Two observations are folded into one entry only when all of these
 * match, so instance 0 and instance 1 sitting in the same state stay separate.
 */
#define LTSSM_INFO_ID_MASK                                                                         \
	(LTSSM_INFO_STATE_MASK | LTSSM_INFO_LINK_UP | LTSSM_INFO_RDLH_LINK_UP | LTSSM_INFO_INST)

/*
 * Bit layout of struct ltssm_log_entry::cdr.
 *
 * LOCK holds one bit per lane, bit N for lane N, taken from the SerDes
 * per-lane status register's rx-data-valid field - the PHY's "RX CDR is
 * locked onto incoming data" indication.
 *
 * LANES is how many lanes were actually read, which makes an entry
 * self-describing: zero means the PHY was not sampled for this entry, so a
 * clear LOCK bit reads as "unknown" rather than "not locked". The recorder
 * leaves it zero throughout Detect, where there is no partner signal to lock
 * to and sampling would only cost NOC reads.
 */
#define LTSSM_CDR_LOCK_MASK   0x0000FFFFU
#define LTSSM_CDR_LANES_MASK  0x001F0000U
#define LTSSM_CDR_LANES_SHIFT 16
#define LTSSM_CDR_MAX_LANES   16

/*
 * One run of consecutive observations of the same state.
 *
 * A run is a single state-and-CDR observation that was seen @a repeat + 1
 * times without a different one being recorded in between, or - so that
 * Detect churn cannot evict real training data - one half of a two-state
 * alternation that the recorder collapsed in place. See ltssm_log_record().
 */
struct ltssm_log_entry {
	/*
	 * Free-running refclk timestamp of the FIRST observation in this run,
	 * in ticks of LTSSM_LOG_TICK_HZ. The hardware counter is 64 bits wide
	 * and never wraps in practice, so readers need no unwrapping.
	 */
	uint64_t timestamp;
	/* State, link-up bits, instance and repeat count; see LTSSM_INFO_*. */
	uint32_t info;
	/*
	 * Ticks from @a timestamp to the LAST observation in this run, so the
	 * time a collapsed run spans is recoverable. Zero when the repeat
	 * count is zero. Saturates at UINT32_MAX (about 85.9 s); past that,
	 * a run that is not the newest one is still bounded by the timestamp
	 * of the entry after it.
	 */
	uint32_t last_delta;
	/*
	 * Per-lane CDR lock and the lane count it covers; see LTSSM_CDR_*.
	 * Part of a run's identity, not a payload: two observations fold into
	 * one entry only when this matches too, so a lane gaining or losing
	 * lock opens a new entry even though the LTSSM state did not change.
	 * That is what puts a timestamped row at the moment lock asserts.
	 */
	uint32_t cdr;
	/* Pads the entry to 24 bytes explicitly rather than by chance. */
	uint32_t rsvd;
};

#define LTSSM_LOG_MAGIC 0x4D53544CU /* "LTSM" */
/*
 * Version 2 changed the buffer from a linear array of 8-byte entries into a
 * wrapping ring of 16-byte entries carrying repeat counts. Version 3 widened
 * the entry again to carry the PHY's per-lane CDR lock bitmap. Both host
 * readers check the version and fail cleanly on a mismatch.
 */
#define LTSSM_LOG_VERSION 3U

/* Refclk is 50 MHz (20 ns period); see WAIT_1MS in timer.h. */
#define LTSSM_LOG_TICK_HZ 50000000U

/*
 * Header at the start of the published buffer, followed by @a capacity
 * entries. Everything the host needs to decode the log lives here, so the
 * firmware and the host script cannot disagree about geometry or time units.
 *
 * The first three words keep their version 1 position and meaning so an old
 * reader rejects the buffer on @a version rather than misparsing it.
 *
 * Entries form a ring. To read them oldest-first:
 *
 *     oldest = (head - count + capacity) % capacity
 *     for i in 0 .. count-1:
 *         entry[(oldest + i) % capacity]
 *
 * which also covers the not-yet-wrapped case, where head == count and so
 * oldest == 0.
 */
struct ltssm_log_header {
	uint32_t magic;
	uint32_t version;
	uint32_t entry_size;
	uint32_t capacity;
	/* Entries currently held in the ring, at most @a capacity. */
	uint32_t count;
	/* Entries evicted by the ring wrapping, so far. */
	uint32_t dropped;
	uint32_t tick_hz;
	/*
	 * Ring index of the slot the next entry will be written to. Was
	 * "reserved" in version 1, where it always read 0.
	 */
	uint32_t head;
};

/**
 * @brief Discard any recorded transitions and publish the buffer location.
 *
 * Writes the buffer address and size to LTSSM_LOG_ADDR_REG_ADDR and
 * LTSSM_LOG_SIZE_REG_ADDR. Call once before recording.
 */
void ltssm_log_reset(void);

/**
 * @brief Append one LTSSM state observation, collapsing repeats.
 *
 * The ring never fills: once @a capacity entries are held, appending evicts
 * the oldest entry and increments the dropped counter, so the buffer always
 * retains the most recent transitions.
 *
 * Two kinds of repetition are collapsed in place instead of appending, so a
 * link that churns for minutes cannot evict real training data:
 *
 *  - the same state and CDR observed again, which bumps the newest entry's
 *    repeat count, and
 *  - an observation matching the entry before the newest one, which bumps
 *    that entry's repeat count instead. This is what folds an endless
 *    DETECT_QUIET <-> DETECT_ACT alternation into a fixed two entries, and
 *    equally bounds a lane whose CDR lock flaps in place at two entries.
 *
 * A collapsed entry keeps the timestamp of its first observation and tracks
 * the last one in @a last_delta. Callers may suppress unchanged states
 * themselves to keep the sampling loop tight; doing so only skips the first
 * case above.
 *
 * @param pcie_inst PCIe instance the observation came from (0 or 1)
 * @param state Raw smlh_ltssm_state_sync value
 * @param link_up Value of smlh_link_up_sync
 * @param rdlh_link_up Value of rdlh_link_up_sync
 * @param cdr Per-lane CDR lock and lane count; see LTSSM_CDR_*. Zero when
 *            the PHY was not sampled for this observation.
 * @param timestamp Refclk timestamp of the observation
 */
void ltssm_log_record(uint8_t pcie_inst, uint8_t state, bool link_up, bool rdlh_link_up,
		      uint32_t cdr, uint64_t timestamp);

#endif /* PCIE_LTSSM_LOG_H */
