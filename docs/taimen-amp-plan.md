# Plan: running taimen's UI-critical threads on the gold cores (2026-10-23)

Plan only: nothing was built, booted or changed in the kernel. Sources read: `scratch/xnu` at `da8a26b`
(patches 0001-0233; the files cited here are unchanged since `0e2d165` = 0230, and line numbers refer to that
tree), `port/` at `8bf1a08` (NOTES-taimen.md, tools, the CI workflow), `scratch/dxb/build.sh`, the r51
kernel's symbol table (`out/kernels/taimen-r51`), the boot 47 and 51 logs (`out/taimen-xnu-r47-gold`,
`out/taimen-xnu-r51-gpu2`), and the iPhone X device tree `research/ipsw-x/d22ap.adt` (dumped with
`tools/adt.py dump`; an Apple file, not for git). Statements marked **(unverified)** are inferences that no
log or source in hand confirms.

## 0. Short version

- Our TAIMEN kernel is **not AMP in any sense**. `__AMP__` is `__ARM_AMP__` (`osfmk/arm/smp.h:34`), which only
  `H13.h:70` defines, and `board_config.h:134-136` turns off again when `MAX_CPU_CLUSTERS == 1` (SWEET/TAIMEN:
  `board_config.h:108`). The ADT marks all eight cpus `cluster-type 0` (`tools/mkadt.py:201`), so xnu sees one
  SMP cluster, one processor set, and the Clutch scheduler. The r51 kernel has `_sched_clutch_dispatch` and no
  `_sched_amp_dispatch` or `_sched_edge_dispatch`.
- The stock placement is **biased toward silver**, not just blind to gold. With all CPUs in one pset,
  `choose_processor()` returns the last processor if it is idle (`sched_prim.c:5080-5088`), or else the
  **lowest-numbered idle CPU** (`lsb_first`, `sched_prim.c:5242-5253`). Realtime placement uses the same lsb_first
  rule (`sched_prim.c:8306-8318`). Logical CPUs 0-3 are silver, so under a light UI load most wakeups land on
  silver while gold sits idle **(unverified: no per-cluster numbers yet; experiment E0 measures it)**.
- Gold matters more than "2x per core" suggests for this workload. The software renderer and the presenter are
  memcpy-bound, and memcpy runs at 1.0-1.1 GB/s on silver vs 7.9 GB/s on gold (boot 46). The presenter is a
  `kernel_task` thread (`SweetFramebuffer.cpp:516`) that copies 4608 x 2436 = 11.2 MB per frame (`:1015`): about
  11 ms on silver, 1.4 ms on gold.
- What userland already gives us is enough: QoS sets base priorities of UI 46, 47 for the frontmost app's UI
  threads, 37 user-initiated, 31 default, 20 utility, 4 background. Apple's per-thread-group P/E choices come from
  CLPC (Apple's closed performance controller), which we do not have. So a priority-based policy is the only
  signal we can use without CLPC.
- **Options, smallest first:**
  - **A, ADT only.** Not possible: `cluster-type` E/P with `MAX_CPU_CLUSTERS 1` hits the asserts at
    `machine_routines.c:1059/1063` in early boot. A gold-first cpu *order* is possible, but it needs a
    `sweet_perf.c` fix and only changes the idle choice.
  - **B, derecommend silver 1-3.** `sched_processor_enable()`, about 10 lines in `sweet_perf.c`, no scheduler
    code. An A/B test of "CPU0 + gold" against 8 cores, and a fallback.
  - **C, a narrow TAIMEN policy inside today's Clutch scheduler**, behind a `taimenamp=N` boot-arg. Gold first
    for threads at priority 21 and above and for RT threads; silver first for utility and background. Then
    migration on gold idle, then preemption preference. About 80 lines in `sched_prim.c` and `sched_clutch.c`.
  - **D, real AMP topology with the iOS-era AMP scheduler** (`sched_amp.c`). `MAX_CPU_CLUSTERS 2` and
    `__ARM_AMP__`, keep Edge off, ADT E/P. This is the configuration the iPhone X's own A11 kernel uses (an
    E-cluster boot CPU, clusters assigned by type, no `HAS_CLUSTER`). Without CLPC its default already sends user
    threads to P and kernel, utility and background work to E. The presenter must be soft-bound to P.
  - **E, Edge.** What our macOS-target build would select with `__ARM_AMP__`. Without CLPC every thread group
    prefers pset 0 (silver) and E->P migration is off, so gold would go **unused**. Not recommended.
