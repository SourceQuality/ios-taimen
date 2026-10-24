# Wi-Fi on taimen (WCN3990 through the modem DSP): implementation plan

2026-10-24. Plan only, nothing built or run. Reference keys used below:
`DTS` = research/taimen/live.dts, `dmesg` = hw/dmesg.txt (LineageOS boot on this phone),
`W/` = research/dl/msm-wahoo/ (downstream msm-4.4, wahoo), `M/` = research/dl/mainline-6.1/,
`X/` = scratch/xnu/, `TZ/` = ref/ios-topaz/xnu/patches/, `FL` = ref/ios-flame/HANDOFF.md,
`NOTES` = port/NOTES-taimen.md.

## 0. Verdict in one paragraph

This is feasible, and most of it has been done before. The WLAN MAC firmware (`wlanmdsp.mbn`) runs on the
modem Hexagon as a user PD. The host talks to it over QMI (WLFW, service 0x45) through QRTR on GLINK-over-SMEM.
Data goes through 12 copy engines at 0x18A40000. Two sibling ports already run this exact chain under XNU:
- **ios-topaz** in the kernel (`TopazModem`, TZ/0113-0130, SM6225).
- **ios-flame** in userland (`wland`, FL:605-640 and 168-198, SM8150, same CE block at 0x18800000+0x240000).

Taimen differs in three places:
1. **The modem boots by MBA self-authentication, not TZ PAS.** dmesg:536/883 says "No pas_id found", and DTS:3953 has
   `qcom,pil-self-auth` and `qcom,mem-protect-id = 0xF`. So topaz's PAS loader (TZ/0113) is replaced by a q6v55
   power-up plus the RMB handshake with the MBA. This is new code, but it uses only MMIO and the `hyp_assign`
   SCM that topaz already uses from XNU.
2. **The WLAN firmware is older.** It is FW 2.2.4.161.0 on HW WCN3990_V2.1 with HTT 3.83 (dmesg:3685, 3726).
   Flame and topaz ran WLAN.HL.3.x. Downstream ICNSS never sends host_cap, BDF or cal_report
   (W/drivers_soc_qcom_icnss.c:1974-2058).
3. **SoC addresses** (SMEM, IPC bits, SMMU, GCC, RAM).

The upper layers, from WMI-TLV to the iOS face, carry over from sailfish_wlan.c / SailfishWLAN.cpp. Their WMI
command IDs and TLV tags are the same as topaz's.

**Hardest stage:** booting the modem through the MBA (stage 5). After that comes keeping it alive with
rmtfs/tftp/pd-mapper and the IPA answer (stage 7).

**First milestone:** M0, a read-only SMEM and SCM census (section 4).

## 1. Facts that fix the design

- Android starts `vendor.rmt_storage`, `vendor.tftp_server` and `vendor.pd_mapper` (dmesg:2964-2968), and only then
  boots the modem (dmesg:3169-3458).
  - The servreg locator is up at dmesg:3008.
  - The modem's "msm/modem/wlan_pd" (instance 180) comes up at dmesg:3540.
  - WLFW connects at dmesg:3542.
  - "WLAN FW is ready" arrives 3.8 s later (dmesg:3639).
  - tftp_server touches persist (dmesg:3498).
- The MBA is 1 MB (dmesg:3227). MPSS is `modem.mdt` plus `.b02`-`.b27` (dmesg:3184-3440). `modem_a` is VFAT,
  mounted at /firmware (dmesg:1853).
- Addresses below come from the `reg` values, not the node names, which are stale as with the splash region:
  - MBA region: 0x94900000 + 0x200000 (DTS:16399).
  - MPSS region: 0x8CC00000 + 0x7800000 (DTS:16411).
  - Mainline's msm8998 map (M/arch_arm64_boot_dts_qcom_msm8998.dtsi:53-110) is a different phone layout. Do not use it.
- XNU owns RAM from 0xb1000000 (NOTES:713-714). The 28 MB at 0x88f00000-0x8aafffff is Linux System RAM that XNU
  does not manage (NOTES:28-29). Mainline puts rmtfs at 0x88f00000. Proposed **modem pool**: 0x89000000 + 16 MB,
  below 4 GB, mapped Normal-NC or Device. It holds:
  - the SCM argument pages,
  - the mdt metadata,
  - the MSA (1 MB) and rmtfs (2 MB) buffers,
  - CE rings, HTT buffers and WMI host memory.
- The boot-args budget is about 602 of 607 bytes (NOTES:756). Use one numeric boot-arg, `taimenwifi=N` (stage level).

## 2. Bring-up chain, stage by stage

Each stage lists what it touches, what XNU already has, what to write (rough lines) and the risks.

### Stage 1. Rails and clocks (WLAN and MSS)

