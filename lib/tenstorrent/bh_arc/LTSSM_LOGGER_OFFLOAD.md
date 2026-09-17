# Offloading the PCIe LTSSM logger off the SMC boot core

Feasibility study for running the LTSSM recorder somewhere other than the ARC
core that runs the SMC firmware, so that enabling the logger no longer costs a
bootable chip.

Companions: `pcie_ltssm_log.{c,h}`, `Kconfig.pcie_ltssm_log`,
`pcie.c:488` (`CaptureLtssmTraining()`), and the host-side readers in
`syseng:src/t6ifc/t6py/packages/tenstorrent/scripts/pcie/ltssm_dump/`.

---

## 1. The problem being solved

`CaptureLtssmTraining()` never returns. It is called at the very end of
`pcie_init()` (`pcie.c:591`, `SYS_INIT` priority 103) and busy-polls the PCIe
SII `LTSSM_STATE` register forever. Everything registered after it therefore
never runs:

| SYS_INIT priority | Step | Status with the logger on |
|---|---|---|
| 103 | `pcie_init` | runs, never returns |
| 104 | `tensix_init` | never runs |
| 105 | `InitMrisc` | never runs |
| 106 | `eth_init` | never runs |
| 107 | `InitSmbusTarget` | never runs |
| 111 | `gddr_training` | never runs |
| 115–119 | msgqueue, telemetry, **fan control**, DVFS | never run |

The chip parks at post code 8. This is why the feature is documented as a debug
firmware rather than a bootable one.

The busy-poll is not gratuitous. Transitions inside the CFG and RCVRY
substates are as little as ~600 ns apart, and Blackhole is an endpoint whose
link partner is trained by host BIOS seconds to minutes after `pcie_init()`
runs, so there is no bounded window to sample. A Zephyr thread that yields at
any useful rate drops transitions; a thread that does not yield starves the
system exactly as today. **The disruption is inherent to one core spending
100% of its time on a sub-µs poll loop**, so the only real fixes are to move
that loop onto a core nobody needs, or to stop polling at all.

---

## 2. Check this first — it may make the whole question moot

Before building either option, confirm whether the DesignWare controller
already keeps an LTSSM state history in hardware. Many DWC PCIe cores expose
a state-change FIFO or `PL_DEBUG`-style capture registers; Blackhole's SII
register map is a Tenstorrent wrapper around one, and only offsets `0x128`
(`LTSSM_STATE`), `0x5C` (`APP_PCIE_CTL`) and the NOC TLB data registers are
used in `pcie.c` today.

If such a FIFO exists, the recorder becomes a slow drain loop (milliseconds
per pass) that can live in an ordinary Zephyr thread on the boot core, with no
second core, no new toolchain, and no lost transitions. This is a half-day of
databook reading and one JTAG read sweep, and it dominates every option below
in cost and quality. **Do this before committing to A or B.**

---

## 3. What the hardware actually offers

Established from the tree, not assumed:

* **Four ARC HS4xD cores** in one cluster — `cpu@0` … `cpu@3` in
  `dts/vendor/tenstorrent/tt_blackhole_smc.dtsi:20`. Zephyr is configured for
  one (`CONFIG_MP_MAX_NUM_CPUS=1`,
  `soc/tenstorrent/tt_blackhole/Kconfig.defconfig.tt_blackhole_smc:45`;
  `CONFIG_SMP` unset, `CONFIG_ARC_CONNECT` commented out at line 39). Cores 1–3
  are idle silicon today.
* **Run/halt control** — `RESET_UNIT_ARC_MISC_CNTL` at `0x80030100`, fields
  `run[3:0]`, `halt[7:4]`, `soft_reset[12]`, `self_reset[31]`
  (`lib/tenstorrent/fw_common/msgqueue.c:390`). One bit per core, so releasing
  a spare core is a single register write.
* **One shared reset vector** — all four cores fetch the vector word at
  `0x80000000` (`ROM_MEMORY_MEM_BASE_ADDR`). The DMC writes it over JTAG
  (`jtag_bootrom.c:319`) and the SMC rewrites it at runtime to install the
  panic handler (`soc/tenstorrent/tt_blackhole/soc.c:73`). There is no per-core
  vector register.
* **Private ICCM, shared CSM** — 4 KB ICCM per core at address 0, CSM 512 KB at
  `0x10000000` shared by the cluster (`tt_blackhole_smc.dtsi:625,631`).
