# Plan: raising the gold CPU cluster and DDR clocks on taimen (2026-10-23)

Plan only: nothing was built, booted or committed. Sources read: `research/taimen/live.dts` (line numbers below
refer to it), `scratch/xnu` at `fb73534` (0223: `bsd/dev/sweet_perf.c`, `bsd/dev/sweet_rpm.c`), the boot-41 logs in
`out/taimen-xnu-r44-perf/` (XNU `sweetlog.txt` and the log reader's Linux `reader/dmesg.txt`), and the Pixel 2 kernel.
For the kernel I read the LineageOS mirror of `android-msm-wahoo-4.4`
(https://github.com/LineageOS/android_kernel_google_wahoo/tree/lineage-19.1; that it matches
android.googlesource.com `kernel/msm` `android-msm-wahoo-4.4` line for line is **unverified**). Kernel file names below
are relative to it: `drivers/clk/msm/clock-osm.c`, `clock-gcc-8998.c`, `clock-rpm.c`, `clock-local2.c`,
`drivers/regulator/cpr3-regulator.c`, `cpr3-util.c`, `cprh-kbss-regulator.c`, `qpnp-regulator.c`,
`drivers/soc/qcom/msm-spm.c`, `scm.c`, `drivers/thermal/msm-tsens.c`, `drivers/spmi/spmi-pmic-arb.c`,
`include/dt-bindings/clock/msm-clocks-hwio-8998.h`, `arch/arm/boot/dts/qcom/msm8998.dtsi` and `msm8998-bus.dtsi`.
Also https://github.com/linux-msm/debugcc/blob/master/platforms/msm8998.c and AngeloGioacchino Del Regno's
OSM and CPR3 series (links in 1.5 and 3.4). Statements marked **(unverified)** are inferences that no log or
source in hand confirms. GPU clocks are left to `taimen-gpu-plan.md`.

## 0. Short version

- On MSM8998 each CPU cluster has an **OSM** block (Operating State Manager; silver 0x179C0000, gold 0x179C2000).
  Linux writes its 40-row lookup table (LUT) once and afterwards only writes a row index to
  `DCVS_PERF_STATE_DESIRED` (+0x1F10). The OSM's sequencer (microcode loaded by TZ on v2 silicon) then sets the
  cluster PLL's L value, switches the cluster clock mux (GFMUX), switches the APM (array power mux) and the
  MEM-ACC settings, and asks the **CPRh** controller (silver 0x179C8000, gold 0x179C4000) for the voltage of that
  row's virtual corner. CPRh sends that voltage through the cluster's L2 SAW4 to PM8998 **S10** (silver) or **S13**
  (gold), in mV. The bootloader programs neither OSM nor CPRh. Linux does it in `cpu_clock_osm_driver_probe`
  (arch_initcall) and then sets 1555.2 MHz silver and 1728 MHz gold.
- What we measured fits "no OSM". Silver runs from its PLL at L=81 = 1555.2 MHz, which is also Linux's v2 pwrcl
  boot rate. Gold runs at 300 MHz from GCC `hmss_gpll0` (GPLL0 600 MHz / 2), the source of LUT row 0, through
  the gold GFMUX (cfg 0x17811054, expected SRC_SEL 4). If the gold PLL ran at LUT row 0's L=15 it would show
  288 MHz, not 299. Section 4's read-only dump will confirm this (**unverified** until C0 runs).
- This phone's fuses are in the reader's dmesg: speed bin 2, CPR fusing revision 3, so fuse combo 19. OSM uses
  pwrcl bin 0 (fmax 1900.8 MHz) and perfcl bin 2 (2361.6 MHz on all cores, 2457.6 MHz on one core). From the
  fuses and the DT adjustments, the per-corner open-loop voltages are (section 2): gold 1728 MHz needs 796 mV,
  2112 MHz 956 mV, 2361.6 MHz 1060 mV; silver 1555.2 MHz needs 796 mV and 1900.8 MHz 928 mV.
- The rail voltages that XBL leaves behind are unknown. The only read-only sources without porting SPMI are
  SAW4 `PMIC_STS` (gold 0x17812C18, silver 0x17912C18), and only if the SAW ever sent a value.
- **Minimal safe step (G1).** Leave the voltage, the APM and MEM-ACC untouched. Program the gold PLL and switch
  the gold GFMUX from GPLL0 to the PLL. Pick the highest LUT frequency that meets three conditions: its Linux
  open-loop voltage + 16 mV is at or below the measured gold rail voltage; its MEM-ACC level equals the current
  level; it is at most **1728 MHz**, below the APM (800 mV) and MEM-ACC (852 mV) thresholds. If the gold rail sits
  at its NOM open-loop voltage (about 800 mV), that gives 1651-1728 MHz, 5.5-5.8 times today's clock. If the
  voltage cannot be read, G1 does nothing.
- Caveat for G1: Linux on 8998 v2 never writes the cluster PLL or GFMUX registers itself; only the OSM sequencer
  does. Whether EL1 may write them under QHEE is therefore unknown. A blocked write may reset the phone, which
  is bounded.
- Silver above 1555.2 MHz and gold above 1728 MHz need a higher voltage, the APM switched to the APC rail, and
  MEM-ACC level 3. The safe way to get there is Linux's: CPRh, then OSM, with 7 TZ io-write SCM calls. This phone's
  own Linux shows that path works from EL1 under QHEE (reader `dmesg.txt:336` "OSM driver inited"). That is stage
  A1: a port, not a register poke.
- **DDR.** The RPM resource is "clk2" id 0 (bimc), key "KHz ", the same as on 8996. A vote takes effect only once
  RPM clock scaling is on (clk0 id 2 "Enab"); Linux 8998 turns it on in its GCC probe after a maximum bimc vote.
  `sweet_rpm_clk` works over the TAIMEN GLINK transport: it uses the same `rpm_request` as the touch LDO votes
  that boot 41 acked. `sweet_rpm_clk_scaling`'s handoff table is 8996's, though, and must be replaced. Linux's
  default AP vote is the maximum: cpubw governor "performance", 13763 MB/s = 1804 MHz. Verify through the GCC
  debug mux: CTL 0x162000, selector 0x14e (downstream "bimc_clk") or 0xa9 (debugcc "bimc_clk"), XO/4 branch
  0x143008, counter at 0x162004/0x162008. How the measured value relates to the DDR clock is **unverified**.
- Ladder: C0 read-only dump, C1 debug-mux measurements, V1 voltage readback over SPMI (probably needed), D1 DDR
  vote, G1 gold ramp at constant voltage, S1 silver (only if the voltage allows), A1 OSM + CPRh.

## 1. Who controls the CPU clock on MSM8998 (question 1)

### 1.1 Blocks and addresses (live DT)

| block | address | DT source | role |
|---|---|---|---|
| OSM, silver / gold | 0x179C0000 / +0x2000 (`OSM_CORE_TABLE_SIZE` 8192) | `qcom,cpu-clock-8998@179c0000` reg[0] = <0x179C0000 0x4000>, `live.dts:2455` | LUT, DCVS index, FSMs, sequencer |
| cluster PLL, silver / gold | 0x17916000 / 0x17816000 | reg[1], reg[2]; `qcom,l-val-base = <0x17916004 0x17816004>` (2478), `qcom,apcs-pll-user-ctl = <0x1791600C 0x1781600C>` (2480) | the cluster clock above 825.6 MHz |
| GFMUX (cluster clock RCG) | cmd 0x17911050 / 0x17811050, cfg 0x17911054 / 0x17811054 | `qcom,apcs-cmd-rcgr`, `qcom,apcs-cfg-rcgr` (2481-2482) | selects GPLL0 (SRC_SEL 4), PLL main (3), PLL early (5) |
| APCS common | 0x179D1000 | reg[3] "apcs_common" | OSM clock RCG at +0x12C, MEM-ACC at +0x360..+0x370, ITM at +0x43C |
| ACD, silver / gold | 0x17914800 / 0x17814800 | reg[4], reg[5] | adaptive clock distribution (optional) |
| speed-bin fuse | 0x784130 | reg[6] "perfcl_efuse" | perfcl bin = bits [31:29] |
| CPU debug mux | 0x1791101C | reg[7] "debug" | [9:8]: 0 pwrcl, 1 perfcl, into the GCC debug mux (selector 0xC0) |
| APM | mode 0x179D0004 / 0x179D0010, status 0x179D000C / 0x179D0018 | `qcom,apm-mode-ctl`, `qcom,apm-ctrl-status`, threshold `<0xC3500>` = 800 mV (2483-2485) | memory arrays on VDD_MX (status 0) or VDD_APC (status 3) |
| MEM-ACC | silver cfg 0x179D1360, 0x179D1364 (twice); gold 0x179D1368, 0x179D136C, 0x179D1370 | `qcom,pwrcl-apcs-mem-acc-cfg`, `qcom,perfcl-apcs-mem-acc-cfg`, values (2492-2495), threshold 0xD0020 = 852 mV (2505-2506) | SRAM assist settings per voltage level 1/2/3 |
| CPRh apc0 (silver) / apc1 (gold) | 0x179C8000 / 0x179C4000, fuses 0x784000 | `cprh-ctrl@179c8000` (4790), `cprh-ctrl@179c4000` (4862); clock `gcc_hmss_rbcpr_clk`; `qcom,voltage-base = <0x55F00>` (360 mV), step 0xFA0 (4 mV), `qcom,cpr-saw-use-unit-mV` (4814-4815, 4886-4887) | closed-loop voltage per virtual corner |
| L2 SAW4 silver / gold | 0x17912000 / 0x17812000 | `qcom,spm@179120000` (5020), `qcom,spm@178120000` (5005); `saw2-avs-limit` 0x4200420 / 0x4700470 | carries CPRh's voltage to the PMIC |
| rails | PM8998 SID 1: S10 0x2F00 (568-1056 mV), S13 0x3800 (568-1136 mV) | `pm8998_s10`, `pm8998_s13` (1228-1247), the CPRh `vdd-supply` | VDD_APC0, VDD_APC1 |
| GCC | 0x100000 | `qcom,gcc@100000` (2408) | `hmss_gpll0` RCG 0x14805C (300 MHz, the GFMUX GPLL0 input), rbcpr CBCR 0x148008 |

The SAW AVS limits 0x420 = 1056 and 0x470 = 1136 are the S10 and S13 maximums in mV. That supports the reading
that SAW4 data on this SoC is in mV with an 11-bit or wider field. It is **unverified**: `msm-spm.c:230-235` masks
`PMIC_STS` with 0x300FF, an 8-bit data field, which belongs to older SAWs.

### 1.2 The LUT (DT `qcom,pwrcl-speedbin0-v0`, `qcom,perfcl-speedbin2-v0`, `live.dts:2461, 2508`)

Each row has five cells: frequency, freq_data, PLL override, spare (the MEM-ACC level), virtual corner (1-based)
(`clock-osm.c:53-60, 862-955`). The freq_data fields are [27:26] source (0 = GPLL0 300 MHz, 1 = PLL),
[25:24] PLL divider, [18:16] core count (4 = all cores, 1 = one core), [7:0] L (×19.2 MHz) (`clock-osm.c:840-843`).
Rows with L below 43 (`PLL_MIN_LVAL`) use divider 1. Decoded:

| cluster | rows | examples (MHz: L, MEM-ACC level, VC) |
|---|---|---|
| silver, bin 0 (22 rows) | 300 (GPLL0) ... 1900.8 | 1555.2: L 81, level 2, VC 18 (idx 17); 1670.4: L 87, 2, 19; 1824.0: L 95, 3, 21; 1900.8: L 99, 3, 22 (idx 21) |
| gold, bin 2 (35 rows) | 300 (GPLL0) ... 2361.6 (4 cores), 2457.6 (1 core) | 902.4: L 47, level 1, VC 9 (idx 8); 1132.8: L 59, 1, 12; 1190.4: L 62, 2, 13; 1728.0: L 90, 2, 20 (idx 19); 2035.2: L 106, 3, 24; 2361.6: L 123, 3, 30 (idx 33) |

Speed bins: the pwrcl efuse mask is 0, so pwrcl always uses bin 0. Perfcl uses 0x784130 bits [31:29], which is 2
on this phone ("using perfcl speed bin 2", reader `dmesg.txt:319`; `clock-osm.c:3188-3220`).

### 1.3 What the Linux OSM driver does (`cpu_clock_osm_driver_probe`, `clock-osm.c:3152-3436`)

B is the cluster's OSM base (0x179C0000 or 0x179C2000). `count(ns)` is ns × 200 MHz (`qcom,osm-clk-rate`).

1. Map the resources. Get the CPRh corner regulators `vdd-pwrcl` and `vdd-perfcl` (probe defers until CPRh is up).
   On v2, refuse `secure_init`: TZ must already own the sequencer (`clock-osm.c:3182-3186`).
2. Read the speed bin and parse the LUT. For each row, take the open-loop voltage of its virtual corner from CPRh
   (`regulator_list_corner_voltage`, `clock-osm.c:1582-1596`).
3. Compute the crossover corners (`clock-osm.c:1598-1708`). CPRh has two extra corners after the n functional
   ones: the APM crossover (count-2) and the MEM-ACC crossover (count-1). The APM threshold VC is the first VC
   with open-loop ≥ 800 mV; the MEM-ACC threshold VC is the first with open-loop ≥ 852 mV.
4. `B+0x1F00` (cycle counter) = 1 | ((200/19.2 - 1) << 1) = 0x13.
5. LUT, rows i = 0..39, at B+0x1150+32·i (`clock-osm.c:1536-1580`): INDEX (+0) = i, FREQ (+4) = freq_data,
   VOLT (+8) = (VC-1) << 16 | open-loop mV, OVERRIDE (+0xC) = override, SPARE (+0x14) = MEM-ACC level. Rows past
   the end repeat the last one.
6. Policy (`clock-osm.c:1710-1969`): +0x101C |= count(1000) << 16 | count(1000) (0xC8 each); +0x100C = 1
   (`pc-override-index` 0 + enable); +0x1024 = 0 (`set-ret-inactive`); +0x1020 = 0; +0x102C and +0x1030 |=
   0xFFFF << 16 | 0xFFFF (`llm-*-timer` 327675 ns); +0x1034 |= 0 (LLM frequency and voltage votes honoured).
7. FSMs (`clock-osm.c:2211-2346`): +0x1070 (PDN_FSM_CTRL) |= bits 0 (CC boost), 2 (safe), 1 (PS), 4 (WFx),
   3 (PC), 5 (DCVS droop). Boost timers +0x1074/78/7C, +0x1084/88/8C, +0x1094/98/9C |= 2000 / 1000<<16 etc.
   +0x10A0 = 5. Droop: +0x10AC |= 100 << 16 | 100, +0x10B0 |= 50 << 16 | 50, +0x10BC = 1, +0x10A8 = 1,
   +0x10B8 = 100, +0x10A4 |= 0x80020008. +0x10C0 |= 1.
8. MEM-ACC and APM sequencer values, **through TZ** with `scm_io_write(PA, val)` (`clock-osm.c:2041-2091,
   2414-2432`): SEQ_REG(n) = B+0x300+4n. n=88 gets the MEM-ACC crossover VC; n=55..58 the MEM-ACC thresholds;
   n=32 the L of the first level-3 VC; n=72 the APM crossover VC; n=15 the APM threshold VC; n=31 the threshold
   VC - 1; n=76 = 0x39 | threshold << 6. Also a plain write B+0x1048 (`SEQ_REG1_MSM8998_V2`) = the APM threshold
   VC. The SCM call (`scm.c:1091-1107`, `scm.h:48`) is x0 = 0xC2000502 (SiP, SMC64, atomic, svc 5 IO, cmd 2
   write), x1 = 2 (SCM_ARGS(2)), x2 = PA, x3 = value.
9. Not done on v2: the "OSM WAS" workarounds (returns early, `clock-osm.c:2133-2134`), the cluster PLL setup
   (only with `qcom,osm-pll-setup`, absent from the live DT) and the sequencer upload (`secure_init` only). So on
   v2 Linux never writes the PLL or the GFMUX.
10. ACD (`clock-osm.c:3074-3147`): ACDTD 0x9611, ACDCR 0x2B5FFD, ACDSSCR 0x501, EXTINT 0x2CF9AE8, AUTOXFER_CTL 0x15,
    auto-transfer, GFMUX_CFG = 1, DCVS_SW 1 then 0, EXTINT 0x2CF9AFE (values from `live.dts:2499-2504`).
11. Set `hmss_gpll0` (GCC RCG 0x14805C) to 300 MHz and the OSM clock (`APCS_COMMON_LMH_CMD_RCGR`, 0x179D112C) to
    200 MHz. Write index 0 to +0x1F10 for both clusters. Enable: B+0x1004 = 1, then wait 5 µs. Write the boot
    rates' indices to +0x1F10: pwrcl 17 (1555.2), perfcl 19 (1728.0) (`clock-osm.c:3357-3417`).
12. After that, cpufreq writes only +0x1F10 (`clk_osm_set_rate`, `clock-osm.c:679-723`).

### 1.4 What CPRh needs (`cpr3-regulator.c:1291-1600`, `cprh-kbss-regulator.c`)

The CPRh clock `gcc_hmss_rbcpr_clk` (CBCR 0x148008, RCG 0x148044, 19.2 MHz) stays on. Linux reads the fuses and
computes per corner: open-loop voltage (section 2), floor, target quotient and RO select. It writes
`CPRH_REG_CORNER(i)` (+0x3A00+4i): [7:0] open-loop steps above 360 mV, [15:8] floor steps, [24:16] quotient delta,
[28:25] RO, bit 29 closed loop disabled. Then GCNT, timers, CPR_CTL (+0x4), STEP_QUOT, sensor owners, thread
thresholds, and MARGIN_ADJ_CTL (+0x7F8: HW closed loop, PMIC step 4). Then `CPRH_REG_CTL` (+0x3AA0): base 90
steps, multiplier 4, mode-switch delay, **bit 0 OSM_ENABLED**. Last, loop enable. `CPRH_REG_STATUS` (+0x3AA4)
holds the current corner in [5:0] and the last voltage in [17:6]. Reader dmesg lines 275 and 287: "apc0/apc1:
Default CPR mode = full HW closed-loop".

### 1.5 State at hand-over

| item | expected | evidence |
|---|---|---|
| OSM | not enabled; LUT empty | "on MSM8998, SDM630/660 ... the bootloader will *not* initialize the OSM (and the CPRh ...)" (https://lkml.iu.edu/hypermail/linux/kernel/2107.0/00223.html, v6 6/9 of "cpufreq-qcom-hw: Implement full OSM programming"); Linux programs every LUT row and enables OSM itself (1.3). **unverified** on this phone until C0 |
| OSM sequencer | loaded by TZ | v2 refuses `secure_init` and writes sequencer registers only through SCM. Not readable from EL1 (do not try) |
| CPRh | not programmed, OSM interface off | same series; Linux writes every CPRh register |
| silver | PLL at L=81, GFMUX on the PLL (SRC_SEL 5 or 3) | 1555 MHz measured on CPUs 0-3 (boot 41, `sweetlog.txt:184-306`); 1555.2 = 81 × 19.2 = `MSM8998V2_PWRCL_BOOT_RATE` (`clock-osm.c:231`) |
| gold | GFMUX SRC_SEL 4 = `hmss_gpll0` 300 MHz; PLL probably off | 299 MHz measured on CPUs 4-7; LUT row 0 is "src 0" and "LUT index 0 is always sourced from this clock" (`clock-osm.c:3357-3361`); a PLL at L=15 would be 288 MHz |
| rail voltages, APM, MEM-ACC | unknown | nothing in hand; C0 and V1 |

So, yes: the gold cluster is at 300 MHz because its GFMUX sits on GPLL0 / 2 and nothing has moved it
(**unverified** until C0 reads 0x17811054).

## 2. This phone's voltage table

The inputs are the reader dmesg (`reader/dmesg.txt:265-281`): speed bin 2, fusing revision 3, so combo 19.
The fused open-loop voltages are: silver 608 / 676 / 798 / 926 mV (LowSVS / SVS / NOM / TURBO_L1); gold
646 / 646 / 798 / 1036 mV. Each fuse is 6-bit sign-magnitude × 10 mV around the references 688/756/828/1056 and
756/756/828/1056 mV (`cprh-kbss-regulator.c:462-475, 526-527`). The method is Linux's
(`cprh-kbss-regulator.c:1020-1154`, `cpr3-util.c:1267-1550`):

1. Add `qcom,cpr-open-loop-voltage-fuse-adjustment` for combo 19: silver +40/+24/+12/+30 mV, gold +8/0/+12/+52 mV.
   Make the result monotonic.
2. Interpolate linearly in frequency between the fmax corners (`qcom,cpr-corner-fmax-map`: silver 8/11/18/22,
   gold bin 2 8/12/20/30).
3. Add the per-corner `qcom,cpr-open-loop-voltage-adjustment`, -4 to -28 mV.
4. Round up to 4 mV and clamp to the DT floor and ceiling.

Linux then uses the open-loop value as the corner's ceiling (`qcom,cpr-scaled-open-loop-voltage-as-ceiling`), and
closed loop only lowers the voltage from there. Aging can add up to 15 mV
(`qcom,cpr-aging-max-voltage-adjustment = <0x3A98>`), so the rule below adds **16 mV** (4 steps). The values
were computed here from DT and dmesg data; Linux's own per-corner log is debug-level and not in the dmesg
(**unverified** against Linux's internal numbers).

| gold (bin 2) MHz | 902.4-1132.8 | 1190.4 | 1267.2 | 1344.0 | 1420.8 | 1497.6 | 1574.4 | 1651.2 | **1728.0** | 1804.8 | 1881.6 | 1958.4 | 2035.2 | 2112.0 | 2208.0 | 2265.6 | 2323.2 | 2361.6 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| corner | 9-12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 30 |
| open-loop mV | 648 | 660 | 680 | 700 | 720 | 740 | 760 | 776 | 796 | 824 | 860 | 888 | 924 | 956 | 996 | 1020 | 1044 | 1060 |
| rail needed (+16) | 664 | 676 | 696 | 716 | 736 | 756 | 776 | 792 | 812 | 840 | 876 | 904 | 940 | 972 | 1012 | 1036 | 1060 | 1076 |
| MEM-ACC level (LUT) | 1 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 (3 by threshold) | 2 (3) | 3 | 3 | 3 | 3 | 3 | 3 |

| silver MHz | 825.6 | 883.2 | 960.0 | 1036.8 | 1094.4 | 1171.2 | 1248.0 | 1324.8 | 1401.6 | 1478.4 | **1555.2** | 1670.4 | 1747.2 | 1824.0 | 1900.8 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| open-loop mV | 644 | 656 | 676 | 692 | 704 | 720 | 736 | 752 | 768 | 780 | 796 | 840 | 868 | 896 | 928 |
| rail needed (+16) | 660 | 672 | 692 | 708 | 720 | 736 | 752 | 768 | 784 | 796 | 812 | 856 | 884 | 912 | 944 |
| MEM-ACC level (LUT) | 1 | 1 | 1 | 1 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 (3 by threshold) | 3 | 3 |

Corners at or above 800 mV need the APM on the APC rail. Corners at or above 852 mV take MEM-ACC level 3 even
where the LUT says 2, because Linux moves the highest MEM-ACC threshold to the first VC at or above 852 mV
(`clock-osm.c:2050-2069`). Gold 1728 and silver 1555.2 are the highest corners below both thresholds; they are
also the NOM fuse corners' fmax and Linux's v2 boot rates.

## 3. Raising the clocks (question 2)

### 3.1 Routes

| route | what it writes | proven from EL1 on this phone? | reach | verdict |
|---|---|---|---|---|
| A. Linux: CPRh + OSM | CPRh registers, OSM LUT/policy/enable, 7 SCM io-writes (x0 0xC2000502), GCC RCGs | yes: the reader's Linux does exactly this ("OSM driver inited", `dmesg.txt:336`) | everything up to 1900.8 / 2361.6, with APM, MEM-ACC and the voltage sequenced by hardware | the safe full solution; a port (stage A1) |
| B. Constant voltage, direct PLL and GFMUX | gold (or silver) PLL MODE/L/USER/CONFIG, GFMUX cfg/cmd | **no**: Linux on v2 leaves these to the OSM sequencer | up to the corner the boot voltage supports, capped at 1728 / 1555.2 | the minimal step (G1) |
| C. Raise the voltage by hand (SAW4 VCTL as `sweet_perf.c` did on 8996, or SPMI S13 `VOLTAGE_LSB/MSB` 0x40/0x41), then APM and MEM-ACC by hand | SAW or PMIC, APM mode 0x179D0010, MEM-ACC 0x179D1368-70 | no; on v2 the APM and MEM-ACC writes belong to the sequencer and may be TZ-only | turbo | not recommended: no closed loop, ordering errors corrupt SRAM |

A fully safe route to 1.9 GHz silver and above 1.73 GHz gold needs CPRh and SCM, which XNU does not have yet:
route A. The safest intermediate is route B at the boot voltage.

### 3.2 G1: gold at constant voltage

Preconditions. All come from C0 and V1; any failure means log the reason and leave the cluster at 300 MHz:

1. The gold OSM is off: 0x179C3004 bit 0 = 0, and the LUT at 0x179C3150.. is all zero.
2. Gold GFMUX: cfg 0x17811054 [10:8] = 4, and cmd 0x17811050 bit 0 = 0.
3. A believable gold rail voltage V. Either SAW4 0x17812C18 decodes to 568-1136 mV, stable over 3 reads with
   busy bits [17:16] clear (encoding **unverified**), or SPMI S13 (V1).
4. The gold MEM-ACC level L_acc, read from 0x179D1370: 0x80000000 is level 1, 0 is level 2, 1 is level 3; any
   other value refuses (`perfcl-apcs-mem-acc-val`, `live.dts:2495`; that the registers hold one level's pattern
   is **unverified**).
5. The gold APM status 0x179D0018 & 0x1F is 0 (MX) or 3 (APC).
6. The silver PLL and GFMUX values are readable (they serve as the template, below).

Target selection: the highest all-core bin-2 row with L ≥ 43, frequency ≤ min(`taimengold`, 1728), section-2
"rail needed" ≤ V, and LUT MEM-ACC level = L_acc. If its open-loop is ≥ 800 mV, the APM must also read 3. With
L_acc = 1 the cap is 1132.8 MHz. With V around 800 mV and L_acc = 2 it is 1651.2 MHz; at 812 mV or more, 1728.

Ramp. Run one step per row through 902.4 (L 47), 1132.8 (59), 1344.0 (70), 1574.4 (82) and 1728.0 (90), up to the
target. Each step bounds the load step and leaves a log line at each frequency. A step:

```c
/* P = 0x17816000 (gold PLL), G = 0x17811000 (gold GFMUX page), S = silver equivalents, read in C0 */
if ((rd(G, 0x54) >> 8 & 7) != 4) { park: wr(G, 0x54, rd(G, 0x54) & ~0x71F | 0x400); wr(G, 0x50, 0x3); poll(G, 0x50, bit0 == 0, 500 us); }
wr(P, 0x00, 0x0);                        /* PLL off: nothing uses it while the mux is on GPLL0 */
wr(P, 0x04, L);
wr(P, 0x0C, silver_user_ctl_or(0x01000008));   /* clock-osm.c:1514 (osm-pll-setup values) */
wr(P, 0x10, silver_config_ctl_lo_or(0x20004AA8));
wr(P, 0x00, 0x2); delay(10);             /* BYPASSNL */
wr(P, 0x00, 0x6); delay(60);             /* + RESET_N */
poll(P, 0x2C, bit16, 200 us) or poll(P, 0x00, bit31, 200 us);   /* lock; which bit is right is unverified */
wr(P, 0x00, 0x7);                        /* + OUTCTRL */
wr(G, 0x54, rd(G, 0x54) & ~0x71F | silver_src_sel << 8);       /* 5 (early) expected, from silver's cfg */
wr(G, 0x50, 0x3); poll(G, 0x50, bit0 == 0, 500 us);            /* the OSM expects 0x2 after the update */
```

On any timeout: write cfg = 0x400, then cmd = 0x3, and turn the PLL off. Log the result and stop.

Checks after each step:

- `perf_mhz()` bound to CPUs 4-7 must read 19.2 × L within 1 %.
- A self-test on each gold core: an xorshift-filled 1 MiB buffer, its FNV-1a hash, and a 64-bit multiply chain,
  10 rounds, compared with CPU0's result. CPU0 waits at most 2 s. On a mismatch or a timeout, CPU0 parks the gold
  GFMUX on GPLL0 (allowed from another cluster) and panics with the step in the message.
- While the perf thread runs (every 10 s), read TSENS. If any of 0x10AA000 sensors 7-10 (the gold CPUs per
  mainline msm8998.dtsi, **unverified**) is above 90 °C, park gold on GPLL0. Nothing else throttles: no OSM, no
  LMh, no `msm-thermal`. Linux's software thermal reset is at 115 °C (`qcom,therm-reset-temp`, `live.dts:4340`).

Run G1 from the perf thread before userland gets busy. `sweet_perf_init` starts it from `sweet_ufs.c:676/681`,
and it should run after `8/8 CPUs initialized` so that cluster 1 is powered. XNU never power-collapses a cluster
(there is no PSCI CPU_SUSPEND in the tree), so TZ will not reset the mux behind our back.

### 3.3 Silver

Silver already runs 1555.2 MHz, its NOM fmax. 1670.4 MHz would need V_silver ≥ 856 mV with the APM on APC, and
1900.8 MHz needs 944 mV and MEM-ACC level 3. Expected outcome: S1 refuses. If it is allowed, the sequence is 3.2's
with the silver registers. The silver cores keep running at 300 MHz from GPLL0 while their PLL is reprogrammed.

### 3.4 Above NOM: stage A1 (route A), outline

1. SCM probe, read-only, smc only (never hvc). First Linux's own version check: x0 = 0xC2000601 (SiP, SMC64,
   atomic, svc 6 INFO, cmd 1 IS_CALL_AVAIL), x1 = 1 (SCM_ARGS(1)), x2 = 0x82000601; expect x0 = 0 and x1 = 1
   (`scm.c:560-577`). Then ask about io-write the way `scm_is_call_available` does: x0 = 0x42000601 (standard
   call), x1 = 1, x2 = 0x02000502; expect x1 = 1 (`scm.c:1110-1130`).
2. CPRh, open loop first: per-corner registers with bit 29 set (closed loop disabled) and the section-2 voltages,
   CTL base/multiplier/OSM_ENABLED, then loop enable. Closed loop (fuse quotients, aging) comes later.
3. OSM as in 1.3, steps 4-12, with an XNU index policy (start with a fixed index; add load-based selection later).

References: the downstream driver; mainline-style ports in
https://lkml.iu.edu/hypermail/linux/kernel/2107.0/00214.html (OSM, v6) and
https://lkml.iu.edu/hypermail/linux/kernel/2407.1/00482.html (CPR3/CPR4/CPRh v15). Neither series is merged as far
as I could find (**unverified**).

## 4. Read-only register dump to add first (question 3)

Map each region with `ml_io_map` and use 32-bit reads only. Log the region name **before** reading it, so a reset
names the culprit. The "Linux reads it" column says whether this firmware is already known to allow the access
from EL1 under QHEE.

| address | register | Linux reads it? | what the value tells |
|---|---|---|---|
| 0x784130, 0x78413C, 0x784218/1C, 0x784228/2C, 0x784324 | fuse rows 38, 39, 67, 69, 100 | yes (CPRh fuses) | speed bin [31:29] (expect 2); fusing revision (row 39 bits 51-53, expect 3); silver/gold open-loop fuses (expect 608/676/798/926 and 646/646/798/1036 mV); force-highest-corner (row 100 bit 45). Validates the section-2 table on-device |
| 0x179C0000, 0x179C2000 | OSM version | yes (`clk_osm_mb`) | block present |
| B+0x1004 | ENABLE | yes (writes) | expect 0: nobody runs the OSM |
| B+0x1F10, B+0x1C00, B+0x1C74 | desired index, WDOG pstate, WDOG PC | yes (+0x1C00/0x1C74 in the panic dump) | expect 0 |
| B+0x1150+32i (i 0-39, 5 words) | LUT | write-only in Linux (**unverified** that reads work) | expect 0; print non-zero rows only |
| B+0x1070, +0x101C, +0x1034, +0x1F00 | FSM ctrl, CC hysteresis, LLM disable, cycle counter | yes (+0x1F00 is only written) | reset values = untouched by XBL |
| 0x17911050/54, 0x17811050/54 | GFMUX cmd/cfg silver/gold | no (same 4 KiB page as the IPC doorbell 0x17911008 that `sweet_rpm.c` writes) | SRC_SEL: silver 5 or 3, gold 4 expected; DIV |
| 0x17916000 +0x0/4/C/10/14/18/1C/2C, 0x17816000 same | PLL MODE, L, USER, CONFIG_LO/HI, TEST_LO/HI, STATUS | no (mapped only; **risk**: separate pages) | silver: L 0x51, locked (the template for G1); gold: off or idle |
| 0x1791101C/20 | CPU debug mux | write on measure | which cluster feeds GCC sel 0xC0 |
| 0x17914800, 0x17814800 (+0x0, +0x3C) | ACD version, GFMUX_CFG | yes (writes) | ACD bypassed (expect 0) |
| 0x179D0004/0C/10/18 | APM mode/status | status in the panic dump | 0 = arrays on MX, 3 = on APC: with the voltage, the G1 gate |
| 0x179D1360, 1364, 1368, 136C, 1370 | MEM-ACC | no (sequencer) | current level per cluster (3.2 rule 4) |
| 0x179D112C/30, 0x179D143C | OSM clock RCG, ITM present | yes (RCG) | OSM clock state |
| 0x17912000, 0x17812000: +0xFD0, +0xC04, +0xC0C, +0xC10, +0xC14, **+0xC18**, +0x900, +0x904, +0x908, +0x0, +0xC | SAW4 version, ID, SPM_STS, STS2, AVS_STS, **PMIC_STS**, VCTL, AVS_CTL, AVS_LIMIT, SPM_CTL, CFG (`msm-spm.c:97-120`) | yes | PMIC_STS: last voltage sent to S10/S13 (mV in the low bits, **unverified**), 0 if none |
| 0x148008, 0x148044/48, 0x14805C/60, 0x100000 | GCC rbcpr CBCR (bit 31 CLK_OFF), rbcpr RCG, hmss_gpll0 RCG, GPLL0 mode | yes | CPRh clock state; GPLL0 / 2 = 300 MHz |
| 0x179C8000/0x179C4000 +0x4, +0x8, +0x7F8, +0x3A00..+0x3A9C, **+0x3AA0**, **+0x3AA4** | CPRh CPR_CTL, status, margin ctl, corners, CTL, STATUS | yes, with the clock on (`cpr3-regulator.c:4504-4535`) | **only if 0x148008 bit 31 = 0**. CTL bit 0 = 0 means unprogrammed; STATUS [17:6] last voltage |
| 0x10AA000/0x10AD000 + 0x10A0+4n, +0x1060+4n, +0x1004 | TSENS status, critical threshold, INT_EN | yes (`msm-tsens.c:86-120`) | deci-°C in [11:0] (signed), valid = bit 21; whether a critical reset is armed |

Do **not** read B+0x300..0x9FF (sequencer registers and microcode, TZ-owned). Linux only writes them, through SCM.

## 5. DDR (question 4)

### 5.1 RPM resources (Linux 8998: `clock-gcc-8998.c:63-121`, IDs in `msm-clocks-hwio-8998.h:22-56`)

| type (LE ASCII) | id | key | clock |
|---|---|---|---|
| clk2 0x326b6c63 | 0 | "KHz " 0x007a484b | bimc = DDR. Linux: `vote_bimc(&bimc_clk, INT_MAX)` active set, first thing in `msm_gcc_8998_probe` (`clock-gcc-8998.c:2691`, `clock-rpm.c:330-350`) |
| clk0 0x306b6c63 | 2 | "Enab" 0x62616e45 = 1, sleep then active | RPM clock scaling (`enable_rpm_scaling`, `clock-rpm.c:293-327`, called at `clock-gcc-8998.c:2739`) |
| clk1 0x316b6c63 | 0 / 1 / 2 | "KHz " | cnoc_periph / snoc / cnoc |
| mmxi 0x69786d6d, ipa 0x00617069, ce 0x00006563 | 0 | "KHz " | mmssnoc_axi (display), ipa, ce1 |
| aggr 0x72676761 | 1 / 2 | **"KHz "** (8998 declares them as rate clocks; the 8996 table sends "Enab") | aggre1_noc (UFS), aggre2_noc |
| clk0 0x306b6c63 | 1 | "Enab" | qdss |
| clka 0x616b6c63 | 1-3 ln_bb, 4-6 rf, 0xb-0xd div | "swen" (and "pccb" for the pin twins 1-6) | XO buffers (8996's id 8 does not exist on 8998) |

### 5.2 Frequency table

`qcom,cpubw` bw-tbl, `live.dts:2333` (MB/s), with the MHz from the comments in downstream `msm8998.dtsi:720-732`:
762=100, 1144=150, 1525=200, 2288=300, 3143=412, 4173=547, 5195=681, 5859=768, 7759=1017, 9887=1296,
11863=1555, 13763=**1804 MHz**. That is bw / 8 B (`slv-ebi` buswidth 8, `msm8998-bus.dtsi:740-747`); the bimc
"KHz" vote is that MHz value. Linux policy: cpubw uses governor "performance" (`live.dts:2330`), so the AP always
votes the top entry. The memlat maps would ask for 1017 MHz with gold at 1728 and 1804 MHz with gold at 2208
(`live.dts:2371-2384`).

### 5.3 Will a vote from XNU raise it

Probably yes (**unverified**). The 8996 port reached 1804 MHz with the same two resources
(`ref/ios-sailfish/README.md:25`, patches 0145-0147), and the 8998 Linux uses the same type, id, key and scaling
switch. Changes needed in `scratch/xnu`:

- `perf_thread`'s `sweetddr` branch is compiled out on TAIMEN.
- `clk_measure_mhz` uses 8996 addresses: 0x362000 and 0x343000 become 0x162000 and 0x143000, and selector 0xad
  becomes 0x14e and 0xa9.
- `rpm_handoff[]` must become the 5.1 table. Only the rate clocks and qdss must be acked before "Enab" is sent; log
  XO-buffer failures and do not count them.

The RPM itself sets the CX/MX rails that DDR needs, so the vote cannot undervolt anything.

### 5.4 Measuring (read-mostly: only GCC debug registers are written, then restored)

| register | value |
|---|---|
| GCC_DEBUG_CLK_CTL 0x162000 | [9:0] selector, [15:12] post-div (write 0 = ÷1, `clock-local2.c:1229-1233`), bit 16 enable |
| PLLTEST_PAD_CFG 0x16200C | 0x51A00 (`clock-gcc-8998.c:2296`) |
| GCC_XO_DIV4_CBCR 0x143008 | bit 0 on during the measurement |
| CLOCK_FRQ_MEASURE_CTL / STATUS 0x162004 / 0x162008 | as `sweet_perf.c` `dbg_run`: ticks 0x1000, then 0x10000; rate = ((n·10 + 15) · 4.8 MHz) / (0x10000·10 + 35) |
| selectors | 0x14e (downstream "bimc_clk", debugcc "ddrss_gcc_dbg_clk"), 0xa9 (debugcc "bimc_clk"), 0xbb (bimc_hmss_axi), 0xbc (rbcpr, 19.2 MHz when on: the calibration point), 0x0 snoc, 0xe cnoc, 0xc0 CPU (pwrcl ÷4, perfcl ÷8 per `clock-osm.c:822-830`) |

debugcc programs a ÷4 into GCC_DEBUG_CLK_CTL [3:0] and multiplies by 4 (`msm8998.c:24-30`). That overlaps the
selector field and disagrees with downstream. Use downstream's [15:12] = 0 and check against 0xbc (19.2 MHz) and
0xc0 (1555.2/4 = 388.8 MHz expected). On 8996 the bimc selector read DDR/2 (`sweet_perf.c` comment). The 8998
ratio is **unverified**: measure before and after the 1804 MHz vote and fit the factor (1, 2 or 4) to the 5.2 table.

## 6. Risks and how each stays bounded

| risk | effect | containment |
|---|---|---|
| a C0 read hits an xPU or stage-2 protected page (PLL pages, MEM-ACC, LUT reads) | reset, or an SError and a panic | log the region before each group; reset goes to the reader with the last line; split C0 into C0a (Linux-proven reads) and C0b (the rest) |
| CPRh read with its clock off | bus hang | gate on 0x148008 bit 31; on a hang the APSS WDT bites |
| G1 PLL or GFMUX write blocked by QHEE | immediate reset (the MSM8996 GICR_WAKER precedent) | first write logged; then route A only |
| PLL does not lock, or the RCG update never completes | gold stays on GPLL0 | 200 µs and 500 µs polls, revert, log |
| wrong voltage estimate: undervolted gold | wrong results, exceptions, a hung core, **silent memory or file-system corruption** | refuse without a believable V; +16 mV margin; cap at NOM 1728; ramp; per-core self-test with a timeout; first G1 boots with the root mounted read-only if the tooling allows (**unverified** option), and `backup/` holds the partitions |
| MEM-ACC or APM inconsistent with the new frequency | SRAM errors | G1 never changes level or rail; target limited to the current level |
| heat with no throttling | TSENS critical reset (threshold **unverified**) | TSENS check parks gold at 90 °C; short deadline (`taimenreboot=180`) |
| RPM scaling on with an incomplete handoff | a NoC (UFS aggre1, display mmssnoc) drops to its minimum: slow or stalled UFS log, display underflow | handoff votes first, acked one by one; no "Enab" on any failure; the bimc vote last |
| RPM rejects or ignores the vote | nothing changes | measure before and after |
| SCM call from XNU (A1) not passed by QHEE | reset, or x0 returned unchanged (the SYSTEM_RESET pattern in `taimen-smp-plan.md` 2) | probe with `IS_CALL_AVAIL` first; smc only, never hvc |
| hang anywhere | APSS WDT, then the reader | each stage logs its step before acting |

## 7. Experiment ladder (question 5)

All stages are TAIMEN-only and off unless their boot-arg is given; C0 may default on once proven. Run from
`perf_thread` after `8/8 CPUs initialized`, with `taimenreboot=180`. Command as in the SMP plan, with extra args
last: `ROOT=1 ROOTFIX=<blob> tools/taimen/autoboot.sh <kernel> out/taimen-xnu-<r>-clk cpus=8 sweetperf=1 <args>`.

| stage | boot-arg | change | proves | expected lines |
|---|---|---|---|---|
| C0a | `taimenclkdump=1` | reads: fuses, OSM ENABLE/desired/WDOG, SAW4, APM status, GCC, TSENS, CPRh (if clocked) | hand-over state, fuse decode | `TAIMEN CLK: fuse bin 2 rev 3 silver 608/676/798/926 gold 646/646/798/1036 mV`; `TAIMEN CLK: osm0 ver=0x... en=0 want=0 pstate=0x...  osm1 ... en=0`; `TAIMEN CLK: saw1 ver=0x4... pmic_sts=0x... (V? mV)`; `TAIMEN CLK: apm0 sts=0x.. apm1 sts=0x..`; `TAIMEN CLK: cprh1 not read: rbcpr clock off` or `cprh1 ctl=0x... (osm_en 0) sts=0x...` |
| C0b | `taimenclkdump=2` | adds: GFMUX, PLLs, MEM-ACC, ACD, LUT, ITM | gold on GPLL0, silver on L81 | `TAIMEN CLK: gfmux0 cfg=0x5.. (src 5)  gfmux1 cfg=0x400 (src 4)`; `TAIMEN CLK: pll0 mode=0x...7 l=81 user=0x... cfg=0x... sts=0x...`; `TAIMEN CLK: pll1 mode=0x0 l=...`; `TAIMEN CLK: memacc 1364=0x0 1370=0x...(level n)`; `TAIMEN CLK: lut0/lut1 all zero` |
| C1 | `taimenclkmeasure=1` | GCC debug-mux counts only | debug-mux scale; DDR today | `TAIMEN CLK: measure 0xbc 19200 kHz, 0xc0/pwrcl 388800 kHz, 0xc0/perfcl 37500 kHz, 0x14e N kHz, 0xa9 N kHz` (CPU values **unverified**: the ÷4/÷8 dividers) |
| V1 | `taimenvread=1` | SPMI read only: PM8998 SID 1, S10 0x2F00 and S13 0x3800, registers 0x04/05 (type 0x1C, subtype 0x0A FTS426), 0x40/0x41 voltage in mV, 0x46 enable (`qpnp-regulator.c:111, 138-139`); arbiter v3 read path from `spmi-pmic-arb.c` (APID map, observer channel at +0x8000·apid, `:1047-1057`), base addresses in `taimen-audit-display-touch-gpu-wifi.md` item 4 | the gold/silver rail voltage | `TAIMEN SPMI: arb ver 0x3...; S10 type 0x1c sub 0x0a en 0x80 V 8xx mV; S13 ... V ... mV` |
| D1 | `taimenddr=1804` | 8998 handoff, scaling, bimc 1804800 kHz; measure; memcpy | DDR follows the AP vote | `SWEET RPM: link up ...`; `SWEET RPM: clock 316b6c63 id 0 2147483647 kHz: ok` (one per resource); `SWEET RPM: clock handoff (0 votes failed)`; `SWEET RPM: clock scaling on: ok`; `SWEET RPM: clock 326b6c63 id 0 1804800 kHz: ok`; `TAIMEN CLK: ddr sel 0x14e A -> B kHz, 0xa9 C -> D kHz`; `SWEET PERF: cpuN memcpy ... MB/s` above boot 41's 0.4-1.3 / 1.55 GB/s |
| G1 | `taimengold=1728` (MHz cap; 0 = off) | 3.2 ramp | gold at the boot voltage | `TAIMEN CLK: gold V=8xx mV (S13) acc 2 apm 0 -> target 1728.0 MHz (corner 20, needs 812)`; per step `TAIMEN CLK: gold L=47 902.4 MHz: locked 7x us, gfmux 0x400 -> 0x500, cmd 0x2`, `TAIMEN CLK: gold cpu4-7 902/902/902/902 MHz, selftest ok`; then `SWEET PERF: MHz 1555/1555/1555/1555/1728/1728/1728/1728`. Refusal: `TAIMEN CLK: gold left at 300 MHz: rail voltage unknown (saw pmic_sts 0x0, no SPMI)` |
| S1 | `taimensilver=1900` | same for silver | expected refusal | `TAIMEN CLK: silver stays 1555.2: needs 856 mV for 1670.4, have 8xx` |
| A1 | `taimenscm=1`, later `taimenosm=1` | 3.4 | SCM path; OSM + CPRh | `TAIMEN SCM: is_call_avail(0x02000502) -> 1`; later `TAIMEN OSM: cprh1 open-loop corners 1-30 written, ctl=0x...1; osm1 enabled, index 19 -> 1728.0` |

Reading a failure:

| log | meaning | next |
|---|---|---|
| ends right after `reading <region>` | that page is protected | drop it from the dump; for the PLL/GFMUX pages, G1 is impossible and only A1 remains |
| `osm1 ... en=1` or a non-zero LUT | something programmed the OSM | do not touch the PLL; work through +0x1F10 instead |
| `gfmux1` SRC_SEL not 4 | gold already on a PLL | log and stop; re-read section 1.5 |
| `pmic_sts=0` and no V1 | voltage unknown | port V1 |
| `PLL did not lock` | wrong USER/CONFIG template or a lock-bit guess | compare with silver's live values; try MODE bit 31 |
| self-test mismatch or timeout at step k | step k is too fast for the rail | panic leaves the line; cap below step k; recheck V |
| `clock handoff (n votes failed)` | an id is not on 8998 | the line before names it; fix the table |
| scaling on, bimc unchanged | the RPM needs another vote (e.g. "slv" EBI bandwidth, **unverified**) | compare with Linux's `msm_bus_rpm_smd.c` traffic |

## 8. Open questions

- The gold and silver rail voltages XBL leaves, and the SAW4 `PMIC_STS` encoding in mV mode.
- Whether EL1 under QHEE may read and write the cluster PLL pages (0x17916000, 0x17816000), the GFMUX
  (0x17911050, 0x17811050) and MEM-ACC (0x179D136x). Linux on v2 never touches them.
- What exactly the MEM-ACC registers encode (the level patterns in the DT), and whether XBL left gold at the level
  that matches its voltage.
- Which PLL lock bit applies (STATUS bit 16 or MODE bit 31), and whether full-rate rows use the early output
  (SRC_SEL 5) or main (3). Silver's live state answers both.
- The ratio between the debug-mux reading and the DDR clock on 8998 (0x14e or 0xa9), and the DDR rate XBL leaves.
- Whether the 8998 RPM needs anything besides the bimc clock vote (EBI bandwidth) to raise DDR.
- Whether TZ's SCM io-write is reachable from XNU (A1), and whether the OSM microcode is present before Linux runs.
- The TSENS sensor-to-CPU map and the hardware critical-reset threshold.
- Whether the LineageOS `lineage-19.1` files equal AOSP `android-msm-wahoo-4.4`.