WLAN rails, as ICNSS sets them (W/drivers_soc_qcom_icnss.c:230-241, DTS:4722-4739):
- pm8998 L5 0.8 V (DTS:2966)
- L7 1.8 V pin-ctrl (DTS:3015)
- L17 1.304 V pin-ctrl (DTS:3188)
- L25 3.312 V pin-ctrl (DTS:3333)
- `cxo_ref_clk_pin` = RPM `clka` id 5 key `pccb` (rf_clk2_pin)

Flame lesson 6 (FL:630-633): turn WLAN power on *before* the modem boots, and keep the **QDSS clock** on (RPM
`clk0` id 1 `Enab`). Without QDSS the firmware takes an exception at calibration.

MSS proxy votes:
- `rwcx` and `rwmx` levels 0x180 = TURBO (DTS:3953-3988, `vdd_cx-voltage`, `vdd_mx-uV`; DTS:2704, 2841)
- XO
- QDSS
- boot ROM AHB: GCC vote 0x52004 bit 10

MSS active clocks (GCC 0x100000; W/include_dt-bindings_clock_msm-clocks-hwio-8998.h:61-62, 161, 213-216; enable
CBCR bit 0, poll bit 31 clear):
- `MSS_CFG_AHB` 0x8A000
- `MSS_Q6_BIMC_AXI` 0x8A040
- `MSS_SNOC_AXI` 0x8A03C
- `MSS_MNOC_BIMC_AXI` 0x8A004
- `gpll0_out_msscc`: vote 0x5200C bit 2

The restart BCR is GCC_MSS_RESTART at 0x179000 (DTS mss `restart_reg`). Write 0 to release it, then wait 2 us
(W/drivers_soc_qcom_pil-msa.c:210-239).

- **Have:** RPM over GLINK at 0x778000 with LDO, rail-level and clock votes. The RPM handoff already votes qdss,
  rf_clk2 and rf_clk2_pin (X/bsd/dev/sweet_rpm.c:37-47, 381-479, 521-560).
- **Write:** about 80 lines (votes plus GCC branch enables).
- **Risk:** low. The pin-ctrl LDOs are voted with `swen` like normal LDOs. If the RPM NACKs them, use the non-pin
  regulators instead.

### Stage 2. SMEM, hwspinlock, SMP2P

- SMEM is 0x86000000 + 2 MB (DTS:2634). The hwlock is TCSR mutex 0x1F40000 (8 locks, stride 0x1000, DTS:2616); SMEM uses lock 3.
- Downstream expects **SMEM version 11** (W/drivers_soc_qcom_smem.c:69-71):
  - global items sit in the legacy global heap (TOC in the header);
  - host-pair items sit in the `$TOC` private partitions.
- SMP2P items (M/...msm8998.dtsi:414-432, DTS:306-312, 439-459):
  - out 428 (APPS to modem), in 435;
  - doorbell APCS IPC 0x17911008 bit 14 (`irq-bitmask 0x4000`);
  - modem IRQ SPI 451.
- Out entry `master-kernel`: bit 0 force-stop.
- In entry `slave-kernel`:
  - bit 0 err-fatal
  - bit 1 err-ready
  - bit 2 proxy-unvote/handover
  - bit 3 stop-ack
  - bit 7 shutdown-ack
  - (DTS:3974-3979)
- A second pair, `ipa` out/in (DTS:520-539), carries the modem's IPA power query. TZ/0113:425-427 and 476-487:
  if it goes unanswered, the modem watchdog fires once data services start.
- SFR crash string: SMEM item 421, in the *global heap* on v11.
- **Have:** nothing in X/.
- **Write:** port TZ/0113 smem/smp2p (about 300 lines). Its global lookup (`SMEM_GLOBAL_HOST` 0xFFFE) is v12-only,
  so add the v11 global-heap TOC lookup (about 40 lines).
- Flame lessons 1-2 (FL:617-620):
  - create item 428 with features SSR_ACK *before* booting the modem, or it never posts READY;
  - mirror RESTART_DONE into RESTART_ACK.
- **Risk:** low.

### Stage 3. SCM `hyp_assign` and the modem pool

- The call is SCM svc 0x0C cmd 0x16 (W/drivers_soc_qcom_secure_buffer.c:41-56, 320-327).
- Arguments: `{mem list pa, size, src vmid list pa, size, dest {vm,perm,ctx,ctx_size} list pa, size, 0}`.
- Arginfo is `SCM_ARGS(7, RO,VAL,RO,VAL,RO,VAL,VAL)`. Arguments 3 and up go indirectly through x5.
- VMIDs (W/include_soc_qcom_secure_buffer.h:27-47): HLOS 3, MSS_MSA 0xF, WLAN 0x18, WLAN_CE 0x19. Perms: R 4, W 2, X 1.
- **Have:** `scm_call` with 3 arguments and x5 = 0 (X/bsd/dev/sweet_gpu.c:567-598, static). PAS from XNU works on
  this phone (NOTES:482-486).
