# Audit: platform constants, SMP, interrupts, timers, reset paths (2026-10-23)

Read-only review of the low-level kernel (patches 0001-0195) against the taimen hardware facts
(`live.dts`, `hw/iomem.txt`, NOTES-taimen.md).

## 1. SMP / secondary bring-up (`cpus=8`)

**Verdict: no Pixel 1 code mismatch; the path is generic enough for taimen.**

- PSCI conduit `smc #0`, CPU_ON 0xC4000003 (`SweetCPU.cpp:32-36`); DTS `arm,psci-1.0`,
  `method = "smc"`. Entry `ml_static_vtop(sweet_secondary_entry)`.
- MPIDR handling: `start.s:146-147` masks Aff1|Aff0; `SweetCPU.cpp:80` sanity (Aff0 < 16,
  phys_id <= 0xffff); ICC_SGI1R built with Aff1 in bits 16-23. All fit 0x0-0x3 / 0x100-0x103.
- GICR lookup by affinity over the 8 frames (`pe_fiq.c:86-114`), ADT GICR size 0x100000, stride
  default 0x20000. The ARE_NS fix runs per CPU and is a no-op after CPU0.
- Per-CPU timer: `arm_init.c:758-760` runs `PE_init_cpu(); fiq_context_init(TRUE)` on every
  secondary, enabling PPI 19 + SGI 1 in that CPU's GICR and CNTV.
- Entry at EL1 under QHEE; the EL2 block in `start.s:296-324` is never used.
- 4K secondary page tables: TTBR0 = bootstrap tables, TTBR1 = `cpu_ttep`; nothing under
  `__ARM_16K_PG__`; `cpu.c:908-918` cleans the handoff structures to PoC before CPU_ON.
- Topology: `cluster-type` 0 for every CPU → one SMP pset, `MAX_CPU_CLUSTERS 1` is safe; no
  P/E, DynamIQ or AMX code reachable. No `cpu-impl-reg`/`coresight-reg` in the ADT.

**To fix or watch before `cpus=8` (ranked):**

1. **TAIMEN diagnostics in `pe_fiq.c` ran on every secondary** (window scan, per-frame printf,
   `taimen_gic_dump`, 15 early-hex lines) inside splsched while the boot CPU waits in a 5 s
   alive window; the console kick waits up to 50 ms per line. Fixed in 0197: diagnostics once,
   on the first call. `taimen_gicr_pe_base` is a single static overwritten by each secondary,
   so later dumps from the WDT thread report the last-booted CPU's frame.
2. **Set/way cache ops use geometry read once on the boot (silver) core** (`cpu.c:333,344,615,
   1059`); gold cores have a different L1. The sleep/halt paths panic on SWEET anyway; check
   whether `cpu.c:615` (cpu_signal path) is reachable before enabling gold cores.
3. **`BOOTSTRAP_TABLE_SIZE` = 8 pages** on taimen (`pmap.h:115`, no ARM_LARGE_MEMORY); `start.s:
   707-741` adds two extra L2 pages (FB at L1 index 2, MDP at L1 index 0); the only overflow check
   is DEVELOPMENT-only (`arm_vm_init.c:1996-2000`). Assert in RELEASE.
4. Cosmetic: "Kryo 470" name, Redmi comments, `cluster_core_id = phys_id >> 8`.

Untested runtime assumptions for the first `cpus=8` log: QHEE returns CPU_ON at EL1 with
ICC_SRE.SRE allowed per CPU; GICR_WAKER ChildrenAsleep clears for frames 1-7 (1 s timeout);
gold-cluster power-up through PSCI with the cluster L2 off at boot.

## 2. Interrupt routing beyond the timer: clean