* **Shared NOC2AXI TLBs** — the ARC block's NIU register space at `0x80050000`
  holds 16 TLBs per ring (`noc2axi.c:61,71`). They belong to the *block*, not to
  a core, so all four cores share one set. Current ring-0 allocation:
  0–5 (`pcie.c:33`), 0 again (`eth.c:42`), 13 (`gddr.c:57`), 14 (`pcie.h:42`),
  plus dynamic use by DMA/MSI. Indices 6–12 are free.
* **A working "load firmware onto another RISC and run it" precedent** —
  `wipe_dest()` (`tensix_init.c:252`) pulls a TRISC binary out of SPI flash by
  `tt_boot_fs` tag `destwipe`, multicasts it into Tensix L1 at `0x6000`, sets
  `TRISC0_RESET_PC`/`TRISC_RESET_PC_OVERRIDE`, releases `SOFT_RESET_0`, syncs on
  an atomic counter in L1, then re-asserts reset. This is production code.
* **Sampling path** — `ReadSiiReg()` is a single load through a NOC2AXI TLB
  window aimed at the PCIe tile (logical `(2,0)` for instance 0, `(11,0)` for
  instance 1; `pcie.h:39`). One blocking NOC read per sample; that is the
  floor for any sampler on this chip.
* **Timebase** — the free-running 50 MHz refclk counter at `0x800300E0`, in the
  reset unit inside the ARC tile at NOC `(8,0)`.

---

## 4. Option A — run the logger on a spare ARC core

### 4.1 Verdict

**Feasible, and the recommended option.** Highest confidence of the two, lowest
effort, and no regression in sample rate. The one genuine unknown — bringing a
second core up against a shared reset vector — is a bounded experiment that can
be settled in a day with JTAG.

### 4.2 Why it fits well

* The recorder code moves **verbatim**. `pcie_ltssm_log.c` and the poll loop
  need no algorithmic change; only their environment changes.
* The sampling path is **identical** — the same single-load TLB window, so no
  sample-rate regression versus today. Nothing else can beat one blocking load.
* The log stays in **CSM**, so `ltssm_dump.py` (PCIe/J-Link) and
  `ltssm_dump_bmc.sh` (BMC JTAG `--axi-read`) keep working with zero changes.
  The BMC path is the one that reaches a chip whose link never trained, i.e.
  the case the whole feature exists for. Preserving it unchanged is worth a
  lot.
* Same repo, same toolchain, same build, same Kconfig. No cross-repo
  dependency.
* Core 0 boots to completion: telemetry, fan control, message queue and GDDR
  all come up. The chip is a normal chip that happens to also be logging.

### 4.3 Two sub-approaches, and why only one is worth doing

**A1 — Zephyr SMP.** Set `CONFIG_MP_MAX_NUM_CPUS=2`, `CONFIG_SMP=y`,
`CONFIG_ARC_CONNECT=y`, and pin a logger thread to CPU 1. Zephyr's ARC port
already supports this (`zephyr/arch/arc/core/smp.c`,
`zephyr/arch/arc/core/reset.S:164`). **Not recommended**: it turns every
spinlock in the firmware into a real one, needs ARConnect IDU/IPI brought up
and validated, adds per-CPU stacks and timers, and makes the debug image differ
from production in the kernel's concurrency model — precisely the kind of
divergence that makes a debug capture untrustworthy. Large blast radius for a
core that only needs to run one loop.

**A2 — bare-metal "parasite" core (recommended).** Zephyr stays at
`MP_MAX_NUM_CPUS=1` on core 0 and never learns core 1 exists. Core 1 runs a
freestanding loop with its own stack, interrupts disabled, touching nothing but
its reserved TLB, the refclk register and its ring buffer.

### 4.4 Design sketch for A2

```
core 0 (Zephyr, unchanged)                 core 1 (freestanding)
──────────────────────────                 ─────────────────────
pcie_init() … PCIe up
  ltssm_log_reset()                        (in reset)
  reserve TLB 6 -> PCIe inst0
  reserve TLB 7 -> PCIe inst1
  vector[0x80000000] = ltssm_core_entry
  ARC_MISC_CNTL.run |= BIT(1)  ───────────► entry: check IDENTITY
  restore vector                              != 0 -> set SP, disable IRQs
  return 0  (init continues!)                 loop { read TLB6/TLB7,
tensix_init … telemetry … fan ctrl                 ltssm_log_record() }
```