- **Write:**
  - move `scm_call` into a shared `sweet_scm.c`;
  - add the indirect-argument form and `scm_assign()` from TZ/0121:1982-2014 / TZ/0122:36-80;
  - total about 90 lines, plus a bump allocator over the pool (about 40).
- Rules from flame lesson 8 (FL:635-637):
  - never take MSA or rmtfs memory back while the modem runs; the SMC never returns;
  - one rmtfs buffer per phone boot;
  - unmap a region from XNU before assigning it away. Linux keeps these regions `no-map`, and a speculative or
    stray access from HLOS is an XPU violation.
- **Risk:** low. TZ accepted this call from XNU on SM6225 and SM8150.

### Stage 4. Firmware access (file names only)

From `modem_a` (VFAT, image/):
- `mba.mbn`
- `modem.mdt`
- `modem.b00`..`modem.b27`
- whatever the modem fetches later over TFTP: MBN configs and similar, unknown until it is logged.

From /vendor/firmware (research/taimen/firmware/list.txt:28-29, 86, 125):
- `wlanmdsp.mbn` (3,723,164 bytes)
- `bdwlan.bin` / `bdwlan.b09` (19,152 bytes each)
- `modemuw.jsn` (713 bytes)

EFS lives on modemst1 (sdf1), modemst2 (sdf2), fsg (sde32) and fsc (sdf3) (NOTES:15). Backups are in backup/.
Locally there are only GPU files: nothing above has been pulled yet.

How each set gets to the kernel:
- **modem_a:** read in the kernel from UFS, as TZ/0115 does. XNU's `sweet_ufs.c` only knows one named partition on
  LUN 0 (X/bsd/dev/sweet_ufs.c:307-356, 591-660). Port the read-only "any GPT partition on LUNs 0-5" API (about 100
  lines), plus either:
  - a host-generated extent table, as topaz does (needs a one-off modem_a dump), or
  - an in-kernel FAT16/32 directory walker (about 250 lines). Recommended: no PC dump needed.

  The full modem image cannot ride the payload: ABL starts no ramdisk over about 35 MiB, and the payload is already
  22.9 MB (NOTES:627-633).
- **The three /vendor files:** payload blobs through `/chosen/memory-map`, as `wl_blob` does
  (X/bsd/dev/sailfish_wlan.c:996-1009). This adds about 3.8 MB. Alternatively, a one-off ROOTFIX zlib record into
  the root filesystem.
- **EFS:** read directly from UFS by rmtfs (stage 7).
- **Risk:** low. Pulling modem_a needs root adb once, or none at all with the in-kernel FAT reader.

### Stage 5. Modem boot through the MBA (the new part)

Registers (DTS:3953-3988):
- QDSP6SS 0x4080000
- RMB 0x4180000
- AXI halt q6/modem/nc at 0x1F63000/0x1F65000/0x1F64000
- wdog IRQ SPI 448

The q6v55 power-up is downstream `__pil_q6v55_reset` for `qcom,qdsp6v62-1-2` (W/drivers_soc_qcom_pil-q6v5.c:357-556,
offsets 30-37, bits 47-82). Order: proxy votes, MSS restart deasserted, active clocks, then:
1. Copy `mba.mbn` (at most 1 MB) to 0x94900000, then hyp_assign HLOS to MSS_MSA RW
   (pil-msa.c:543-647; W/drivers_soc_qcom_peripheral-loader.c:186-200).
2. RMB writes (pil-msa.c:488-511):
   - `RMB_MBA_IMAGE` (+0x00) = 0x94900000
   - `PMI_CODE_START` (+0x14) = 0
   - `PMI_CODE_LENGTH` (+0x18) = 0 (there is no msadp debug policy, dmesg:3226)
3. Q6 power-up:
   - `RESET`(0x14) |= CORE_ARES|BUS_ARES_ENA|STOP_CORE;
   - `XO_CBCR`(0x38) |= 1, poll bit 31 clear;
   - `PWR_CTL`(0x30) |= BHS_ON(24), poll `BHS_STATUS`(0xC4) bit 0;
   - |= LDO_BYP(25); &= ~CLAMP_QMC_MEM(22); |= L2DATA_STBY_N(18)|SLP_RET_N(19);
   - `MEM_PWR_CTL`(0x34) bits 28..0 one at a time, 1 us apart;
   - clear CLAMP_WL(21), then CLAMP_IO(20);
   - `RESET` &= ~(CORE_ARES|STOP_CORE); `GFMUX_CTL`(0x20) |= CLK_ENA(1).
4. Poll `RMB_PBL_STATUS`(+0x04) until non-zero; it must be 1. Then poll `RMB_MBA_STATUS`(+0x0C) for 1 or 2
   (XPU unlocked). Timeout 1 s (pil-msa.c:241-274).