- **Recommendation:** one probe patch first (per-cluster accounting, the touch counter fix, a synthetic swipe load,
  and B as a boot-arg). Its E0/E1 boots show how much gold placement can buy. Then C, levels 1-2, behind
  `taimenamp=`. D is the follow-up on a side branch if C stays below E1, or when `hw.nperflevels`/recount per-kind
  data are wanted. Skip E.

## 1. How xnu-8796 decides AMP, and what TAIMEN compiles (question 1)

### 1.1 Compile-time switches

| switch | where it comes from | TAIMEN today |
|---|---|---|
| `__ARM_AMP__` | `pexpert/pexpert/arm64/H13.h:70` only (A14/M1); `board_config.h:134-136` `#undef`s it when `MAX_CPU_CLUSTERS == 1` | undefined (`SWEET.h`/`TAIMEN.h` never define it; `board_config.h:100-112` sets `MAX_CPU_CLUSTERS 1`) |
| `__AMP__` | `osfmk/arm/smp.h:34`: `#define __AMP__ __ARM_AMP__` | 0 |
| `CONFIG_SCHED_CLUTCH` / `CONFIG_SCHED_EDGE` | `osfmk/arm64/proc_reg.h:85-97`: with `CONFIG_CLUTCH` (config option `config_clutch`, `MASTER.arm64.taimen:76`): non-AMP → Clutch; AMP **and** `XNU_TARGET_OS_OSX` → Clutch + Edge; AMP on iOS → neither | Clutch |
| `XNU_TARGET_OS_OSX` | `makedefs/MakeInc.def:76`, set when `PLATFORM=MacOSX`; dxb `build.sh:712, 732` builds with `SDKROOT=macosx` | **defined**, so turning on `__ARM_AMP__` alone would give Edge, not the iOS AMP scheduler |
| scheduler table | `osfmk/kern/sched_prim.h:810-834`: `__AMP__` → `sched_edge_dispatch` (Edge) or `sched_amp_dispatch`; else `sched_clutch_dispatch` or `sched_dualq_dispatch`. Compile-time macro `SCHED(f)`, no runtime choice | `sched_clutch_dispatch` (r51 symbols: clutch, dualq, multiq, traditional) |
| `MAX_PSETS` | `proc_reg.h:80` = `MAX_CPU_CLUSTERS` | 1 |
| `CONFIG_THREAD_GROUPS` | `proc_reg.h:102`, 1 on arm64 | 1 (thread groups exist, nobody recommends anything) |
| `HAS_CLUSTER` | `H13.h:39` only (per-cluster ACC registers, MPIDR-based cluster ids) | undefined |
| `sched_amp.c`, `sched_amp_common.c` | `osfmk/conf/files:159-160`, `optional config_sched_multiq` (in `SCHED_BASE`), bodies under `#if __AMP__` | compiled, empty |

`ml_get_cluster_type()` does not exist in xnu-8796. The accessors are `ml_get_boot_cluster_type()`
(`machine_routines.c:1201`), `ml_get_cluster_number()`, `ml_get_topology_info()`, `cpu_data->cpu_cluster_type`,
`pset->pset_cluster_type` and `ml_get_cpu_types()`.

### 1.2 Topology from the ADT (`osfmk/arm64/machine_routines.c`)

- `ml_parse_cpu_topology()` (944-1108) walks `/cpus` children in ADT order. `cpus=N` takes the first N
  (SWEET: 957-965). For each cpu it reads `reg` (MPIDR), `l2-*`/`l3-*`, and `cluster-type`: `'E'` → E, `'P'` → P,
  anything else → SMP (1034-1043). `ml_readprop` accepts 1-, 2-, 4- or 8-byte values (867-890), so u32 `'E'` works.
- Cluster id: without `HAS_CLUSTER`, `phys_cluster_id = (cluster_type == P)` (1050-1054). E and SMP cpus share
  cluster 0 and P cpus get cluster 1. MPIDR Aff1 is ignored. `assert(cpu->cluster_id < MAX_CPU_CLUSTERS)` (1059)
  and `assert(num_clusters < MAX_CPU_CLUSTERS)` (1063) are live (DEVELOPMENT has `mach_assert`). **An E/P ADT on
  today's kernel trips them** in `arm_init`, before any console.
- ADT `cluster-id` and `die-cluster-id` are descriptive (SWEET reads `die-cluster-id` with default 0, 1090-1097).
  No open-source code reads `cpu-cluster` nodes, `pmgr` `voltage-states*` or `nominal-performance*`: a grep of
  osfmk/bsd/iokit/pexpert finds none. Those are for the closed platform kexts and CLPC.