Entry is handled by **repointing the shared reset vector word** just before
releasing core 1, then restoring it. Core 0 is long past its own reset and
never re-reads the word, so the swap is invisible to it. This is not a novel
trick in this codebase — `soc.c:73` already rewrites that same word at runtime
for the panic handler. The stub still checks `_ARC_V2_IDENTITY` on entry and
falls through to the original vector if it finds itself on core 0, so a
watchdog or panic reset during the swap window cannot strand the boot core in
the logger.

### 4.5 Risks and unknowns, with mitigations

| # | Risk | Assessment | Mitigation |
|---|---|---|---|
| 1 | Core 1 may not start from the vector word the way core 0 does | **Main unknown.** Settle it first with a 20-instruction stub that writes a scratch register and spins | One JTAG session; if the vector path does not work, fall back to the `reset.S` core-ID branch under `#if CONFIG_MP_MAX_NUM_CPUS > 1` that Zephyr already provides |
| 2 | TLB collision — TLBs belong to the block, not the core | Real but trivially avoided | Statically reserve indices 6 and 7 (one per PCIe instance, so neither core ever re-points them). `BUILD_ASSERT` against `pcie.c`/`gddr.c`/`eth.c` constants |
| 3 | CSM write visibility from core 1 (`CONFIG_DCACHE=y`, per-core L1) | Low. Core 0 already publishes the ring to JTAG/NOC readers with only a `compiler_barrier()`, which means CSM is effectively uncached or write-through for the cluster | Verify by dumping the ring over JTAG while core 1 writes it; if it fails, flush explicitly or place the ring in an uncached alias |
| 4 | Core 1 corrupts Zephyr state | Low by construction | No Zephyr headers, no drivers, no interrupts, own stack in a reserved CSM block. Review is easy because the whole thing is ~100 lines |
| 5 | Continuous NOC read traffic on ring 0 | **No regression** — the identical traffic exists today | Optional throttle knob if it ever matters |
| 6 | No console or logging on core 1 | Accepted | Heartbeat counter in a scratch register so the host can tell a wedged logger from an idle link |
| 7 | MCUboot RAM-load overwriting the stub | None if the stub lives in the normal image `.text` and the ring in `.bss` | Core 1 is released after all image load and BSS clear, so both are stable by then |

### 4.6 Implementation steps

1. **Spike the bringup (do this before anything else).** Hand-write a stub that
   sets a stack, writes a magic value to `SCRATCH_RAM[26]`, increments a
   counter, and spins. Repoint the vector, set `ARC_MISC_CNTL.run` bit 1, and
   watch the scratch register over JTAG. Everything downstream depends on this
   working; budget a day and stop if it does not.
2. **Reserve the TLBs.** Add `LTSSM_LOG_INST0_TLB 6` / `LTSSM_LOG_INST1_TLB 7`
   to `pcie_ltssm_log.h` with `BUILD_ASSERT`s that they collide with no
   existing allocation. Point them in `pcie_init()` before releasing core 1.
3. **Carve the core-1 region.** A small `.ltssm_core` section in the SMC linker
   script for stub text plus ~256 B of stack, plus the existing
   `.bss.pcie_ltssm_log` ring. Keep the ring where it is so the host readers do
   not move.
4. **Write the freestanding loop.** Port the body of `CaptureLtssmTraining()`
   (`pcie.c:488`) into `pcie_ltssm_log_core.c`, dropping `ConfigurePCIeTlbs()`
   (no longer needed — one TLB per instance) and keeping the `last_state[]`
   early-out and `LtssmTimestamp()` carry-glitch fix verbatim. Compile with
   `-ffreestanding`, no Zephyr headers.
5. **Replace the call site.** `pcie_init()` calls `LtssmLogStartOnCore1()`
   instead of `CaptureLtssmTraining()` and then **returns 0**. Delete the
   "never returns, so this has to stay last" comment; that constraint is gone.
6. **Rewrite the Kconfig help.** The current text in
   `Kconfig.pcie_ltssm_log` enumerates everything that does not boot. With A2
   the chip boots normally and the warning becomes "consumes ARC core 1 and two
   NOC TLBs". Consider flipping the default and whether it can ship enabled.
7. **Add the heartbeat.** Core 1 bumps a scratch register every N passes; the
   host readers print it so a stalled logger is distinguishable from a quiet
   link.
8. **Validate.** Confirm (a) the chip reaches the message queue and fan control
   spins up, (b) a capture on a known-bad link matches what today's build
   records, (c) telemetry over the message queue and a JTAG dump of the ring
   both work *simultaneously*.