5. Metadata (pil-msa.c:676-770):
   - copy `modem.mdt` into a 4K-aligned pool buffer and assign it to MSS_MSA;
   - `PMI_CODE_LENGTH` = 0, `PMI_META_DATA`(+0x10) = pa, `MBA_COMMAND`(+0x08) = 1;
   - poll STATUS for 3 (a negative value is an MBA error code), at most 10 s;
   - assign the buffer back to HLOS.
6. MPSS segments (peripheral-loader.c:846-874, pil-msa.c:786-852):
   - assign 0x8CC00000+0x7800000 from HLOS to {HLOS, MSS_MSA};
   - map it, copy each PT_LOAD `.bNN`, zero-fill up to memsz;
   - per segment: if `CODE_LENGTH` is 0, set `CODE_START` = segment pa and `COMMAND` = 2; then
     `CODE_LENGTH` += size; check STATUS >= 0;
   - unmap, then reclaim {HLOS, MSS_MSA} to MSS_MSA only;
   - poll STATUS for 4 (AUTH_COMPLETE), at most 10 s;
   - reclaim the MBA region to HLOS.

   Mainline instead gives MSS the MPSS region *before* the MBA runs, for MBAs that wipe it
   (M/drivers_remoteproc_qcom_q6v5_mss.c:1101-1110). Keep that as the fallback ordering.
7. Wait for SMP2P `slave-kernel` bit 1 (ready). On bit 0, print SFR item 421.

- **Have:** nothing for this. The zap PAS loader (X/bsd/dev/sweet_gpu.c:610-717) only donates mdt/ELF parsing.
  Mainline msm8998 data is at M/drivers_remoteproc_qcom_q6v5_mss.c:2173-2201.
- **Write:** about 450 lines: ELF/mdt 80, MBA plus assign 60, q6v55 reset 120, RMB 80, segment loader 80, halt/dump 50.
- **Risks:**
  - PBL/MBA failures are silent apart from status codes. Log all 9 RMB registers on every failure, like
    `modem_log_rmb_regs` (pil-msa.c:99-119).
  - An unclocked QDSP6SS/RMB access hangs the bus. Touch them only after the clocks are on.
  - Assigning a region XNU still maps.
  - Not a blocker: no PAS is involved, so TZ's view of the HLOS does not matter beyond `hyp_assign`.

### Stage 6. GLINK over SMEM, then QRTR