- `ml_processor_register()` (1369-1470) creates one pset per cluster id: with `__AMP__`, `PSET_AMP_E`/`PSET_AMP_P`,
  the non-boot type in `pset_node1` (1437-1441); otherwise `PSET_SMP` in `pset_node0` (1442-1445).
  `processor_bootstrap()` (`osfmk/kern/processor.c:189-234`) types pset 0 from the boot cluster.
- The only existing "deprioritise this core" knob is `l2-access-penalty`. A non-zero value makes the core an SMT
  secondary of CPU0 (`machine_routines.c:1455-1463`), and `processor_set_primary()` asserts a 2-way SMT pairing
  (`processor.c:326`), so at most one core could use it, with SMT semantics attached. Not usable.
- `hw.nperflevels` and `hw.perflevelN.*` are `popcount(ml_get_cpu_types())` (`bsd/kern/kern_mib.c:296-366`), so
  they follow the ADT types even without `__AMP__`. Today userland sees 1.

### 1.3 What Apple's own A11 tree says (`d22ap.adt`, iPhone X, the userland we run)

cpu0-3: `cluster-type 'E'`, `cluster-id 0`, `compatible apple,mistral`, cpu0 `state running`. cpu4-5:
`cluster-type 'P'`, `cluster-id 1`, `die-cluster-id 1`. `reg` 0x0-0x5 (Aff0 numbered across both clusters, Aff1
0). This is H10, the case the `!HAS_CLUSTER` branch was written for (`machine_routines.c:1124-1131` comment). The
boot CPU is an E core, as on taimen. On iOS (`XNU_TARGET_OS_OSX` unset) this config selects `sched_amp`
(`proc_reg.h:85-97`) **(unverified on a device: the open-source logic says so)**.

### 1.4 The schedulers we could get, without CLPC

| scheduler | how a thread picks a cluster | default without CLPC |
|---|---|---|
| Clutch, 1 pset (today) | none: one shared Clutch root, `choose_processor` (`sched_prim.c:5024-5465`) | last-idle hint, then lowest idle cpu id (silver), then lowest-priority running cpu with rotation (`last_chosen`, 5260-5295) |
| AMP (`sched_amp.c`) | `recommended_pset_type()` (`processor.c:1642-1697`): bound cluster > `base_pri <= 4` → E > `<= 20` → E (policy `SCHED_PERFCTL_POLICY_DEFAULT`, `sched_amp_common.c:89-90`) > thread-group recommendation; `CLUSTER_TYPE_SMP` (the initial value, `thread_group.c:278`) → **P** for user tasks, E for `kernel_task` | P-first for user threads. Spill to E when the P load average reaches cpus + `sched_amp_spill_count` (3; `sched_amp_common.c:68, 98-103, 215-248`); E cores steal from P (`sched_amp.c:489-530`); an idle P core pulls P-eligible threads back from E (`sched_amp_balance`, `sched_amp_common.c:300-346`); P threads on E are "avoided" (`sched_amp.c:638-653`) |
| Edge (`sched_clutch.c`, `CONFIG_SCHED_EDGE`) | per-QoS preferred cluster in each thread group's Clutch bucket groups, set by CLPC (`thread_group.c:1294-1360`) | preferred cluster = `pset0.pset_cluster_id` for every bucket (`sched_clutch.c:1139`) = **silver**. Default edges: E->P `migration_allowed 0, steal_allowed 0` (`sched_clutch.c:4870-4875`). Gold idle |

## 2. Options (question 2)

### 2.0 What userland already provides

- QoS → base priority (`osfmk/kern/thread_policy.c:83-89`): UI 46, IN 37, legacy/default 31, UT 20, BG and
  maintenance 4. RunningBoard's foreground role sets `tep_qos_ui_is_urgent` (`task_policy.c:831-833`), and UI
  threads of the frontmost app get 47 (`thread_policy.c:1094-1097`). Kernel threads run fixed at 80+
  (`BASEPRI_KERNEL` 81, `sched.h:151`); the presenter is one of them. Clutch buckets: `sched_clutch.c:322-342`.
- Thread groups: one per coalition. Nothing ever changes `tg_recommendation` from `CLUSTER_TYPE_SMP`: there is no
  CLPC, and `sched_perfcontrol_register_callbacks` (`osfmk/arm/machine_routines_common.c:173`) is never called.
- Work intervals: CoreAnimation and HID clients create typed ones (`bsd/sys/work_interval.h:166-176`: COREANIMATION/
  CA_RENDER_SERVER, CA_CLIENT, HID_DELIVERY). They exist for CLPC. The kernel only uses them for thread-group
  joins and an optional workload-config priority policy (`work_interval.c:689-725`). A later refinement of C could
  treat "joined a CA/HID work interval" as UI-critical.