9. **Update `LTSSM_DUMP.md`** in syseng — most of its "this is not a bootable
   firmware" framing becomes obsolete.

**Effort: roughly 1–2 weeks for one engineer**, front-loaded on step 1.

---

## 5. Option B — run the logger on a Tensix tile

### 5.1 Verdict

**Feasible, and the mechanism is already proven in production code — but it is
the more expensive option and it is unlikely to sample any faster.** Worth
pursuing only if Option A's bringup spike fails, or if a spare ARC core turns
out to be needed for something else.

### 5.2 What is already solved

`wipe_dest()` (`tensix_init.c:252`) is a complete worked example of the hard
part: find a RISC binary in SPI flash by `tt_boot_fs` tag, push it into Tensix
L1 over the NOC, override the reset PC, release the RISC from soft reset,
synchronise with it through L1, and put it back in reset. The packaging path
(`tt_boot_fs`), the transfer path (`spi_transfer_by_parts` +
`NOC2AXIWrite32`), and the tile-selection helper (`GetEnabledTensix()`, which
already respects harvesting) all exist and are exercised on every boot.

Adapting it for the logger means unicast instead of multicast, and not
re-asserting reset at the end.

### 5.3 Where it gets harder

**Sample rate is the headline concern.** The ARC samples with one blocking load
through a TLB window. A Tensix RISC has no such window: it issues NOC
transactions through command buffers, which means writing several registers,
triggering, and polling for completion per read. The BRISC core clock is higher
than the ARC's, but both are dominated by NOC round-trip latency, and the
command-buffer setup is pure overhead the ARC does not pay. Expect **equal or
worse** sampling than today, against a 600 ns transition floor. This must be
measured on silicon before committing — it is the one number that decides
whether the option is worth anything.

**Timestamps need care.** There is no refclk in the tile. Reading
`0x800300E0` over the NOC from the ARC tile at `(8,0)` on every pass would
roughly halve the sample rate. The tile's own wall clock runs on aiclk, which
DVFS moves, so it is not a usable timebase on its own. The workable answer is
to **timestamp only when a transition is actually recorded** — transitions are
rare relative to the poll rate, so the hot loop stays at one NOC read and
timestamps cost nothing in steady state.

**Ordering is tighter than it looks.** Tensix is usable early — NOC init is
priority 99 and RISC resets are deasserted at 101, both before `pcie_init` at
103 — but `tensix_init` at 104 calls `wipe_l1()` and `wipe_dest()`, and
`wipe_dest()` step 7 re-asserts `ALL_RISC_SOFT_RESET` on every tile. A logger
launched at the end of `pcie_init` would have its code and its ring wiped
moments later. **The logger must start at priority ≥ 105**, after
`tensix_init` completes — a few milliseconds later than today. Acceptable
given that BIOS trains the link seconds later, but it is a real (small) loss of
early coverage, and it is easy to get wrong.

**The BMC/JTAG read path is the significant open question.** With the ring in
Tensix L1, `ltssm_dump.py` over PCIe is easy (host TLBs read L1 routinely), but
`ltssm_dump_bmc.sh` issues `bmc-jtag-access --axi-read` at ARC AXI addresses.
Reaching L1 that way means reading *through* an ARC NOC2AXI window
(`0xC0000000 + (tlb << 24)`), and whether the JTAG AXI master can drive that
window is unverified. **This matters more than it sounds**: the BMC path is the
only one that works on a chip whose link never trained, which is the primary
use case. Two mitigations, both workable:
  * reserve a static ARC TLB pointing at the tile's L1 and have the BMC script
    read through it (no firmware involvement at read time); or
  * have core 0 periodically DMA the ring from L1 into CSM using the existing
    NOC DMA (`dma1`). This puts core 0 back to work, but a periodic block copy
    is nothing like a busy poll and would not disturb the boot.

**Power and tile management.** The chosen tile must stay powered and ungated
for the life of the capture. `TensixInit()` re-gates tile clocks when the
Tensix power domain is down, and `is_cable_fault_mode()` gates them outright —
both would silently kill the logger. The tile is also unavailable to workloads
while logging. For a debug firmware this is acceptable, but it has to be
handled explicitly rather than discovered.

**Toolchain and repo surface.** A RISC-V firmware needs its own build, its own
`tt_boot_fs` packaging, and a place to live. `destwipe` proves the packaging
path but its build lives outside this repo, so this adds a cross-repo
dependency that Option A does not have.

### 5.4 Implementation steps

