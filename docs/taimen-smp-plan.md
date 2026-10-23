# Plan: starting the secondary CPUs on taimen from XNU (2026-10-23)

Plan only: nothing was built, booted or committed. Sources read: `scratch/xnu` at `7defcb6` (patches
0001-0216; line numbers below refer to that tree), `port/` (NOTES-taimen.md, docs, tools, xnu/patches),
`research/taimen/live.dts`, and the saved boot logs in `out/` (the log reader's Linux dmesg included).
Statements marked **(unverified)** are inferences that no log or source in hand confirms.

## 0. Short version

- Linux on this phone starts all 8 cores with standard PSCI `CPU_ON` (SMC64 id `0xC4000003`) through
  `smc #0`, target = MPIDR, entry = physical address of `secondary_entry`, context id 0. There is no
  Qualcomm-specific step (no kpss-acc, spin-table or SCM boot-address call). The cores come up at EL1.
  The reader's Linux does exactly this on every reader boot (`out/taimen-xnu-r33/reader/dmesg.txt:66-76,
  132-150`: "PSCIv1.0 detected in firmware", CPU1-CPU7 booted, "All CPU(s) started at EL1").
- XNU already has the complete path (SweetCPU + `sweet_secondary_entry`, from sweet; the Pixel 1 port ran
  `cpus=4` with it on MSM8996, which also has a Qualcomm hypervisor). `cpus=N` (default 1 on SWEET) selects
  the first N ADT cpu nodes, so `cpus=2` starts exactly one silver core (MPIDR 0x1).
- The SYSTEM_RESET result (x0 returned untouched by both `smc` and `hvc`) is not explained by a missing
  firmware implementation: the same call reset the phone from the shim, and a compliant implementation
  never returns x0 unchanged. The call most likely did not reach PSCI at all (dropped at EL2,
  **unverified**). XNU issues CPU_ON from the same kind of context (a kernel thread with IRQs on), so the
  first experiment probes PSCI with harmless queries (`PSCI_VERSION`, `PSCI_FEATURES`, `AFFINITY_INFO`) in
  three contexts and only then calls CPU_ON for one core.
- Two taimen-specific changes before any secondary runs: never write `GICR_WAKER` (Linux on this phone
  never does; on MSM8996 touching it reboots the board), and add MMU-off breadcrumbs so that a core that
  starts and dies is visible in the log. Recommended in the same patch: stop using set/way whole-cache
  operations for large flushes (`c_bulksize_op = 0`), which are wrong once a second core holds dirty lines.

## 1. How Linux brings up the secondaries (question 1)

### Device tree (`research/taimen/live.dts`)

| item | value | lines |
|---|---|---|
| cpu nodes | `cpu@0..3` (reg 0x0-0x3), `cpu@100..103` (reg 0x100-0x103), `#address-cells = 2` | 17-246; reg at 23, 56, 83, 110, 137, 169, 196, 223 |
| enable-method | `"psci"` on all eight | 26, 59, 86, 113, 140, 172, 199, 226 |
| clusters | cluster0 = cpu@0..3, cluster1 = cpu@100..103 | 247-276 |
| psci node | `compatible = "arm,psci-1.0"; method = "smc";` and no `cpu_on`/function-id properties | 16337-16340 |
| GIC | GICv3, GICD 0x17a00000 + 0x10000, GICR 0x17b00000 + 0x100000, `redistributor-stride = 0x20000`, ITS at 0x17a20000 | 682-699 |
| arch timer | PPIs 1/2/3/0 (secure phys, NS phys, virt, hyp), 19.2 MHz | 700-704 |
| reboot | `restart@10ac000`, `compatible = "qcom,pshold"` | 705-709 |
| cpu idle | `qcom,lpm-levels` with `qcom,use-psci` (PSCI CPU_SUSPEND power states) | 5035-5153 |

No node uses `qcom,kpss-acc-v2`, `qcom,arm-cortex-acc` or `spin-table`; the only `qcom,acc-*` property
(line 5001) belongs to an unrelated block.

### Linux code path

- `drivers/firmware/psci.c` (v4.4): the conduit comes from the DT `method` ("smc" -> `__invoke_psci_fn_smc`);
  with PSCI 0.2+ `psci_function_id[PSCI_FN_CPU_ON] = PSCI_FN_NATIVE(0_2, CPU_ON)`, which on arm64 is the
  SMC64 id `0xC4000003`; `psci_cpu_on()` calls `invoke_psci_fn(fn, cpuid, entry_point, 0)`, so the
  **context id is 0**. https://github.com/torvalds/linux/blob/v4.4/drivers/firmware/psci.c and the same
  in the wahoo tree: https://github.com/LineageOS/android_kernel_google_wahoo/blob/lineage-19.1/drivers/firmware/psci.c
- `arch/arm64/kernel/psci.c`: `cpu_psci_cpu_boot()` passes `cpu_logical_map(cpu)` (the MPIDR from `reg`)
  and `__pa_symbol(secondary_entry)` (v4.4: `__pa(secondary_entry)`). The wahoo copy contains nothing
  Qualcomm-specific: https://github.com/LineageOS/android_kernel_google_wahoo/blob/lineage-19.1/arch/arm64/kernel/psci.c
- The Qualcomm SCM cold/warm boot-address calls and the kpss-acc release belong to the 32-bit
  `arch/arm/mach-qcom` enable-methods; this DT does not select them.
- Inside the firmware (described for MSM8916 by the qhypstub project; assumed similar on MSM8998,
  **unverified**): the kernel's SMC goes to TZ, TZ powers the core and asks the hypervisor to initialise EL2
  and jump to the requested entry at EL1. https://github.com/msm8916-mainline/qhypstub
- Proof on this phone: the log reader's Linux (4.4.169, `dmesg.txt:6`) prints "psci: PSCIv1.0 detected in
  firmware", "Using standard PSCI v0.2 function IDs", "MIGRATE_INFO_TYPE not supported" (lines 66-69),
  boots CPU1-3 with MIDR 0x51af8014 (silver) and CPU4-7 with 0x51af8001 (gold), about 12 ms apart (lines
  132-145), "Brought up 8 CPUs", "All CPU(s) started at EL1" (146-150), timers "virt/virt" (110).

### What XNU passes (`iokit/Kernel/arm/SweetCPU.cpp`)

`sweet_psci_cpu_on()` (28-40): `x0 = 0xC4000003`, `x1 = target MPIDR`, `x2 = entry PA`, `x3 = context`,
`smc #0`, x4-x17 declared clobbered (SMCCC 1.0). `startCPU()` (235-255) uses target = ADT `reg`
(`phys_id`), entry = `ml_static_vtop(&sweet_secondary_entry)`, context = logical CPU number, and panics
on any non-zero result. Function id width, conduit, immediate and register use match Linux. The only
difference is the entry address: XNU's is above 4 GiB (kernel RAM at 0x1_0000_0000+), Linux's is in the
low bank (kernel at 0x80080000). Whether this TZ accepts an entry above 4 GiB is **unverified**; SMC64
CPU_ON takes a 64-bit address, and `INVALID_ADDRESS` (-9) would be the answer if it does not.

## 2. Why SYSTEM_RESET from XNU came back with x0 unchanged (question 2)

### What was issued

- 0197 `taimen_halt_restart` (`port/xnu/patches/0197-taimen-audit-quick-fixes.patch:59-71`) and 0199's ladder
  (`0199-taimen-reset-ladder.patch`, commit `279c5a2`): `x0 = 0x84000009, x1 = x2 = x3 = 0;
  "dsb sy; smc #0"` (then the same with `hvc #0`), from the WDT pet thread (`taimen_wdt_thread`, now
  `SweetPlatformExpert.cpp:210-259`): a preemptible kernel thread with IRQs enabled, GIC Group 1 enabled,
  timer interrupts flowing, MMU on, 60-85 s after kernel start.
- Result: boot 14 nothing happened; boot 15 "smc SYSTEM_RESET returned x0 = 0x84000009 untouched; hvc the
  same" (NOTES-taimen.md:208-216). No exception, no panic.

### Checks

| possible mistake | finding |
|---|---|
| function id width | Correct. SYSTEM_RESET exists only as the SMC32 id 0x84000009 (PSCI 0.2+). CPU_ON must be the SMC64 0xC4000003 to carry a 64-bit entry; SweetCPU uses that. |
| `smc` immediate | `#0`, as SMCCC requires. |
| registers | x1-x3 zeroed. The 0197/0199 asm does not declare x4-x17 clobbered (SMCCC 1.0 allows firmware to clobber them); a latent bug for calls that return, not an explanation for x0. SweetCPU's helper declares them. |
| MMU / caches on | Allowed: Linux makes every PSCI call with the MMU on. |
| exception level / security state | NS EL1, the same as Linux and as the shim. The SMC executed (an undefined SMC would have trapped into XNU and panicked). |
| firmware lacks SYSTEM_RESET | No. The shim's identical call (`tools/granule-probe-fb.S:46-58`, `port/shim/taimen.c:47-56`) reset the phone with bootreason "reboot" on 2026-10-22 (NOTES-taimen.md:54-57, 69-71). Linux never uses it at runtime: `msm-poweroff` replaces `arm_pm_restart` with `do_msm_restart` (SCM dload/arbiter calls, then PS_HOLD) (https://github.com/LineageOS/android_kernel_google_wahoo/blob/lineage-19.1/drivers/power/reset/msm-poweroff.c), driven by `live.dts:705-709`. Mainline `msm8998.dtsi` has no `qcom,pshold` node, so a mainline kernel would reboot through PSCI (https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8998.dtsi; not confirmed to work on taimen). |
| result encoding | A PSCI implementation returns 0 or a negative code, and SMCCC requires -1 (`NOT_SUPPORTED`) for an unknown id (https://developer.arm.com/documentation/den0022/latest, https://developer.arm.com/documentation/den0028/latest). An untouched x0 is neither, so the call did not complete inside PSCI. |

### Interpretation

- Both conduits returned the same way, which points at an EL2 handler that returns (ERET) without writing
  x0 (**unverified**). QHEE does trap selected SMCs on Qualcomm SoCs (the PAS `auth_and_reset` call "is first
  trapped by QHEE": https://lkml.iu.edu/hypermail/linux/kernel/2510.0/04426.html).
- Why the shim's call worked and XNU's did not is open. Differences: the shim ran seconds after ABL, with the
  MMU off, DAIF masked (`granule-probe-fb.S:27`) and GICD_CTLR still 0 as ABL leaves it (NOTES-taimen.md:154),
  so no Group 1 NS interrupt could be pending; XNU ran minutes in,
  in a thread with IRQs on and the vtimer PPI live. Hypothesis H-IRQ: the firmware abandons the call when
  a non-secure interrupt is pending and returns with registers untouched (**unverified**). Section 5.2's
  probe tests this for fast calls directly.
- The APSS watchdog bite (on Qualcomm SoCs serviced by TZ, which then resets; **unverified** for this one)
  has not worked since boot 13 (NOTES-taimen.md:204, 213-216). Whether both failures share a cause is unknown. It matters here because it removes the
  hardware safety net (section 4).
- Consequence for CPU_ON: not inferable from SYSTEM_RESET either way. Linux uses CPU_ON on this firmware at
  every boot, and the Pixel 1 XNU used the identical SweetCPU code (`docs/upstream-sailfish-README.md:24,
  87`, `cpus=4`). But `cpu_start()` is called with interrupts enabled (`osfmk/kern/processor.c:858` splx, then
  `:912-915`), the same context in which SYSTEM_RESET was dropped. If CPU_ON is dropped the same way, x0
  comes back as 0xC4000003, `rc = (int32_t)0xC4000003 = -1006632957`, and the existing panic in
  `SweetCPU.cpp:251-253` fires, which is bounded (PS_HOLD, log reader).

## 3. The XNU start path on SWEET/TAIMEN and what taimen needs (question 3)

### Selection: `cpus=`

- `ml_parse_cpu_topology()` (`osfmk/arm64/machine_routines.c:943-1011`): on SWEET, a missing `cpus=` or 0
  means 1, values above `SWEET_SMP_MAX_CPUS` (8, `SWEET.h:26`) are clamped (957-964). The topology takes the
  first N `/cpus` children in ADT order. `mkadt.py --taimen` emits them as 0x0, 0x1, 0x2, 0x3, 0x100..0x103
  (`port/tools/mkadt.py:61-62, 192-212`; `state` "running" only for cpu0). So `cpus=2` = MPIDR 0x0 + 0x1,
  `cpus=4` = all silver, `cpus=5` adds the first gold core.
- `cpumask=` is unusable on SWEET: without `cpus=` it is forced to 1, and with both set `cpu_config_correct =
  false` (969-971), which panics later.
- `boot-xnu.sh:30` hard-codes `cpus=1` before the extra args. `PE_parse_boot_argn` keeps scanning and the
  last occurrence wins (`pexpert/gen/bootargs.c:140-148, 173-185`), so passing `cpus=2` as an extra arg
  works.
- Nubs outside the topology get no `logical-cpu-id` (`IOPlatformExpert.cpp:1656-1659`), and `SweetCPU::start`
  returns false for them (`SweetCPU.cpp:65-71`). Already exercised with `cpus=1` and 8 nodes.

### Start sequence

1. `SweetCPU::start` (`SweetCPU.cpp:61-125`) registers each selected CPU with `ml_processor_register`; the
   boot CPU's instance spawns `startAll` (129-182). It waits ≤10 s for all registrations, then for each CPU
   calls `processor_start()` and waits ≤5 s for `initialized` plus the CPU's `SIGPdisabled` to clear (the
   self-SGI went through). Boot 12 onward logs "cpu=0 ... came alive; SGI1 OK".
2. `processor_start` -> `cpu_start(cpu)` (`osfmk/arm64/cpu.c:897-968`): `cpu_reset_handler =
   start_cpu_paddr`, active thread = startup/idle thread, then cleans `CpuDataEntries`, the target cpu_data,
   `ResetHandlerData`, `BootArgs`, `sysreg_restore`, `cpu_ttep` and `bootstrap_pagetables` to PoC (930-941,
   SWEET-only), then `PE_cpu_start(cpu_id, NULL, NULL)` -> `IOCPU::startCPU` (`IOCPU.cpp:90-99`) ->
   `SweetCPU::startCPU` -> PSCI CPU_ON.
3. Per-CPU data and stacks: secondary `cpu_data` lives in the percpu area from `pmap_steal_memory` (physically
   contiguous, `osfmk/arm/cpu_common.c:729-768`), stacks from `cpu_stack_alloc` (KVAs used only after the MMU
   is on). `ResetHandlerData.cpu_data_entries = ml_static_vtop(CpuDataEntries)` and `start_cpu_paddr`
   (`cpu.c:1217-1218, 1243`).
4. Entry (`osfmk/arm64/start.s:282-330`): `sweet_secondary_entry` masks DAIF, accepts EL1 (taimen) or EL2
   (unused: the hypervisor keeps EL2), sets SPSel=1 and branches to `LowResetVectorBase`. The context in x0 is
   ignored.
5. `reset_vector` (104-196), MMU off: VBAR = `LowExceptionVectorBase` (every vector is `b .`), looks up the
   CPU by `MPIDR & 0xFFFF` in `CpuDataEntries` (146-164), checks the handler is `start_cpu`, branches. If not
   found it spins at `Lskip_cpu_reset_handler`.
6. `start_cpu` (340-398): x20 = `const_boot_args` PA, x22/x23 virt/phys base, x25 = `bootstrap_pagetables` PA,
   TPIDR_EL0 = cpu number, SP_EL1 = exception stack, SP_EL0 = interrupt stack, lr = `arm_init_cpu` KVA.
7. `common_start` (782-975): TCR from `sysreg_restore` (4K: `TCR_EL1_BOOT`, T0SZ = T1SZ = 25, IPS 40 bits,
   `proc_reg.h:956, 974, 982, 1004`), TTBR0 = the bootstrap V=P tables (the same ones CPU0 booted on; they stay
   resident, `arm_vm_init.c:87, 2013-2018`), TTBR1 = `cpu_ttep` (869-873), MAIR, VBAR = `ExceptionVectorsBase`
   KVA, SCTLR, CPACR, then `Ltrampoline` -> `arm_init_tramp` -> `arm_init_cpu`.
8. `arm_init_cpu` (`osfmk/arm/arm_init.c:676-801`): current thread, `wfe_timeout_init`, `pmap_clear_user_ttb`,
   `cpu_init`, `cpu_timebase_init`, `PE_init_cpu()` -> `pe_init_fiq()` (GIC), `fiq_context_init()` (CNTV on, CNTP
   masked, `machine_routines.c:2271-2286`), `slave_main` -> `cpu_machine_init` -> `PE_cpu_machine_init(.., boot =
   !StartedState)` (`cpu_common.c:681-697`) -> `SweetCPU::initCPU(true)` (191-210): self-SGI, state Running,
   `initialized = 1`. SGI 1 arrives through `sleh_irq` (`osfmk/arm64/sleh.c:1981-1986`) and clears
   `SIGPdisabled`.

### What the entry must cope with on taimen

| topic | requirement | status |
|---|---|---|
| exception level | firmware returns at the caller's EL; Linux: "All CPU(s) started at EL1" | EL1 branch, no change |
| MMU/caches | PSCI entry: MMU and D-cache off, DAIF masked, x0 = context | handled; all MMU-off reads are cleaned to PoC (`cpu.c:930-941`) |
| 4K granule | bootstrap tables and TCR are 4K, from start.s; same tables as CPU0 | no change. Budget: `BOOTSTRAP_TABLE_SIZE` = 8 pages (`pmap.h:115`) |
| GICD | ARE_NS set once by CPU0 (0191, `pe_fiq.c:288-303`); later writes of ARE_NS\|Grp1A\|Grp1 are idempotent | no change |
| GICR frame | found by affinity at stride 0x20000 over 0x100000 (`pe_fiq.c:87-117`); frames +0x20000 TYPER 0x100000101 (aff 0x1) ... +0xe0000 TYPER 0x10300000711 (aff 0x103, Last) (`out/taimen-xnu-r33/sweetlog.txt:2-17`) | no change |
| GICR_WAKER | all 8 frames read `WAKER 0x0` at handoff (same log), so `pe_fiq.c:308-323` will not write. Linux on taimen never writes it: wahoo `gic_enable_redist()` is an empty stub unless `CONFIG_ARM_GIC_V3_NO_ACCESS_CONTROL`, which `wahoo_defconfig` leaves unset (`CONFIG_ARM_GIC_V3_ACL=y`; Kconfig help: some GIC registers may not be accessed from the non-secure world). On MSM8996 the hypervisor restricts GICR_WAKER and touching it reboots the board (mainline quirk `qcom,msm8996-gic-v3`) | change: never write it on TAIMEN; log it |
| SGI/PPI config | `GICR_IGROUPR0 = ~0` (RAZ/WI from NS, harmless), priorities 0xA0, `ISENABLER0 = PPI19 \| SGI1`, `ICC_SRE`, `ICC_BPR1 = 0`, `ICC_PMR = 0xFF`, EOImode 0, `ICC_IGRPEN1 = 1` (`pe_fiq.c:332-378`). Linux does the same set (PMR 0xF0) | no change. SGI 1 must be NS Group 1 on every frame; Linux's IPIs (SGIs 0-6) imply it (**unverified** per frame) |
| timer | CNTV (vtimer INTID 19) per CPU; Linux uses virt too, so CNTVOFF must already be consistent across cores | no change |
| trapped registers | avoid `ICC_*0_EL1` and CNTP reads (boots 7-9 reset, NOTES-taimen.md:164-168); the secondary path only writes `CNTP_CTL_EL0 = IMASK`, which CPU0 survives | no change |
| per-CPU diagnostics | `taimen_fiq_done` keeps the frame scan to CPU0 (0197). `taimen_gicr_pe_base` is overwritten by each secondary, so the WDT thread's dumps show the last-started frame (cosmetic) | optional |
| cache maintenance | set/way whole-cache ops act on the executing core only; used when a range ≥ `c_bulksize_op` (= L2 size, `osfmk/arm/cpuid.c:367, 389`) in `flush_dcache_syscall`, `dcache_incoherent_io_flush64/store64` and `platform_cache_flush_wimg` (`osfmk/arm/caches.c:128-154, 197-216, 355-379`). The UFS driver uses VA-range ops (`sweet_ufs.c:136-146`), which broadcast | change: `c_bulksize_op = 0` on TAIMEN |

## 4. Risks and how each attempt stays bounded and visible (question 4)

| risk | effect | containment |
|---|---|---|
| CPU_ON dropped like SYSTEM_RESET | raw x0 = 0xC4000003 | the probe (5.2) detects it before CPU_ON; otherwise the existing panic -> PS_HOLD -> reader |
| CPU_ON refused | -2 INVALID_PARAMETERS, -3 DENIED, -4 ALREADY_ON, -6 INTERNAL_FAILURE, -9 INVALID_ADDRESS (entry above 4 GiB?) | logged and decoded, then panic; -9 -> plan B2 (6.3) |
| core never reaches the entry | rc 0, no breadcrumb, `AFFINITY_INFO` stays OFF/ON_PENDING | 5 s timeout panic with crumb + AFFINITY_INFO state |
| fault with the MMU off | spins in `LowExceptionVectorBase` | breadcrumb exception stub records ESR/ELR/FAR; CPU0 panics after 5 s |
| fault after the MMU is on | `sleh` panic on the secondary | ordinary panic path -> PS_HOLD (`debug=0x10000c`, `boot-xnu.sh:24-27`) |
| secondary touches a protected register (`GICR_WAKER` write) | phone resets at once (MSM8996 precedent) | 5.5 removes the write; RAM survives, the log reader recovers pstore |
| whole-system hang (CPU0 spinning with IRQs off on a lock held by a dead secondary, or the SMC never returns) | no reset: the APSS WDT has not bitten since boot 13 and the deadline runs in a thread | autoboot.sh exit 3 ("needs the power key"); hold power; pstore keeps the log (boot 25, NOTES-taimen.md:339). Line before the SMC is logged, so a hang inside it is identifiable |
| SMP races in code written for `cpus=1` | display or log glitches, rarely a panic | the fbcon kick already has an atomic lock (`pe_serial.c:782, 882, 939`); `taimen_gicr_pe_base`, `sweet_irq_seen` are cosmetic; watch the first SMP runs |
| set/way ops on SMP | stale lines on DMA or cache-mode changes | 5.7 |
| gold cluster | first gold CPU_ON powers cluster 1 and its L2; different L1 size; unknown default clock | separate step (E3), after the silver cores are proven |
| early-boot time | each core may use its 5 s window | Linux needs ~12 ms per core; total budget acceptable |

Observability: everything below goes through `IOLog`/`printf` into the pstore PRZ and the UFS console ring,
which the log reader returns after the PS_HOLD reset (`autoboot.sh`). Keep `taimenreboot=` short for SMP
experiments (180 s) so a run that does not hang returns by itself.

## 5. Patch 0217 (taimen-smp-bringup): what to write

All code TAIMEN-only (`#if defined(ARM64_BOARD_CONFIG_TAIMEN)` in C, `#if defined(TAIMEN)` in start.s).

### 5.1 Breadcrumbs (`osfmk/arm64/cpu.c`, `osfmk/arm64/start.s`)

One 64-byte line per core, slot = MPIDR.Aff1 * 4 + Aff0 (0-7). The secondary writes with the MMU off
(non-cacheable); CPU0 zeroes and cleans the slot before CPU_ON, then reads it after `dc civac` (CPU0 never
writes the line in between, so the clean+invalidate cannot overwrite the secondary's data).

```c
/* cpu.c */
struct taimen_smp_crumb {
	uint64_t stage, mpidr, v[4], t, pad;   /* +0, +8, +16..+40, +48 */
} __attribute__((aligned(64)));
struct taimen_smp_crumb taimen_smp_crumbs[8] __attribute__((aligned(64)));

static inline struct taimen_smp_crumb *
taimen_crumb_slot(uint64_t mpidr)
{
	return &taimen_smp_crumbs[(((mpidr >> 8) & 1) << 2) | (mpidr & 3)];
}
void taimen_smp_mark(uint64_t stage, uint64_t a, uint64_t b)        /* secondary, MMU on */
{
	struct taimen_smp_crumb *c = taimen_crumb_slot(__builtin_arm_rsr64("MPIDR_EL1"));
	c->v[0] = a; c->v[1] = b; c->t = __builtin_arm_rsr64("CNTVCT_EL0");
	__atomic_store_n(&c->stage, stage, __ATOMIC_RELEASE);
	__builtin_arm_dsb(DSB_ISH);
}
void taimen_smp_crumb_clear(uint32_t mpidr)                            /* CPU0, before CPU_ON */
{
	struct taimen_smp_crumb *c = taimen_crumb_slot(mpidr);
	bzero(c, sizeof(*c));
	__asm__ volatile ("dc civac, %0" : : "r"(c) : "memory");
	__builtin_arm_dsb(DSB_SY);
}
void taimen_smp_crumb_read(uint32_t mpidr, uint64_t out[8])           /* CPU0, polling */
{
	volatile uint64_t *c = (volatile uint64_t *)taimen_crumb_slot(mpidr);
	__asm__ volatile ("dc civac, %0" : : "r"(c) : "memory");
	__builtin_arm_dsb(DSB_SY);
	for (int i = 0; i < 8; i++) out[i] = c[i];
}
```

start.s macro (Apple assembler argument syntax `$n`; clobbers x9-x12, stage in a register so the
exception stub can pass a computed one):

```asm
#if defined(TAIMEN)
.macro TAIMEN_CRUMB		// $0 = stage reg, $1-$3 = value regs (not x9-x12)
	mrs	x10, MPIDR_EL1
	and	x11, x10, #3
	ubfx	x12, x10, #8, #1
	orr	x11, x11, x12, lsl #2
	adrp	x9, EXT(taimen_smp_crumbs)@page		// PA while the MMU is off, V=P after
	add	x9, x9, EXT(taimen_smp_crumbs)@pageoff
	add	x9, x9, x11, lsl #6
	str	x10, [x9, #8]
	str	$1, [x9, #16]
	str	$2, [x9, #24]
	str	$3, [x9, #32]
	str	$0, [x9]
	dsb	sy
.endmacro
#endif
```

Call sites (each loads the stage first with `mov x13, #stage`, then `TAIMEN_CRUMB x13, a, b, c`; x13
is free at all of them, and the registers the surrounding code still needs, x0, x1, x15, x21, are only read):

| stage | where | values |
|---|---|---|
| 0x01 | `sweet_secondary_entry`, right after `msr DAIFSet` (start.s:291): `mrs x1, CurrentEL; mrs x2, SCTLR_EL1` (x1 is compared right after, unchanged) | x0 (context), x1 (CurrentEL), x2 (SCTLR_EL1) |
| 0x02 | `reset_vector`, just before `blr x0` (start.s:194) | x21 (cpu data PA), x0 (handler), x15 (MPIDR) |
| 0xE1 | `Lskip_cpu_reset_handler` (start.s:195), before `b .` | x15, x0, x21: no cpu data for this MPIDR or wrong handler |
| 0x03 | first instruction of `Ltrampoline` (start.s:967), MMU on, still V=P | x0 (trampoline KVA), x21, xzr |
| 0xF0+n | `LowExceptionVectorBase` (start.s:232-270): replace the 8 EL1 `b .` entries (n = 0-3 SP0 sync/IRQ/FIQ/SError, 4-7 SPx) with `mov x13, #(0xF0 + n); b Ltaimen_lowvec`; `Ltaimen_lowvec` (after the `.align 12, 0`) reads ESR/ELR/FAR into x14-x16, runs `TAIMEN_CRUMB x13, x14, x15, x16`, then `b .` | ESR_EL1, ELR_EL1, FAR_EL1 |

C marks (`taimen_smp_mark`) in `arm_init_cpu` (`osfmk/arm/arm_init.c`), secondaries only
(`cpu_data_ptr != &BootCpuData`): 0x04 after `machine_set_current_thread` (691), 0x05 after `cpu_init`
(729), 0x06 after `cpu_timebase_init` (739), 0x07 from `pe_init_fiq` after the frame lookup (a = frame
index, b = GICR_WAKER, see 5.5), 0x08 after `fiq_context_init` (760), 0x09 before `slave_main` (800);
0x0A in `SweetCPU::initCPU` (a = DAIF).

### 5.2 PSCI helper and probe (`iokit/Kernel/arm/SweetCPU.cpp`)

```c
enum { TPSCI_THREAD, TPSCI_NOIRQ, TPSCI_NOIRQ_PMR0 };
static int taimen_psci_ctx = -1;           /* chosen by the probe */

static uint64_t
taimen_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, bool hvc, int ctx)
{
	boolean_t istate = TRUE;
	uint64_t pmr = 0;
	if (ctx != TPSCI_THREAD) {
		istate = ml_set_interrupts_enabled(FALSE);
	}
	if (ctx == TPSCI_NOIRQ_PMR0) {        /* NS PMR 0: no NS Group 1 interrupt is signalled */
		pmr = __builtin_arm_rsr64("ICC_PMR_EL1");
		__builtin_arm_wsr64("ICC_PMR_EL1", 0);
		__builtin_arm_isb(ISB_SY);
	}
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	if (hvc) {
		__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
		    "x14", "x15", "x16", "x17", "cc", "memory");
	} else {
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
		    "x14", "x15", "x16", "x17", "cc", "memory");
	}
	uint64_t r = x0;
	if (ctx == TPSCI_NOIRQ_PMR0) {
		__builtin_arm_wsr64("ICC_PMR_EL1", pmr);
		__builtin_arm_isb(ISB_SY);
	}
	if (ctx != TPSCI_THREAD) {
		ml_set_interrupts_enabled(istate);
	}
	return r;
}
```

Probe, at the top of `startAll` after the registration wait (runs with any `cpus=`), for ctx in
{THREAD, NOIRQ, NOIRQ_PMR0}, every result logged raw as `TAIMEN PSCI [ctx] fn a1 -> x0` with "UNCHANGED"
appended when x0 == fn:

| call | x0 | x1 | x2 | expected |
|---|---|---|---|---|
| PSCI_VERSION | 0x84000000 | 0 | 0 | 0x00010000 (Linux: "PSCIv1.0") |
| PSCI_FEATURES(CPU_ON) | 0x8400000A | 0xC4000003 | 0 | 0 (or ≥ 0 flags) |
| PSCI_FEATURES(AFFINITY_INFO) | 0x8400000A | 0xC4000004 | 0 | 0 |
| PSCI_FEATURES(SYSTEM_RESET) | 0x8400000A | 0x84000009 | 0 | 0 |
| AFFINITY_INFO x 8 | 0xC4000004 | 0x0, 0x1, 0x2, 0x3, 0x100-0x103 | 0 | 0 (ON) for 0x0, 1 (OFF) for the rest |

Plus one `hvc` PSCI_VERSION in the THREAD context, for comparison only.

Decision: `taimen_psci_ctx` = first ctx whose PSCI_VERSION returned `0x10000 <= x0 < 0x80000000`. If
none and `num_cpus > 1`: `panic("TAIMEN PSCI: firmware did not answer smc in any context ...")` (the probe
lines are already in the log). With `cpus=1` only log. If AFFINITY_INFO(target) is not 1 (OFF) before its
CPU_ON, panic with that value.

### 5.3 `SweetCPU::startCPU` (TAIMEN branch)

1. `taimen_smp_crumb_clear(phys_id)`.
2. `IOLog("SWEET CPU: cpu=%u CPU_ON target=%#x entry_pa=%#llx ctx=%u psci_ctx=%d aff_before=%lld")`.
3. `raw = taimen_psci(0xC4000003, phys_id, entry, getCPUNumber(), false, taimen_psci_ctx)`;
   `rc = (int32_t)raw`.
4. Log `rc`, `raw` and a decoded name (0 SUCCESS, -1 NOT_SUPPORTED, -2 INVALID_PARAMETERS, -3 DENIED,
   -4 ALREADY_ON, -5 ON_PENDING, -6 INTERNAL_FAILURE, -9 INVALID_ADDRESS); if `raw == 0xC4000003` add
   "x0 unchanged: the call did not reach PSCI".
5. Keep the panic on rc != 0.

### 5.4 `SweetCPU::startAll` wait loop (TAIMEN additions)

Inside the existing 5 s loop (`SweetCPU.cpp:162-170`), every iteration: `taimen_smp_crumb_read()`; on a
stage change `IOLog("SWEET CPU: cpu=%u +%llu us crumb %#llx mpidr %#llx v %#llx %#llx %#llx")` (µs since
CPU_ON). Every 100 ms: AFFINITY_INFO(target) in `taimen_psci_ctx`, logged when it changes (1 OFF, 2
ON_PENDING, 0 ON). On timeout: log the full crumb and the last AFFINITY_INFO, then the existing panic. After
"came alive", optionally run `taimen_cpu_speed()` (0216) bound to that CPU (`thread_bind` +
`thread_block`) to learn each core's clock.

### 5.5 `pe_fiq.c`: no GICR_WAKER writes on TAIMEN

In `pe_init_fiq()` (`pexpert/arm/pe_fiq.c:307-323`): on TAIMEN read WAKER, record it (crumb 0x07 with the
frame index `(gicr_pe_base - gicr_base) / GICR_STRIDE`), and skip the write and the wait. If ChildrenAsleep
is ever set, the IRQ-delivery check (SGI1) will fail visibly instead of a register access resetting the
phone.

### 5.6 Remove the remaining per-CPU noise

`kprintf("SWEET GIC dbg ...")` (`pe_fiq.c:380-385`) runs on every secondary with IRQs off; if kprintf
reaches the fbcon it can wait for the panel. Guard it with `!taimen_fiq_done` like the other dumps.

### 5.7 Set/way bulk operations off (`osfmk/arm/cpuid.c:367, 389`)

On TAIMEN set `cpuid_cache_info_p->c_bulksize_op = 0`. Every `c_bulksize_op != 0` test in `caches.c` then
takes the VA-range path (broadcast to the inner-shareable domain), and `platform_cache_batch_wimg()` returns
FALSE. Costs time on very large flushes only. Needed before the userland runs on two cores (E1 boots to
SpringBoard). The effect on `pmap_batch_set_cache_attributes` is **unverified**: check that it falls back to
per-page maintenance.

### 5.8 Boot command

Unchanged scripts, extra args last (they override `cpus=1`):

```
ROOT=1 ROOTFIX=<current blob> tools/taimen/autoboot.sh <kernel r-0217> out/taimen-xnu-<r>-smp2 cpus=2 taimenreboot=180
```

## 6. Experiments

### 6.1 Ladder

| step | args | what it proves | success lines |
|---|---|---|---|
| E0 (optional, zero risk) | 0217 kernel, `cpus=1` | the PSCI answer from XNU in 3 contexts; can ride along with any other boot | `TAIMEN PSCI [thread] 0x84000000 -> 0x10000` |
| E1 | `cpus=2` (MPIDR 0x1, silver, CPU0's cluster: no cluster power-up) | CPU_ON, MMU-off entry, 4K tables, GICR frame 1, SGI1, timer on a 2nd core | crumbs 0x01-0x0A, `cpu=1 mpidr=0x1 came alive; SGI1 OK`, `2/2 CPUs initialized`, then the normal boot to SpringBoard |
| E2 | `cpus=4` | all silver cores, SMP userland | `4/4 CPUs initialized`, no new panics over a long run |
| E3 | `cpus=5` | first gold core (0x100): cluster-1 power-up, frame +0x80000, Aff1 SGI routing | `cpu=4 mpidr=0x100 came alive` |
| E4 | `cpus=8` | everything; then make 8 the default in `boot-xnu.sh` | `8/8 CPUs initialized`, UI speed |

### 6.2 Reading an E1 failure

| log | meaning | next |
|---|---|---|
| PSCI_VERSION unchanged in all three contexts | XNU's SMCs do not reach PSCI | plan B (6.3) |
| unchanged in THREAD, 0x10000 with IRQs masked | H-IRQ holds: issue all PSCI calls masked (the probe already chose that context) | continue |
| CPU_ON raw 0xC4000003 although VERSION answered | CPU_ON specifically intercepted | plan B |
| rc -9 / -2 | entry or target rejected | plan B2 for -9; check the MPIDR for -2 |
| rc 0, no crumb, AFFINITY_INFO 2 then 0 | core on, but it never ran our entry | entry address (plan B2) |
| rc 0, no crumb, AFFINITY_INFO stays 1 | power-up failed in firmware | compare with Linux; plan B |
| crumb 0x01 only | stuck between entry and cpu-data lookup | check x0/CurrentEL/SCTLR in the crumb |
| crumb 0xE1 | `CpuDataEntries` lookup failed (stale data at PoC, or `cpu_phys_id` mismatch) | check cleans, MPIDR mask |
| crumb 0xF0-0xF7 | exception with the MMU off (0xF4 = synchronous on SP1, the usual case) | decode ESR/ELR (ELR is a PA) |
| crumb 0x03 then a panic from cpu 1 | MMU-on fault in the trampoline or arm_init_cpu | panic text |
| crumbs up to 0x0A, no "came alive" | SGI1 not delivered to frame 1 (grouping) or IRQs stay masked | crumb 0x07 (frame, WAKER); GICR_ISPENDR0 of that frame |

### 6.3 Fallbacks (unverified designs)

- **Plan B, shim holding pen:** the shim's SMC worked (SYSTEM_RESET). The STAGE2 shim could CPU_ON the 7
  secondaries before jumping to XNU, park them at EL1 with the MMU off in a WFE loop on per-core mailboxes in
  its own low-RAM image (0x8008xxxx, untouched by XNU), and pass the mailbox addresses in the ADT (like Linux
  `spin-table`'s `cpu-release-addr`). XNU's `startCPU` would write the PA of `sweet_secondary_entry` to the
  mailbox (uncached alias via `ml_io_map`, or clean to PoC), `dsb sy; sev`. Parked cores stay powered;
  they need nothing from the GIC until XNU's `pe_init_fiq`.
- **Plan B2, low trampoline:** if CPU_ON rejects an entry above 4 GiB, write a 16-byte stub
  (`ldr x1, 1f; br x1; 1: .quad <PA of sweet_secondary_entry>`) into a low-bank page XNU never manages (for
  example inside the shim image) through an `ml_io_map` alias, and give its PA to CPU_ON.
- **SYSTEM_RESET retest (optional, explains section 2):** at the deadline, before PS_HOLD, call
  `taimen_psci(0x84000009, 0, 0, 0, false, TPSCI_NOIRQ_PMR0)`. A reset with bootreason "reboot" confirms
  H-IRQ; a return falls through to PS_HOLD as now.

## 7. Open questions

- Why SYSTEM_RESET from XNU returns untouched (EL2 drop vs. pending-interrupt abort vs. state change after
  boot 12); answered in part by E0's three contexts.
- Whether this TZ accepts a CPU_ON entry above 4 GiB.
- Whether SGI 1 is NS Group 1 on all eight redistributors (IGROUPR0 is RAZ from NS; only delivery shows it).
- Why the APSS WDT bite stopped working after boot 12 (no hardware safety net for a hang).
- Default clock of the gold cores without cpufreq (0216 measures CPU0 only).