- So priorities are the usable signal. Which priority and mode backboardd's render thread and SpringBoard's main
  thread actually run at is **unverified** (realtime? 46/47?). The thread census in 3.2 finds out.

### 2.A ADT only

1. `cluster-type` E/P on today's kernel: the asserts in 1.2, or out-of-bounds writes to
   `topology_info.clusters[1]` if asserts were off. Not possible without a kernel change (D).
2. Gold-first order (`0x0, 0x100-0x103, 0x1-0x3`; `mkadt.py:61-62`). CPU0 must stay first: `SweetCPU::startAll`
   panics unless the boot CPU is logical 0, MPIDR 0 (`SweetCPU.cpp:299-301`). Logical 1-4 become gold, so the idle
   search prefers CPU0, then gold. Costs and limits:
   - `taimen_gold()` hard-codes gold as logical 4-7 and requires 8 cpus (`sweet_perf.c:739-742, 815`), so it needs
     an MPIDR-based gold list (a small kernel change after all).
   - Every `cpuN` in the logs gets renumbered.
   - CPU_ON order changes: gold before silver 1-3 (every SMP boot so far started silver first).
   - The effect is class-blind (background work also goes gold-first) and limited to the idle search: the
     last-processor hint and the preemption rotation are unchanged.
   - Not recommended: C's level 1 does the same, gated, without renumbering.

### 2.B Recommendation tweak: derecommend silver 1-3 (experiment and fallback)

`sched_processor_enable(processor, FALSE)` (`sched_prim.c:7630-7667`) clears the CPU from the user-controlled
recommended mask. CPU0 cannot be disabled (7635-7638). The CLPC failsafe never re-enables cores here: it needs
`perfcontrol_requested_recommended_core_count < processor_count`, and that count stays `MAX_CPUS` without CLPC
(7363, 7463). Derecommended processors only run threads bound to them, such as the perf thread's per-CPU loop
**(unverified for that loop; it uses `thread_bind`)**.

```c
/* bsd/dev/sweet_perf.c, perf_thread(), TAIMEN, after taimen_clocks() (the gold ramp) */
int slow = 1;
if (PE_parse_boot_argn("taimenslow", &slow, sizeof(slow)) && slow == 0) {
	const ml_topology_info_t *t = ml_get_topology_info();
	for (unsigned c = 1; c < t->num_cpus; c++) {
		if ((t->cpus[c].phys_id & 0x100) == 0) {        /* silver: Aff1 0 */
			kern_return_t kr = sched_processor_enable(cpu_to_processor((int)c), FALSE);
			printf("TAIMEN SCHED: cpu%u mpidr %#x derecommended: %d\n", c, t->cpus[c].phys_id, kr);
		}
	}
}
```

Risk: low (an existing path that CLPC uses all the time). It runs on five cores, so it is a measurement and a
stop-gap, not the policy.

### 2.C Narrow TAIMEN policy in the Clutch scheduler (recommended)

All code is `#if defined(ARM64_BOARD_CONFIG_TAIMEN)`. A boot-arg sets the level; 0 runs the stock code paths
unchanged.

```c
/* osfmk/kern/sched_prim.c */
_Atomic uint64_t taimen_fast_map;   /* gold cpus while gold runs at full clock; 0 = no preference */
TUNABLE(int, taimen_amp_level, "taimenamp", 0);

static inline int
taimen_amp_class(int pri)           /* +1 wants gold, -1 wants silver, 0 either (gold if idle) */
{
	if (pri >= BASEPRI_USER_INITIATED) {    /* 37+: IN, UI 46, frontmost 47, kernel 80+, RT */
		return 1;
	}
	return pri <= BASEPRI_UTILITY ? -1 : 0; /* UT 20, BG/maintenance 4 */
}

static inline cpumap_t              /* never turns a non-empty map into an empty one */
taimen_amp_prefer(cpumap_t map, int pri)
{
	cpumap_t fast = os_atomic_load(&taimen_fast_map, relaxed);
	if (taimen_amp_level == 0 || fast == 0) {
		return map;
	}
	cpumap_t want = taimen_amp_class(pri) < 0 ? (map & ~fast) : (map & fast);
	return want ? want : map;
}
```

`taimen_fast_map` is set by `sweet_perf.c` through an exported `taimen_amp_set_fast(map)` once `taimen_gold()` has
finished its ramp. The map comes from the topology: cpus whose MPIDR Aff1 is 1. It is cleared in
`taimen_gold_park()`. The thermal park drops gold to GPLL0 (`sweet_perf.c:702-708, 877-907`); a parked gold core
is slower than silver, so the preference must turn off with it. Without `taimengold=` the map stays 0.