1. **Measure first.** Write a minimal BRISC loop that reads the PCIe SII
   `LTSSM_STATE` register over the NOC and counts iterations against the wall
   clock. If the achievable sample period is materially worse than the ARC's,
   stop — the rest of the work buys a worse capture.
2. **Settle the JTAG read path.** Try a `bmc-jtag-access --axi-read` through an
   ARC NOC2AXI window at `0xC0000000 + (tlb << 24)` with a TLB pointed at a
   Tensix L1 address. This decides whether the BMC reader survives as-is or
   needs the DMA-to-CSM fallback.
3. **Pick and pin a tile.** Reuse `GetEnabledTensix()` so harvesting is
   respected. Record the chosen `(x, y)` for the host readers and exclude the
   tile from power gating for the duration.
4. **Build the RISC firmware.** Port `ltssm_log_record()` — it is plain C with
   no ARC dependencies — plus a NOC-read loop and the deferred-timestamp scheme
   from §5.3. Package it under a new `tt_boot_fs` tag alongside `destwipe`.
5. **Add the launcher** at `SYS_INIT` priority 105+, modelled directly on
   `wipe_dest()` steps 1–5, minus the multicast and minus the final
   re-assert.
6. **Redefine the published location.** `LTSSM_LOG_ADDR_REG_ADDR` currently
   holds a flat CSM address. It must now carry the tile's NOC `(x, y)` plus an
   L1 offset. Bump `LTSSM_LOG_VERSION` to 3 so old readers reject the buffer
   instead of misparsing it — the header comment in `pcie_ltssm_log.h` already
   anticipates exactly this.
7. **Update both host readers** for the new addressing and whichever read path
   step 2 selected.
8. **Validate** the same three ways as Option A, plus a check that the tile
   stays ungated across a DVFS excursion.

**Effort: roughly 3–5 weeks**, plus ongoing cross-repo maintenance of the RISC
binary.

---

## 6. Side by side

| | A2 — spare ARC core | B — Tensix tile |
|---|---|---|
| Mechanism proven in-tree | No (but Zephyr's ARC SMP path exists as a fallback) | **Yes** — `wipe_dest()` |
| Recorder code reuse | **Verbatim** | Port to RISC-V, plain C so straightforward |
| Sample rate vs. today | **Identical** (same single-load TLB path) | Equal or worse; must be measured |
| Capture start point | **Unchanged** (end of `pcie_init`, prio 103) | Delayed to prio ≥ 105 |
| Log location | **CSM — both host readers unchanged** | Tensix L1 — both readers need work; BMC path unverified |
| Timebase | **Direct refclk read** | Deferred refclk read over NOC |
| New toolchain / repo | **None** | RISC-V build + `tt_boot_fs` packaging |
| Resources consumed | 1 ARC core, 2 NOC TLBs | 1 Tensix tile (powered, unavailable), 1 TLB, SPI flash slot |
| Interacts with power mgmt / harvesting | No | **Yes** — both |
| Main unknown | Second-core bringup (1 day to settle) | Sample rate **and** JTAG read path |
| Effort | **1–2 weeks** | 3–5 weeks |

---

## 7. Recommendation

1. **Check for a hardware LTSSM history FIFO in the DWC controller first**
   (§2). Half a day. If it exists, neither option is needed and the recorder
   becomes an ordinary low-rate thread on the boot core.
2. **Otherwise pursue Option A2.** Start with the bringup spike (§4.6 step 1) —
   it is the only part with real uncertainty, it is cheap, and it is a clean
   go/no-go. Everything after it is mechanical.
3. **Hold Option B as the fallback** if the spike fails. Its mechanism is the
   better-proven one; what makes it second choice is the surrounding cost —
   a second toolchain, a delayed capture start, an unverified BMC read path,
   and entanglement with power management and harvesting.

Both options leave the ring format, the collapse logic and the host-side decode
untouched. Whichever is chosen, the recorder itself does not change.

---

## 8. Alternatives not pursued

* **Ethernet RISCs** (14 tiles, firmware already loaded by `eth.c`) — same
  shape as Option B, same NOC command-buffer sampling cost, same packaging
  work, and eth tiles are needed for Galaxy fabric bringup. No advantage over
  B.
* **L2CPU clusters** (4 × RISC-V, `tile_enable.l2cpu_enabled`) — considerably
  more capable than needed, and bringing one up for a poll loop is more work
  than either option above.
* **Zephyr thread on core 0** — cannot sample fast enough if it yields, and
  starves the system if it does not. This is the status quo with extra steps.