Only two SPIs are coded, both in drivers gated off on taimen: `TOUCH_IRQ_INTID 240` (TLMM
summary; taimen's is also SPI 208 = INTID 240 but at TLMM 0x3400000) and `WL_IRQ_INTID 276`.
UFS is polled (taimen UFS irq would be SPI 265 = INTID 297). WDT bark SPI 3 is not attached; the
kernel pets instead. GPU/USB/display have no IRQ attach. `sleh.c` dispatch is generic.
Hardening: `pe_init_fiq` never clears `GICD_ISENABLERn` at handoff, so any SPI ABL left enabled
fires into the unhandled branch with no device ack; add the dump or write ~0 to
`GICD_ICENABLER(1..31)` once, like Linux `gic_dist_config`, before `cpus=8`.

## 3. Clocks / timers / PMU

- Timebase 19200000 from the ADT matches CNTFRQ 0x124f800. `SweetPlatformExpert.cpp:57`
  hard-codes 19200000 for a busy wait (correct value; could use `gPEClockFrequencyInfo`).
- **CNTP:** the only access on the live path is the write `CNTP_CTL_EL0 = IMASKED` in
  `_enable_virtual_timer` (`machine_routines.c:2277`), executed on every boot and survived. The
  note "reading CNTP_CTL_EL0 resets the phone" is unproven: 0192 removed ICC_HPPIR0_EL1 and the
  CNTP read together, and HPPIR0 came first in the printf, so the CNTP read was never evaluated.
  Do not reintroduce HPPIR0. `cntpct_el0` reads are fine.
- PMU/kpc/monotonic: no PMCR/PMCCNTR/PMUSERENR access anywhere; `PERFMON_BASE` undefined so
  MONOTONIC is off; kpc is a stub. Clean.
- Watchdog 0x17817000 (RST +4, EN +8, BARK +0x10, BITE +0x14, 20-bit) matches the DTS and iomem.

## 4. Reset / power / panic log

1. **No restart hook.** `PE_halt_restart` was never assigned and no PSCI SYSTEM_RESET existed;
   reboot and panic-restart spun until the APSS WDT bit (~32 s). Fixed in 0197 (PSCI 0x84000009
   via smc).
2. **0187 constants verified but dead code:** IMEM 0x146bf000+0x65c and PS_HOLD 0x10ac000 are
   right, but `sweet_perf_init` is not called on taimen and ABL ignores the IMEM reason anyway.
3. **"gPanicBase is still not initialized" x2:** `SWEET.h` defines PLATFORM_PANIC_LOG_DISABLED,
   so `check_for_panic_log` needs a `pram` node plus `/chosen embedded-panic-log-size`; mkadt emits
   neither. `debug_log_init` is called twice (machine_init and kdp_init). Consequence: paniclog/kdp
   core buffers inert; panic text relies on `sweet_early_panic` into the ramoops zone and the
   console. Cosmetic unless a paniclog is wanted (fix: ADT `pram` node pointing at a carve-out).
4. Early log / ramoops constants (0xb0000000 / 0xb0080000, 1 MiB) match iomem and the DTS;
   `sweet_early` is single-writer and every secondary's early-hex call is a no-op after
   `sweet_early_disable`.

## 5. Other MSM8996 constants not behind a TAIMEN ifdef

- `SweetFramebuffer.cpp:547-548` GPU present buffers 0xc0800000 / 0xc1000000: Linux system
  RAM on taimen, outside XNU's window; dead twice over today.
- `pe_serial.c:1051-1075` derives VIG0 / INTR / DSI0 from the ADT kickoff register, all matching
  the DTS; the flip reprograms LAYER/FLUSH for LM0 only (display track).
- bsd/dev drivers keep every Pixel 1 base (`sweet_perf.c`, `sweet_keys.c`, `sweet_touch.c`,
  `sailfish_wlan.c` incl. WL_DMA 0xa0000000 = ABL's ramdisk on taimen, `sweet_gpu.c`); all
  unreachable via the `sweet_ufs.c:663` gate.
- No other MSM8996 physical address in osfmk/pexpert outside `SWEET.h:43-44` (overridden by
  TAIMEN.h).

## Summary ranking

1. per-CPU diagnostics in `pe_fiq.c` (fixed, 0197); 2. no restart hook (fixed, 0197);
3. GICD SPIs not masked at handoff (hardening); 4. BOOTSTRAP_TABLE_SIZE budget (assert);
5. set/way ops with boot-core geometry; 6. gPanicBase/pram, GPU buffer PAs, 19200000 literal,
Kryo 470 name. Clean: PSCI conduit, MPIDR/SGI/GICR affinity, per-CPU timer, 4K secondary
tables, topology, SPI numbers, timebase, CNTP (one proven-safe write), PMU, WDT, IMEM/PS_HOLD.