| level | hook | change |
|---|---|---|
| 1 placement | `choose_processor`, idle search (`sched_prim.c:5242-5253`) | `idle_primary_map = taimen_amp_prefer(idle_primary_map, thread->sched_pri)` before `lsb_first` |
| 1 | `choose_processor`, idle last-processor hint (5080-5088) | drop the hint (`processor = PROCESSOR_NULL; break;`) when it is not in `taimen_amp_prefer(idle & recommended, pri)`, so the search above runs. (`machine_choose_processor`, `machine_routines.c:1975-1978`, is the arch hook but gets no thread.) |
| 1 | `choose_processor_for_realtime_thread` (8306) | `primary_map = taimen_amp_prefer(primary_map, BASEPRI_RTQUEUES)` (every caller wants any available CPU) |
| 2 migration | `sched_clutch_thread_avoid_processor` (`sched_clutch.c:3205-3218`; checked in `thread_select`, `sched_prim.c:2589`, and `csw_check_locked`, 5816) | return true when the thread has class 1, is below RT, is unbound (`bound_processor == NULL`, else it would bounce back forever), is on a silver CPU, and an idle recommended gold CPU exists. It is then re-dispatched to that gold CPU |
| 2 | `.processor_balance` (`sched_clutch.c:2803`, today `sched_SMT_balance`, `sched_prim.c:2096-2146`) | `taimen_balance`: when a gold CPU goes idle, pick the silver CPU running the highest `current_pri` in 37..96 with `!current_is_bound` and send it one `SCHED_IPI_EVENT_REBALANCE` (modelled on `sched_amp_balance`); then call `sched_SMT_balance` (it only unlocks without SMT) |
| 3 preemption | active scan (5260-5295) and the choice at 5382 | also track the lowest-priority gold CPU (class 1) or silver CPU (class -1); prefer it when `thread->sched_pri` is above its `current_pri`. The preempted thread then finds an idle silver CPU or preempts something lower |

Counters per interval (3.2): idle picks by class and cluster, hint drops, bounces, rebalance IPIs, preference
preemptions.

Risk: medium-low. These are hot paths under the pset lock, but every change narrows a map that already exists,
and level 0 is today's code. The failure modes are wrong placement or migration ping-pong (contained by the
"idle gold exists" test and one IPI per gold-idle event), not a NULL processor. Clutch thread-group fairness is
untouched.

### 2.D AMP topology and the iOS AMP scheduler (follow-up, side branch)

| file:line | change |
|---|---|
| `pexpert/pexpert/arm64/board_config.h:100-112` | TAIMEN: `MAX_CPU_CLUSTERS 2`, `__ARM_AMP__ 1` (SWEET keeps 1 cluster) |
| `osfmk/arm64/proc_reg.h:89` | `#if XNU_TARGET_OS_OSX && !defined(ARM64_BOARD_CONFIG_TAIMEN)`: no Edge, no `CONFIG_SCHED_CLUTCH`, so `sched_amp_dispatch` (`sched_prim.h:814-822`) |
| `tools/mkadt.py:200-201` (new `--taimen-amp`) | `cluster-id u32(cluster)`, `cluster-type u32(ord('E' if cluster == 0 else 'P'))`; `l2-cache-id` is already 0/1 (204) |
| `tools/taimen/boot-xnu.sh:29` | pass `--taimen-amp` when the kernel has `_sched_amp_dispatch` (`nm`/`strings`), so ADT and kernel cannot mismatch |
| `iokit/Kernel/arm/SweetFramebuffer.cpp:516` | after `kernel_thread_start`: `thread_bind_cluster_type(pt, 'P', true)` (soft; `sched_prim.c:8664-8706`). Otherwise the `kernel_task` → E rule copies every frame on silver |
| `iokit/Kernel/arm/SweetCPU.cpp:357-362` | log `sched_string`, the psets with their types, `popcount(ml_get_cpu_types())`; panic if this is an AMP kernel and the ADT has no E/P (one SMP cluster makes pset 0 `PSET_AMP_P` with `ecore_set` NULL, `processor.c:213-220`, `sched_amp.c:218-230`) |

What comes free with D:
- `hw.nperflevels` 2, as on a real iPhone X.
- Recount keeps per-kind thread and task time (`recount.c:100-118`, `recount_task_usage_perf_only`, 675-682).
- `vm_compressor` threads soft-bound to E (`vm_pageout.c:4577-4585`).
- Test knobs: `enable_skstsct=2` forces everything onto E as a negative control (`sched_prim.c:473-475`,
  `processor.c:1661`), and `kern.sched_thread_bind_cluster_type` / `kern.sched_amp_*` sysctls.