- **GLINK** edge "mpss" (DTS:3521-3527):
  - SMEM items 478 (descriptor, 32 bytes: tx tail/head, rx tail/head), 479 (our 16 KB TX FIFO, we allocate it)
    and 480 (the modem's FIFO);
  - doorbell APCS IPC 0x17911008 **bit 15** (topaz uses bit 12);
  - IRQ SPI 452;
  - one channel, `IPCRTR`, fragmented data (`TX_DATA_CONT`) (DTS:3650-3658).
- **QRTR:** IPC router v1 header. APPS node 1 (DTS:3646-3649); the modem says its own node in HELLO. Control port
  0xFFFFFFFE. We are the APPS name service.
- **Have:** the GLINK command layer for the *intentless* RPM edge (X/bsd/dev/sweet_rpm.c:59-316). The SMEM edge
  needs RX intents, so reuse it only as a pattern.
- **Write:** port TZ/0118 (590 lines) with the bit and address changes, plus the QMI TLV encode/decode from TZ/0121
  (about 150).
- Flame lessons 4-5 (FL:622-627):
  - control packets are always 20 bytes, and `RESUME_TX` = {cmd, node, port};
  - intents are per channel;
  - TFTP blocks of 1024 bytes fit the 1152-byte reusable intents.
- **Risk:** low. The same modem-side GLINK and QRTR stack runs on all these SoCs, and mainline msm8998 uses it.

### Stage 7. Host services the modem needs from APPS

| Service | Detail | Topaz source |
|---|---|---|
| **rmtfs** (QMI 14) | OPEN `/boot/modem_fs1`, `fs2`, `fsc`, `fsg`; `ALLOC_BUFF`; `GET_DEV_ADDR`; `RW_IOVEC` | TZ/0121:1965-2060 |
| **tftp** (QRTR 4096) | serves by path from modem_a, plus `wlanmdsp.mbn` and `bdwlan.*`; `/readwrite/*` kept in RAM (topaz: dropped writes starve `modem_cfg`) | TZ/0121:1596-1960 |
| **pd-mapper** (servreg locator QMI 0x40) | maps `wlan/fw` to `msm/modem/wlan_pd` from `modemuw.jsn` | TZ/0121:1540-1595 |
| **IPA power query** | SMP2P `ipa` answer (stage 2) | TZ/0113 |

rmtfs detail:
- The buffer is a dynamic 2 MB (DTS:2531-2536: `reg <0 0x200000>`, client-id 1), assigned to {HLOS, MSS_MSA}.
- The DTS has no NAV path; topaz also adds NAV, which taimen does not need.
- Reads come from UFS. Writes are not persisted.
- Without rmtfs the modem err-fatals about 40 s after boot (flame lesson 3, FL:621).

All three daemons run on this phone (dmesg:2964-2968). `memshare` (DTS:3930-3952) and `ssctl`/`service-notifier`
are not needed. Mainline msm8998 Wi-Fi runs without memshare. Log every QRTR NEW_SERVER and lookup to confirm.

- **Have:** nothing.
- **Write:** about 750 lines (from TZ/0121's 2,092, which include a 1,350-line generated file table we do not need with a FAT walker).
- **Risk:** medium. Some service could turn out to be required on this firmware: memshare, or a time service.
  Watch for err-fatal and SFR strings.

### Stage 8. WLFW QMI (service 0x45 v1 instance 0) to mission mode

Message IDs are in W/drivers_soc_qcom_wlan_firmware_service_v01.h:16-65. This firmware's downstream order
(W/drivers_soc_qcom_icnss.c:1974-2058, 3073-3145):
1. `IND_REGISTER` 0x20: fw_ready, msa_ready, pin_connect_result, client_id (header:173-185).
2. `MSA_INFO` 0x2D: msa_addr = our 1 MB pool block, size 0x100000 (icnss allocates it dynamically, 4371-4388).
   The response lists up to 2 regions with `secure_flag`.
3. Assign each region from HLOS to {MSS_MSA, WLAN, +WLAN_CE if not secure}, RW (icnss.c:1024-1061).
4. `MSA_READY` 0x2E.
5. `CAP` 0x24: chip, board and soc info, fw version and build id. Log them.
6. Optional: `DYNAMIC_FEATURE_MASK` 0x3B and `INI` 0x2F.
7. Wait for `FW_READY_IND` 0x21. The firmware loads its BDF itself, presumably over TFTP.
8. `WLAN_CFG` 0x23:
   - host_version;
   - 12 CE target configs (W/drivers_net_wireless_ath_ath10k_snoc.c:167-287);
   - service map (289-398);
   - 12 shadow registers (399-414; flame: Linux sends 12).
9. Optional `MAC_ADDR` 0x33 (exists in this header:58). The firmware reports no MAC of its own (dmesg:3676).
10. `WLAN_MODE` 0x22 = MISSION (0).

Fallback if `FW_READY` never comes: the mainline order. That is `HOST_CAP` 0x34 with the 8-bit quirk set on msm8998
(M/...msm8998.dtsi:2585, M/drivers_net_wireless_ath_ath10k_qmi.c:617-642, 771-818), then `BDF_DOWNLOAD` 0x25 in
6144-byte segments and `CAL_REPORT` 0x26 after `MSA_READY_IND` (qmi.c:875-890).

- **Write:** port TZ/0122 (425 lines). Drop BDF/cal by default and add the MAC (about 350 lines). Flame lesson 7
  (FL:634): the QMI buffer must be 8 KB or more.
- **Risks:**
  - MSA permission ordering; mainline sleeps 20 ms before the assign on SDM845 (qmi.c:795-799).
  - Never touch WLAN registers before the mission response: a register read hangs the bus (TZ/0122:87-91).

### Stage 9. WLAN DMA through the anoc2 SMMU

- The SMMU is at 0x16C0000 + 0x40000, `qcom,skip-init` and not TZ-owned (DTS:5314-5325). Streams are 0x1900 and
  0x1901 (DTS:4728).
- Downstream attaches the WLAN with **S1 bypass** (W/drivers_soc_qcom_icnss.c:3380-3425), so CE addresses are
  physical addresses.
- Do what topaz does (TZ/0123:52-66; flame FL:185-188): read IDR0/IDR1, take a free SMR and CB, then set:
  - SMR = 0x1900 mask 1;
  - S2CR = translate to that CB;
  - CBAR 0x1f000, CBA2R VA64;
  - SCTLR 0xe0 (M=0);
  - TLBI and sync.
  
  Do this *before* the modem boots. Keep ABL's existing entries.
- **Have:** the GPU's `smmu_identity` with traced writes and readback (X/bsd/dev/sweet_gpu.c:424-492), and sailfish's
  SMMU dump (X/bsd/dev/sailfish_wlan.c:753-767).
- **Write:** about 100 lines.
- **Risks:**
  - the hypervisor traps global SMMU writes. On the GPU, writes went through but an all-streams SMR stalled a CPU
    (sweet_gpu.c:424-433). So match only 0x1900/1.
  - an unmatched-stream fault turns the first CE DMA into a hang.

### Stage 10. Copy engines, HTC, WMI-TLV init

CE `n` sits at 0x18800000 + 0x240000 + 0x1000·n, for n = 0..11. The wrapper is at +0x24C000; RRI-in-DDR address
lo/hi at 0x24C004/0x24C008. Register offsets (W/drivers_net_wireless_ath_ath10k_hw.c:141-165, 287-301;
TZ/0123:155-178):

| Register | Offset |
|---|---|
| SR base lo/hi | 0x0 / 0x4 |
| SR size | 0x8 |
| DR base lo/hi | 0xC / 0x10 |
| DR size | 0x14 |
| CTRL1 | 0x18 (dmax[15:0], src swap 17, dst swap 18, RRI update 19) |
| HOST_IE | 0x2C |
| MISC_IE | 0x34 |
| SR_WR | 0x3C |
| DR_WR | 0x40 |
| SRRI | 0x44 |
| DRRI | 0x48 |
| WM | 0x4C / 0x50 |

- Descriptors are **16 bytes**: le64 address (37 bits used), u16 nbytes, u16 flags, u32 hash (W/drivers_net_wireless_ath_ath10k_ce.h:60-65).
- Shadow write-index registers are at 0x18832000 + 4·i (FL:188).
- Pipe map (W/snoc.c:289-398; matches qcacld's log, dmesg:3655-3671): HTC ctrl out 0 / in **2**, WMI out 3 / in 2,
  HTT out 4 / in 1, extra HTT rx 9/10, log 11. On QCA6174 the HTC ctrl rx pipe is CE1.
- HTC is unchanged: READY, then CONNECT for HTT (0x300) and WMI (0x100), then SETUP_COMPLETE_EX.
- WMI-TLV: SERVICE_READY, then INIT, then READY. WMI host-memory chunks must be below 4 GB unless service 179 is set
  (FL:190); the pool is.
- Polled first. The CE IRQs are SPI 413-418 and 420-425 (DTS:4729). Downstream uses rising edge
  (W/snoc.c:999); mainline uses level-high.
- **Have:** HTC send/poll/connect, the WMI TLV helpers, the SERVICE_READY parse and INIT
  (X/bsd/dev/sailfish_wlan.c:1439-1760, 1808-1931).
- **Write:**
  - the CE layer (about 300 lines, from TZ/0123:153-375);
  - the WCN3990 resource config instead of the QCA6174 table at sailfish_wlan.c:1901-1906;
  - pipe numbers;
  - interrupts later (about 150).
- **Risk:** low-medium. Older firmware may reject fields in the resource config; log the full SERVICE_READY.

### Stage 11. HTT (64-bit) and scan

- HTT setup (TZ/0125:38-66), on CE4:
  - VERSION_REQ;
  - FRAG_DESC_BANK_CFG (64-bit);
  - RX_RING_CFG 64-bit (host ring of buffer addresses plus a host-written alloc index);
  - AGGR_CFG v2.
- RX comes back as `RX_IN_ORD_PADDR_IND` (0x12). The firmware does full reorder offload (dmesg:3656, 3683).
- The rx descriptor is the WCN3990 v2 layout (TZ/0125:56-66).
- Without the host rx ring no beacon reaches the scan (FL:191).
- Scan: sailfish's `wl_scan` (sailfish_wlan.c:1938-2183; the IDs match TZ/0123:552-567).
- **Write:** about 400 lines (rx ring, in-order indication parse, 64-bit tx descriptor).
- **Risk:** medium. HTT 3.83 against topaz's newer firmware, for example the in-order indication format.

### Stage 12. Station, WPA2, data, iOS face

Keep sailfish's:
- 4-way handshake, GTK handling and crypto (sailfish_wlan.c:2215-2342, 2535-2731);
- key install;
- peer and vdev commands (2184-2209);
- the mbuf queue (2728-2794);
- Apple80211 (2795-3019);
- link change and rejoin (3121-3175);
- request loop and thread (3531-3683);
- SailfishWLAN.cpp (320 lines).

These have already passed wifid, configd and Settings on iOS 16 (port/docs/upstream-sailfish-README.md:32).

Change:
- HTT TX to the **64-bit** descriptor (`ath10k_htt_tx_64`). Sailfish uses 32-bit at 2414-2459.
- management frames (auth/assoc) through **WMI `MGMT_TX_SEND` 0x7008, tag 0x1a6** (TZ/0129:356, 369), when
  the service bit allows it. Otherwise HTT mgmt as sailfish does.
- the MAC: no OTP MAC comes back in WMI READY (sailfish_wlan.c:1926-1929). Use the WLFW MAC or a locally
  administered one.
- add `wlan` to the ADT for wifid's `_LimitLoadToDeviceTree`.

- **Write:** about 250 lines changed. TZ/0129 (station on WCN39xx) is the reference.
- **Risk:** low.

## 3. sailfish_wlan.c, layer by layer

| Lines | Layer | Taimen |
|---|---|---|
| 60-117, 123-595 | PCIe rails, GCC, PARF/PHY/DBI, PERST, link | drop (stages 1, 5 instead) |
| 600-850 | QCA6174 CE (8-byte desc, BAR 0x34400) | rewrite (stage 10) |
| 825-1190 | diag, BMI, OTP/firmware download, early config, PLL | drop: no BMI on SNOC; the modem loads the firmware |
| 1191-1438 | cold reset, stage 2 | replace with modem plus WLFW (stages 2-8) |
| 1439-1760 | HTC + WMI wait | keep; pipe numbers change |
| 1761-1807, 2414-2470 | HTT 32-bit setup and TX | rewrite 64-bit (stage 11) |
| 1808-1931 | WMI SERVICE_READY/INIT/READY | keep; new resource config and MAC source |
| 1938-2183 | scan, BSS table | keep |
| 2184-2731 | join, keys, 4-way, GTK, crypto | keep; mgmt TX path changes |
| 2732-3019 | ifnet queue, Apple80211 | keep |
| 3020-3120, 3176-3239 | ASPM, temperature, PCIe IRQ | drop; CE IRQs later |
| 3240-3700 | run loop, join requests, thread | keep; stage calls change |

Recommended shape:
- a new `bsd/dev/taimen_modem.c` (stages 1-9, ported from TopazModem with the MBA loader), exporting
  `taimen_modem_up()` and `taimen_wlfw_mission()`;
- a `taimen` SNOC variant of `sailfish_wlan.c` that keeps the upper half. Compile it on
  `ARM64_BOARD_CONFIG_TAIMEN`. Both board configs are defined in this tree.

The alternative is topaz's whole station plus its `redwood_*` wifid face (TZ/0900, 2,377 lines). That is more new
surface in this tree.

## 4. Milestones (each proven by a log line; boot-arg `taimenwifi=N`)

| M | Proves | Log line to look for |
|---|---|---|
| 0 | SMEM readable, layout known, SCM available; no state changed | `TAIMEN MODEM: SMEM v11, ptable N, apps<->mpss part @0x.. ; items 428/435/478 absent; hwlock3 free; scm avail(0c/16)=1 pas_supported(4)=X` |
| 1 | hyp_assign works from XNU (one 4 KB pool page HLOS→{HLOS,MSS}→HLOS) + WLAN/MSS rails voted | `TAIMEN MODEM: assign rt 0/0 0/0; L5/L7/L17/L25 ok, cx/mx turbo ok, qdss on` |
| 2 | modem_a and EFS readable from UFS | `TAIMEN MODEM: modem_a: mba.mbn 1048576?, modem.mdt N segs; modemst1 ok` |
| 3 | MBA runs | `TAIMEN MODEM: PBL 1, MBA status 1 (XPU unlocked)` |
| 4 | MPSS authenticated and running | `TAIMEN MODEM: meta auth 3, AUTH_COMPLETE 4, slave-kernel 00000002 READY` |
| 5 | GLINK + QRTR up | `TAIMEN MODEM: IPCRTR open, HELLO node 0, services: ...` |
| 6 | modem stable with host services, WLAN PD loaded | `rmtfs open /boot/modem_fs1`, `tftp RRQ .../wlanmdsp.mbn done`, `NEW_SERVER 0x45`, still alive at +120 s |
| 7 | WLFW mission | `TAIMEN WLFW: cap chip .. fw 2.2.4..; FW_READY; mode MISSION ok` |
| 8 | SMMU + CE + HTC | `TAIMEN WLAN: SMMU SMRk 0x1900/1 CBk bypass; HTC READY credits ..` |
| 9 | WMI ready + scan | `TAIMEN WLAN: WMI READY; scan: N BSS` |
| 10 | join + data | `SAILFISH WLAN: CONNECTED to ... (WPA2-PSK, CCMP)`, DHCP lease, wifid shows the network in Settings |

After M4, every failed run means a phone reboot. MSA and rmtfs stay assigned, and there is no in-session modem
restart (FL:635-637). Put timeouts on every wait, and never access CE registers after `slave-kernel` bit 0.

## 5. Estimate

| Work | Lines |
|---|---|
| Ported from TopazModem with SoC changes | about 2,700 (smem/smp2p, GLINK/QRTR/QMI, rmtfs/tftp/pd-mapper, WLFW, CE, HTT) |
| New | about 800 (MBA loader, SMEM v11, FAT walker, SMMU bank, glue) |
| Reused from sailfish_wlan.c with edits | about 1,400 |
| **Total** | about 4,500-5,000 lines touched |

Calendar:
- Flame went from "modem boots" to wifid in about 30 hours with topaz as reference (FL:605, 168, 8).
- Taimen adds the MBA path and the older firmware.
- Realistic: **5-8 working days, 30-50 phone boots.** That assumes M3/M4 do not stall on an unknown MBA error. If they
  do, add 2-4 days of RMB-status bisection.

## 6. Open questions

1. SMEM version on taimen: 11 expected. Is item 428 allocated in the apps-mpss partition by us, as topaz does?
2. MPSS region ownership: hand it over before the MBA runs (mainline 6.1) or only for loading (downstream)?
3. Are the `modem.mdt` PT_LOADs relocatable, or fixed at 0x8CC00000? Read the phdrs at M2.
4. Which TFTP paths does the modem request, for `wlanmdsp.mbn`, `bdwlan.b09`/`.bin` and any `/readwrite`?
   Does the firmware load the BDF over TFTP (assumed) or expect `BDF_DOWNLOAD`?
5. Does FW 2.2.4 accept `WLFW_MAC_ADDR` (0x33) and WMI `MGMT_TX_SEND` (0x7008)? Otherwise use HTT mgmt and set the
   MAC through WMI.
6. Does the modem ask for memshare (QMI 0x34) or other APPS services, and stall without them?
7. Does the msm8998 IPA v3 (hw-ver 0xB) use the same SMP2P `ipa` query as SM6225?
8. Does anoc2 accept HLOS SMR/S2CR/CB writes, and which indices are free (ABL `skip-init` state)?
9. Is 0x89000000-0x8a000000 untouched by ABL and the shim? Check with a pattern at boot.
10. `PAS_IS_SUPPORTED(4)`: if TZ answers 1, a PAS boot of the modem (TZ/0113 nearly as is) is a cheap A/B against the MBA path.
11. CE IRQ trigger type, and the HTT in-order indication format at HTT 3.83.
12. Is the QDSS clock needed by this firmware as it was by flame's?

## 7. Dead ends and things ruled out

- **QCA6174/PCIe stack:** none of it applies (port/docs/taimen-audit-display-touch-gpu-wifi.md:57-62).
  `WL_DMA_PA` 0xa0000000 also collides with ABL's ramdisk (port/docs/taimen-porting-map.md:186).
- **TZ PAS for the modem:** not how this phone boots it (dmesg:536, 883). TZ/0113's PAS sequence and topaz's
  `PAS_ID_MODEM 4` do not transfer as is (open question 10 aside).
- **Mainline msm8998 carve-outs** (mba 0x94100000, mpss 0x7000000, wlan_msa 0x95700000): a different board map. On
  taimen 0x95700000 is inside the SLPI region.
- **Firmware in the payload:** the full modem image would push past ABL's ~35 MiB ramdisk limit.
- **The DTS `qcom,msm_ath10k_wlan` node** (DTS:4714, disabled): its driver (W/drivers_net_wireless_ath_ath10k_snoc.c)
  is Qualcomm's early ath10k-on-ICNSS. It is a good table source but not a separate path, because it still needs
  ICNSS for QMI.
- **Downstream ICNSS powers the rails only when WLFW appears** (icnss.c:2011). Powering them before the modem boots
  (flame) is harmless, so do that.
- **cnss-daemon** gets IPC-router permission failures on Android and Wi-Fi works anyway (dmesg:3538). Not needed.

## 8. Address and ID cheat sheet

| Item | Value |
|---|---|
| SMEM | 0x86000000/2 MB, v11; hwlock TCSR 0x1F40000 lock 3; SFR item 421 |
| APCS IPC | 0x17911008: bit 0 RPM, 14 SMP2P mpss, 15 GLINK mpss |
| SMP2P | out 428 `master-kernel`(b0 stop) + `ipa`; in 435 `slave-kernel` (b0 fatal, b1 ready, b2 handover, b3 stop-ack, b7 shutdown-ack) + `ipa` |
| GLINK | items 478/479/480, channel `IPCRTR`; QRTR APPS node 1 |
| MSS | QDSP6SS 0x4080000, RMB 0x4180000, halt 0x1F63000/5000/4000, restart BCR 0x179000, wdog SPI 448 |
| Carve-outs | MBA 0x94900000/2 MB, MPSS 0x8CC00000/120 MB, MSA 1 MB + rmtfs 2 MB from the pool |
| VMIDs | HLOS 3, MSS_MSA 15, WLAN 0x18, WLAN_CE 0x19; hyp_assign SCM 0x0C/0x16 |
| QMI services | WLFW 0x45, rmtfs 14, servreg-locator 0x40, TFTP QRTR 4096 |
| WLAN | membase 0x18800000; CE0 +0x240000 (+0x1000 each, 12 CEs); wrapper +0x24C000; shadow +0x32000; SMMU anoc2 0x16C0000 SID 0x1900/1; CE IRQ SPI 413-418, 420-425 |
| Rails | L5 0.8 V, L7 1.8 V, L17 1.304 V, L25 3.312 V, rf_clk2_pin, QDSS; MSS cx/mx TURBO |