- `sched_amp_cpu_init_completed` skips its topology asserts whenever `cpus=` is on the command line
  (`sched_amp.c:722-725`), as it always is.

Risks:
- It swaps the whole scheduler: Clutch's thread-group fairness is replaced by priority runqueues (as on iOS AMP
  devices, **unverified**).
- `SWEET` + `__AMP__` has never been compiled.
- Every kernel thread lands on silver except the ones we bind: the presenter, and later the GPU driver threads.
- `hw.nperflevels` 2 may change how userland sizes its thread pools **(unverified)**.
- `cpus=` below 5 leaves no P pset, an untested `sched_amp` path. Keep `cpus=8`.
- Two-SMP-pset variant (`MAX_CPU_CLUSTERS 2` without `__ARM_AMP__`): no gain. `choose_starting_pset` takes
  `lsb_first(pset_idle_map)` = pset 0, silver (`sched_prim.c:5586-5593`), so it is only a topology rung.

### 2.E Edge (not recommended)

Needs everything in D minus the `proc_reg.h` change, plus a CLPC stand-in:
- preferred clusters per bucket at thread-group creation (`sched_clutch.c:1139`, or
  `sched_edge_tg_preferred_cluster_change`, 4758-4804): FIXPRI/FG/IN/DF → P, UT/BG → E;
- E->P edges with migration and steal allowed (`sched_edge_cpu_init_completed`, 4846-4889).

Largest change, and Edge's spill and steal heuristics are tuned for CLPC's edge matrix.

### 2.F Summary

| option | kernel files | size | scheduler changed | boot risk | effect |
|---|---|---|---|---|---|
| A2 reorder | mkadt, sweet_perf | ~15 lines | no | low | idle choice only, class-blind |
| B derecommend | sweet_perf | ~10 | no (existing API) | low | 5 cores, UI on gold; upper bound |
| C policy | sched_prim, sched_clutch, sweet_perf | ~80 | placement hooks, gated | low-medium | gold for 37+, silver for <=20 |
| D AMP | board_config, proc_reg, mkadt, SweetFramebuffer, SweetCPU | ~30 + ADT | whole scheduler | medium | Apple's P-first default |
| E Edge | D + clutch/TG defaults | ~100 | whole scheduler + new policy | high | depends on the stand-in |

## 3. Measuring it from the kernel log (question 3)

### 3.1 What the probe prints now (`bsd/dev/sweet_perf.c:953-1028`, every 10 s with `sweetperf=1`)

- `SWEET PERF: MHz a/b/c/d/e/f/g/h  swaps X/s  presents Y/s  present avg Z us`: per-CPU clock from a bound loop
  (989-997), swap and present rates from SweetFramebuffer.
- `SWEET PERF: swap gaps <20 a <37 b <54 c more d ms, touch moves N`.
- `SWEET PERF: panel kicks ...`, then `SWEET TOP: all N% | proc[pid] p% ...` (per-process ledger CPU time, 387-472).

Gaps:
- Nothing per cluster.
- `present avg` mixes copy speed with `sweet_fbcon_wait_idle()`, which runs before the copy whenever
  `taimenhwscale != 3` (`SweetFramebuffer.cpp:1009-1011`). Boot 47 shows 1176 us next to 6423-23760 us; boot 51
  under use 12-18 ms.
- `touch moves` is always 0 on taimen. It prints `sweet_touch_moves` (`sweet_perf.c:58, 1013-1015`), which only
  `sweet_touch.c` increments, and TAIMEN starts `taimen_touch.c` instead (`sweet_ufs.c:674`; its counter is
  `taimen_touch_moves`, `taimen_touch.c:546, 567`). Boot 51 logged 30 `TAIMEN TOUCH: down` and every perf line said
  `touch moves 0`.
- Unattended boots have no UI load: 0-1.5 swaps/s in boot 47.

### 3.2 Per-cluster accounting worth adding (one probe patch, policy-independent)

| line (new) | source | cost |
|---|---|---|
| `SWEET CPU: busy % 41/22/19/17 \| 63/40/12/3` (silver \| gold, per interval) | `recount_processor_usage(&processor_array[i]->pr_recount, &u, &idle_mach)` (`recount.h:300-305`) → idle delta per CPU; small osfmk helper, BSD cannot see `processor_t` | nothing |
| `SWEET TOP: ... backboardd[57] 50% g34 ...` (`gN` = share of that process's interval time spent on gold) | recount already keeps **per-task time per CPU** on this non-AMP kernel: `recount_task_plan` is `RCT_TOPO_CPU` (`recount.c:123`) and every context switch adds to the task's per-CPU track (`recount_switch_thread`, 778-793). Helper next to `get_task_cpu_time`: sum `task->tk_recount.rtk_lifetime[i].rt_usage.ru_user_time_mach + ru_system_time_mach` over the gold cpus. Print the ledger and the recount total once to check they agree **(unverified that they match)** | nothing new on the switch path |
| `SWEET PERF: present copy avg X us, on gold n of m` | time only the memcpy (`SweetFramebuffer.cpp:1013-1019`) and record `cpu_number()` | two counters |
| `TAIMEN AMP: idle picks fast g/s ... slow g/s ..., hint drops, bounces, rebal, preempt g` | C's counters (or D: spill, steal and rebalance counts) | one increment per decision |
| `TAIMEN THREADS: backboardd[57] tid ... "name" base B pri P mode M bucket K cpu C ms T` (boot-arg `taimenthreads=1`, every 60 s, for the 3 busiest processes of the last SWEET TOP) | `task_lock`, iterate `task->threads`; `base_pri`, `sched_pri`, `sched_mode`, `th_sched_bucket`, `last_processor->cpu_id`, `recount_thread_time_mach` | once a minute |
| fix: `touch moves` from `taimen_touch_moves` on TAIMEN | `sweet_perf.c:1013-1015` | none |
| optional: `SWEET PERF: touch->swap <33 a <66 b <100 c more d ms` | timestamp touch downs and moves in `SweetHID::touch`; the next `AppleCLCD::swap` (853-871) records the delta. The closest thing to "laggy" | two stores |
| load: `TAIMEN SWIPE: from T s, every 2 s` (boot-arg `taimenswipe=T`) | a kernel thread feeding `sweet_hid_touch()` (`SweetHID.cpp:612-618`): finger down at panel (1100, 1600), 10 moves of 76 px 30 ms apart, up; alternate directions. Panel space is 1440x2880 (`SweetHID.cpp:363-364`; the canvas mapping is at 453-500). Whether SpringBoard takes it as a page swipe is **unverified**. `sweethidtest=N` (Home every N s, `SweetHID.cpp:16, 398`) is the existing fallback | none |

Also print once, after `SWEET CPU: 8/8 CPUs initialized` (`SweetCPU.cpp:361`):
`TAIMEN SCHED: clutch, psets 1, silver cpu0-3 (0x0-0x3), gold cpu4-7 (0x100-0x103), taimenamp=N`, from
`sched_string` and the topology. Then `sweet_perf.c` prints `TAIMEN AMP: fast map 0xf0 (gold at 1728 MHz)` or
`TAIMEN AMP: fast map 0 (gold parked: hot)`.

## 4. Experiment ladder

All boots from `port/` with the ROOT=1 defaults (`boot-xnu.sh:25`: `cpus=8 taimengold=1728 sweetkeys=1
taimenhwscale=2 sweetperf=1 taimenddr=1804 ...`); extra args go last and win. Template:

```
ROOT=1 ROOTFIX=../research/taimen/rootfix.bin tools/taimen/autoboot.sh ../out/kernels/taimen-rN/kernel.development.taimen ../out/taimen-xnu-rN-ampE0 taimenswipe=90 taimenthreads=1 taimenreboot=300
```

### 4.1 Steps

| step | kernel | extra boot-args | question | lines to expect |
|---|---|---|---|---|
| E0 | probe patch (3.2), C compiled in but level 0 | `taimenswipe=90 taimenthreads=1 taimenreboot=300` | baseline under a fixed load: where do UI threads and the presenter run? | `TAIMEN SCHED: clutch, psets 1, ... taimenamp=0`; `TAIMEN AMP: fast map 0xf0 (gold at 1728 MHz)`; `SWEET CPU: busy % .../...`; `SWEET TOP: ... backboardd[57] N% gM`; `SWEET PERF: present copy avg X us, on gold n of m`; `TAIMEN THREADS: ...`; touch moves > 0 while swiping |
| E1 | same | `taimenslow=0 taimenswipe=90 taimenreboot=300` | upper bound for placement: CPU0 + 4 gold | `TAIMEN SCHED: cpu1 mpidr 0x1 derecommended: 0` (x3); busy % of cpu1-3 near 0; `g` >= 90 for backboardd, SpringBoard, kernel_task; present copy avg near 1.4 ms when on gold |
| E2 | same | `taimenamp=1 taimenswipe=90 taimenreboot=300` | placement only | `TAIMEN AMP: idle picks fast g/s N/M ...` with N >> M; gold busy % up, silver down |
| E3 | same | `taimenamp=2 ...` | + migration | `... bounces B rebal R` with B and R in tens per interval, not thousands |
| E4 | same | `taimenamp=3 ...` | + preemption preference | `... preempt g P` |
| E5 (optional) | D kernel from a side branch (`workflow_dispatch` on that branch) | `taimenswipe=90 taimenreboot=180`, then the same with `enable_skstsct=2` (E only, control) | Apple's AMP scheduler | `TAIMEN SCHED: amp, pset0 E cpu0-3, pset1 P cpu4-7, nperflevels 2`; `SWEET FB: presenter soft-bound to P` (new); `g` from recount per-kind data |

Compare each step with E0 over the same window (from `taimenswipe` start to the deadline):
- mean swaps/s and the `<20 ms` share of swap gaps;
- present copy average;
- the `g` shares of backboardd, SpringBoard and kernel_task (the presenter);
- `all%` and the first `display on` time (boot 46: about 20 s);
- no new panics.

### 4.2 Reading the results

| result | meaning | next |
|---|---|---|
| E0: UI processes mostly on silver (`g` < 50) | the lsb_first bias is real | E1 |
| E1 no better than E0 | the UI is not placement-bound (present waits, single-thread software render, or the GPU path) | stop here, put the effort into the GPU |
| E1 better, E2/E3 reach most of E1 with 8 cores | C works | make `taimenamp=2` the ROOT=1 default in `boot-xnu.sh:25` |
| E2/E3 well short of E1 | the shared runqueue lets silver dequeue UI threads first | E4, then E5 |
| bounces or rebal in the thousands per interval | ping-pong | tighten the avoid test (require that the gold CPU stays idle for >= 1 tick) |
| background-heavy start (first ~200 s, `all` 300-580 %, boot 41) slower than E0 | the class thresholds starve the middle class | move class 0 to silver-first, or lower the class 1 bound to 31 |
| gold `TSENS` park during a run | `fast map 0` line; the policy turns itself off | that is the intended behaviour |

## 5. Risks and containment

| risk | effect | containment |
|---|---|---|
| E/P ADT on a `MAX_CPU_CLUSTERS 1` kernel | assert in `ml_parse_cpu_topology` before the console: hang or reset with no log | the ADT flag is set only by `boot-xnu.sh` detecting `_sched_amp_dispatch`; the default ADT is unchanged |
| AMP kernel with the plain ADT | single SMP cluster typed P, `ecore_set` NULL | SweetCPU panics with a message (2.D) |
| bug in a C hook | misplacement; the hooks only narrow existing maps | `taimenamp=0` restores stock behaviour in the same kernel |
| avoid/balance ping-pong, IPI storms | CPU burn, jank | avoid only with an idle recommended gold CPU; unbound threads only; one IPI per gold-idle event; counters |
| gold parked to GPLL0 (hot) | gold slower than silver | `taimen_fast_map` cleared in `taimen_gold_park()` |
| gold busy with low-priority work | UI waits | class -1 goes silver-first; level 3 |
| E1 runs on five cores | slower background phases | experiment only |
| D: never-built `SWEET` + `__AMP__` | compile errors, untested asserts | side branch, `cpus=8`, short `taimenreboot` |
| D: kernel threads pinned to E | presenter about 11 ms per frame instead of about 1.4 | soft-bind the presenter to P |
| D: Clutch replaced | fairness between thread groups lost | compare SWEET TOP shares with E0 |
| CI | a push touching `xnu/patches` cancels a running build (memory notes) | coordinate patch pushes with the GPU work in the main session |

## 6. Open questions

- Do iOS 16 kernels on AMP iPhones (A11 included) run the `amp` scheduler? The open-source config logic says yes
  (`proc_reg.h:85-97`, iOS `SCHED_BASE` includes `config_clutch`, `MASTER.arm64.iPhoneOS:72`). Not confirmed on a
  device; it decides how "native" D is for this userland.
- Which backboardd and SpringBoard threads gate a frame, and at what priority and mode (realtime? 46/47? fixed
  79?). The E0 census answers this and may move the class 1 bound.
- Why silver memcpy is 7x slower than gold (1.0-1.1 vs 7.9 GB/s, boot 46) when the clocks differ by 10 %:
  silver L2, prefetch or bus setup? If the clocks plan can fix silver's memory path, the payoff of this plan for
  memcpy-bound work shrinks.
- Whether the UI is placement-bound at all (E1).
- What `hw.nperflevels` 2 changes in iOS userland (D).
- Whether the synthetic swipe registers as a SpringBoard page swipe (otherwise use `sweethidtest=`).
- Cosmetic: the ADT claims 1900.8 / 2457.6 MHz (`mkadt.py:61-62`); the measured clocks are 1555 / 1728 MHz
  (boot 46). Only `hw.cpufrequency`-style reporting reads it, never the scheduler.
