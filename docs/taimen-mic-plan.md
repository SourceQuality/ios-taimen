# Microphones on taimen (WCD9340 over SLIMbus, through the ADSP): implementation plan

2026-10-30. Plan only, nothing built or run. It builds on docs/taimen-audio-plan.md (`AP`), whose ADSP, GLINK, QRTR,
APR, LPASS SMMU stream 1 and `/dev/redwood-audio` stages run by default since boot 160. Reference keys:
`DTS` = research/taimen/live.dts, `dmesg` = hw/dmesg.txt (LineageOS on this phone), `MP` =
research/taimen/firmware/audio/mixer_paths_tavil_taimen.xml, `API` = .../audio_platform_info_tavil_taimen.xml,
`B156` = out/taimen-xnu-r139-audio/sweetlog.txt (boot 156's log), `X/` = scratch/xnu, `TA` =
X/iokit/Kernel/arm/TaimenAudio.cpp, `RA` = tools/phone/RedwoodAudio/audio_plugin.c, `NOTES` = NOTES-taimen.md,
`W/` = research/dl/msm-wahoo/. Read online on 2026-10-30 (local copies under research/dl/mic-*, not in git):
- `WH/` = the wahoo kernel (LineageOS android_kernel_google_wahoo, lineage-22.2), the driver Android ran on this phone:
  `drivers/slimbus/{slim-msm-ngd.c,slim-msm.c,slim-msm.h,slimbus.c}`, `include/linux/slimbus/slimbus.h`,
  `drivers/mfd/{wcd9xxx-core.c,wcd9xxx-slimslave.c,wcd9xxx-utils.c,wcd934x-regmap.c}`,
  `include/linux/mfd/wcd934x/registers.h`, `include/linux/mfd/wcd9xxx/wcd9xxx-slimslave.h`,
  `sound/soc/codecs/{wcd934x/wcd934x.c,wcd9xxx-resmgr-v2.c,audio-ext-clk.c}`, `sound/soc/msm/msm8998.c`,
  `sound/soc/msm/qdsp6v2/{msm-dai-q6-v2.c,msm-pcm-q6-v2.c,q6afe.c,q6asm.c,q6adm.c}`, `include/sound/apr_audio-v2.h`.
- `L/` = Linux v6.6: `drivers/slimbus/{qcom-ngd-ctrl.c,messaging.c,slimbus.h}`, `sound/soc/codecs/wcd934x.c`,
  `drivers/mfd/wcd934x.c`, `include/linux/mfd/wcd934x/registers.h`, `sound/soc/qcom/qdsp6/{q6asm.c,q6adm.c,q6routing.c}`,
  `drivers/pinctrl/qcom/pinctrl-msm8998.c`.
- `HK/` = HackiOS (github.com/oosexgang/hakios-xnu-kernel) `drivers/audio/hakios.mic.c`: the microphone of the same iOS 16
  userland through an Elite APR ADSP on SM7325 (SoundWire codec there; its ASM/ADM read path and ring are what we reuse).

## Status: not started

Playback works (AP status table). The RedwoodAudio plug-in already presents the "Codec" input and opens
`/dev/redwood-mic` (RA:829-880); without the node the input is silence. Nothing of SLIMbus, the WCD9340 or the record
path exists in X/ yet.

## 0. Verdict in one paragraph

Feasible, with one new protocol (SLIMbus satellite) and one new chip (the WCD9340's TX path); the DSP half is HackiOS's
proven record recipe. The chain: the bottom DMIC → WCD9340 decimator 7 → codec SLIMbus TX port 7 → shared channel 135 →
LPASS (the ADSP is the bus manager and framer) → AFE port SLIMBUS_0_TX 0x4001 → ADM TX COPP → ASM session 2 read into our
pool through LPASS SMMU stream 1 → a ring → `/dev/redwood-mic` → RedwoodAudio "Codec" ('imic'). The APPS side of SLIMbus is
small: a QMI handshake with the ADSP's slimbus service (0x301, already announced at B156), the non-ported generic device
(NGD) at 0x171C1000 used in **register mode** (no BAM), address queries, value-element register access to the codec, and
three user messages to connect and activate one channel. **Hardest:** the NGD's register-mode messaging (Android used BAM
message queues) and the codec's MCLK (RPM DIV_CLK1 out of a PMIC pin). **First milestone:** M1, the QMI handshake and the
NGD's capability exchange; read-only towards the codec.

## 1. The capture chain on this phone

### 1.1 Microphones and wiring

| Mic (API:114-128) | Codec input | Clock / bias | Android paths |
|---|---|---|---|
| builtin_mic_1, "bottom", omni, front | **DMIC0** on pad DMIC1 (factory `mictest-taimen-mic1` = path `dmic1`, MP:2226-2229; "1st MIC Bottom" MP:1326-1335; triple-mic ch3, MP:1403-1421) | CPE_SS_DMIC0_CTL, MIC BIAS1 (DTS:15802 routing) | `handset-mic`, `voice-rec-mic`, `unprocessed-mic` use path `dmic2` = **DMIC1**, the other clock edge of the same pad (MP:1337-1345, 1605-1621, 1867-1870) |
| builtin_mic_3, "top front" (geometric_location y 0.154) | DMIC2 (factory mic2 = `dmic3`, MP:1346-1355, 2231-2234) | DMIC1_CTL, MIC BIAS3 | `speaker-mic` (DEC5/TX5) |
| builtin_mic_2, "back" | DMIC4 (factory mic3 = `dmic5`, MP:1365-1373, 2236-2239) | DMIC2_CTL, MIC BIAS4 | `camcorder-mic` (DEC6/TX6, S24_LE) |
| headset mic (USB-C analog) | AMIC2 / ADC2 (`adc2`, MP:1317-1324) | MIC BIAS2 | not for us |

- All three internal microphones are **digital** (PDM). No AMIC is used for them; the DTS's "Handset Mic" on AMIC1
  (DTS:15802) is a generic route that the mixer paths never use.
- DMIC0 vs DMIC1 for the bottom mic: the taimen-specific factory paths say DMIC0, Android's everyday record paths say
  DMIC1. Both select the same pad; with a keeper on the data line either edge can carry the same mic. M6 measures both
  (`taimenmicdmic`, section 6); start with **DMIC0**.
- MIC BIAS1-4 at 1800 mV (DTS:2086-2089); DMIC rate 4.8 MHz in the DT (DTS:2093) but 2.4 MHz at 48 kHz (3.3).

### 1.2 The record path Android uses (main mic), resolved

`audio-record` = `MultiMedia1 Mixer SLIM_0_TX` (MP:1057-1059), so the back end is **SLIMBUS_0_TX, AFE port 0x4001**
(DTS:12306-12310; WH apr_audio-v2.h:932). Capture format default S16_LE (`set-capture-format-default`, MP:2037-2039).

| Mixer control (path `dmic1`/`dmic2`, `handset-mic`) | Register (WH registers.h; widgets WH wcd934x.c:6551, 6608, 6707, 7083) | Value |
|---|---|---|
| `ADC MUX7` = DMIC | CDC_TX_INP_MUX_ADC_MUX3_CFG1 **0x0D25** bits 3:2 | 0 |
| `DMIC MUX7` = DMIC0 (DMIC1) | CDC_TX_INP_MUX_ADC_MUX7_CFG0 **0x0D29** bits 5:3 | 1 (2) |
| `CDC_IF TX7 MUX` = DEC7 | CDC_IF_ROUTER_TX_MUX_CFG1 **0x0D3B** bits 7:6 | 2 |
| `AIF1_CAP Mixer SLIM TX7` | (codec DAI channel list: TX port 7) | port 7 |
| `SLIM_0_TX Channels` = One | AFE num_channels | 1 |
| `DEC7 Volume` 88 (+4 dB; 84 = 0 dB) | CDC_TX7_TX_VOL_CTL **0x0AA4** (SOC_SINGLE_SX_TLV min -84, WH wcd934x.c:5857) | 0x04 |
| decimator 7 on (the `ADC MUX7` widget) | CDC_TX7_TX_PATH_CTL **0x0AA1** bit 5 | 1 |

### 1.3 SLIMbus TX port and channel

- Codec TX port 7 (WH wcd934x.c:296-313, `WCD9XXX_CH(7, 7)`: port 7, shift 7).
- Shared channel = `tx_ch_tavil[7]` = **135** (128 + port; WH msm8998.c:3527-3530, set as the codec DAI's channel map at
  3592-3595; AFE gets the same numbers through msm_dai_q6_set_channel_map, WH msm-dai-q6-v2.c:1935-1955).
- Rate 48 kHz, 16 bits (`SLIM_0_TX` default), mono.

## 2. SLIMbus with the ADSP as manager

### 2.1 Who does what

| Piece | Who | Source |
|---|---|---|
| framer, bus clock (root 24.576 MHz, superframe 4000/s), clock gears | ADSP | WH slim-msm-ngd.c:1767-1771 (`rootfreq = 24576000 >> 3`) |
| enumeration (REPORT_PRESENT), logical-address assignment | ADSP | WH ngd_get_laddr asks, never assigns (`ngd_set_laddr` is empty, :1144-1148) |
| scheduling (segment distribution, NEXT_DEFINE/ACTIVATE_CHANNEL, RECONFIGURE_NOW on the bus) | ADSP, on our user messages | WH ngd_allocbw :1001-1140 |
| CONNECT_SOURCE to the codec's port on the bus | ADSP, on our `CONNECT_SRC` user message | WH ngd_xfer_msg :540-590 |
| the LPASS sink port on channel 135, the data into AFE | ADSP, at AFE DEVICE_START (`shared_ch_mapping`) | WH apr_audio-v2.h:2456-2461 |
| HW power and clocks of the SLIMbus block | ADSP, on our QMI POWER(ACTIVE) | WH slim-msm.c:1579-1588 |
| QMI select/power, NGD enable, capability reply, address queries | **us** | below |
| codec register reads/writes (value elements), codec port registers | **us** (the NGD talks on the bus as a satellite) | WH wcd9xxx-core.c:218-298 |
| connect src + define/activate + reconfig-now + remove for channel 135 | **us** | WH wcd9xxx-slimslave.c:337-443 |

### 2.2 The QMI slimbus service

- Service **0x301**, version 1, instance 0 (WH slim-msm.h:93-95; L qcom-ngd-ctrl.c:51-53). Already announced at B156:
  `ADSP service 301 v1 inst 0 node 5 port b`, 55 ms after the QRTR HELLO, with no lookup from us. Take node/port from
  NEW_SERVER each boot.
- We are a QMI **client** (TA has only a server, the pd-mapper, TA:1430-1479): send DATA from a local port (e.g. 0x4002)
  to (node 5, port), match the reply on txn id. QMI header 7 bytes `{u8 type (0 req, 2 resp), u16 txn, u16 msg, u16 len}`
  then TLVs, as `pdm_rx` builds them.

| Msg | Id | Request TLVs | Response |
|---|---|---|---|
| SELECT_INSTANCE | 0x20 | 0x01 u32 instance = `cell-index >> 1` = 0; 0x10 u32 mode = **2 (MASTER: the ADSP is the bus master)** (14 bytes) | 0x02 {u16 result, u16 error} |
| POWER | 0x21 | 0x01 u32 pm_req = **2 (ACTIVE)**, 1 = INACTIVE (7 bytes; no 0x10 TLV in this era, WH slim-msm.c:1209, 1241-1245) | 0x02 |
| CHECK_FRAMER_STATUS | 0x22 | none | not needed |

Order (WH slim-msm.c:1504-1556, slim-msm-ngd.c:1303-1336): SELECT_INSTANCE once, then POWER(ACTIVE); never send
INACTIVE while a session may run. **Touch no NGD register before the POWER response**: the block's clocks are the ADSP's.

### 2.3 The NGD registers, register mode, polling

- Block `slimbus_physical` 0x171C0000 + 0x2C000, BAM 0x17184000 + 0x32000, IRQ SPI 163 (NGD) and 164 (BAM), cell-index 1,
  `qcom,apps-ch-pipes` 0x1F80, `qcom,ea-pc` 0x210 (DTS:1997-2005).
- HW version = `readl(0x171C0000) >> 16`; NGD base = base + **0x1000** for cell-index 1 (WH `NGD_BASE_V2(1)`,
  slim-msm-ngd.c:39-41; L `ngd_v1_5_offset_info`, id 1: 1·0x1000 + 0·0x1000, qcom-ngd-ctrl.c:110-113, 1555-1556).

| Reg (0x171C1000 +) | Use (WH slim-msm-ngd.c:43-85) |
|---|---|
| 0x00 NGD_CFG | bit 0 enable, bit 1 RX msgq, bit 2 TX msgq. **Write 0x1** (register mode) |
| 0x04 NGD_STATUS | bit 1 NGD_LADDR (the manager gave us an address) |
| 0x08 NGD_RX_MSGQ_CFG | \|= 0x10000 as Linux does (harmless in register mode) |
| 0x10 NGD_INT_EN / 0x14 INT_STAT / 0x18 INT_CLR | bit 24 RECFG_DONE, 25 TX_NACKED_2, 26 MSG_BUF_CONTE, 27 MSG_TX_INVAL, 28 IE_VE_CHG, 29 DEV_ERR, 30 RX_MSG_RCVD, 31 TX_MSG_SENT. Write INT_EN = 0xFF000000 (L DEF_NGD_INT_MASK plus 24) |
| 0x30.. NGD_TX_MSG | a message's words, `(RL + 3) / 4` of them (WH slim-msm.c:637-650) |
| 0x70.. NGD_RX_MSG | word 0 bits 4:0 = RL; read `(RL + 3) / 4` words, then INT_CLR = bit 30 (WH slim-msm-ngd.c:122-140) |

- **Register mode instead of BAM.** Android registered the audio slimbus BAM (dmesg:2265, 31 pipes = 0x32000 window) and
  ran both message queues through it. The register paths are in the same driver: TX through `msm_send_msg_buf` when the
  TX msgq is not enabled (WH slim-msm.c:637-650), RX through `qcom,rxreg-access` (WH slim-msm-ngd.c:1749-1750, 1799-1800,
  122-140). Register mode needs no DMA, no BAM driver and no SMMU stream (the slim nodes have no `iommus`, DTS:1997-2260).
- **Poll, no IRQ.** TX: write the words, poll INT_STAT for bit 31 (sent) or bits 25-29 (error), clear by writing them to
  INT_CLR, 1 s timeout. RX: poll bit 30 every 1 ms while a reply is due. Downstream never sets INT_EN bit 30 even with
  rxreg-access (:1365-1415), so the status bit latches unmasked; still enable it. SPI 163 stays masked at the GIC.

### 2.4 Bring-up, capability exchange and logical addresses

1. After POWER(ACTIVE): read the version and NGD_STATUS. If LADDR is already set and INT_EN != 0, the HW was kept: just
   `NGD_CFG = 1` (WH :1347-1384).
2. Else: INT_EN = 0xFF000000, RX_MSGQ_CFG |= 0x10000, NGD_CFG = 1 (:1406-1424). The NGD reports present; the ADSP gives
   it an address (NGD_STATUS bit 1) and sends **MASTER_CAPABILITY** (user mc 0x00, mt 6 SRC_REFERRED_USER).
3. Answer **REPORT_SATELLITE** (mc 0x01, mt 6, to LA 0xFF, payload `D9 C5 01 01`: magic, version 1, protocol 1);
   retry on NACK up to 3 times, 10 ms apart (WH :1495-1549). The exchange is done when it is sent.
4. Codec out of reset (3.1), then **ADDR_QUERY** (mc 0x0D, mt 2 DEST_REFERRED_USER, to 0xFF, payload
   `[tid, e_addr[0..5]]`); the answer is **ADDR_REPLY** (mc 0x0E, mt 6): `buf[3]` tid, `buf[4..9]` the EA (all zero =
   absent), `buf[10]` the LA (WH :1150-1177, 1257-1278). Retry 5 x 20 ms (WH :1584-1592) within Android's 100 ms
   SLIMBUS_PRESENT_TIMEOUT (WH wcd9xxx-core.c:37, 1194-1213).

| Device | e_addr (DTS) | Expected LA (dmesg) |
|---|---|---|
| WCD9340 generic (registers) | `00 01 50 02 17 02` (DTS:2063-2064): instance 0, index 1, product 0x0250, Qualcomm 0x0217 | **0xCF** (dmesg:2585) |
| WCD9340 interface (port registers) | `00 00 50 02 17 02` (DTS:2092) | **0xCE** (dmesg:2586) |
| WCD9335 (absent; negative control) | `00 01 A0 01 17 02` (DTS:2008-2009) | none (dmesg:2560: -6) |
| LPASS PGD (not needed: no apps channels) | `00 05 10 02 17 02` (ea-pc 0x210, WH :552-560) | - |

### 2.5 Message format (register mode)

First word = `RL | MT<<5 | MC<<8 | DT<<15 | LA<<16` (WH slim-msm.h:66-67): byte 0 RL/MT, byte 1 MC, byte 2 destination LA
(DT 0 = logical); payload from byte 3. **RL = 3 + payload bytes** (Linux sets `rl = len + 4` then `rl--`, WH :586-588).
Core value messages add a TID (requests only) and a 2-byte element code `ec = slicecode | 1<<3 | offset<<4`, slicecode
`{1:0, 2:1, 3:2, 4:3, 6:4, 8:5, 12:6, 16:7}` bytes (WH slimbus.c:939-960, 1069-1074).

| Message | MT | MC | Dest | Payload | RL | Done when |
|---|---|---|---|---|---|---|
| REPORT_SATELLITE | 6 | 0x01 | 0xFF | D9 C5 01 01 | 7 | TX_MSG_SENT |
| ADDR_QUERY | 2 | 0x0D | 0xFF | tid, EA[6] | 10 | ADDR_REPLY |
| REQUEST_VALUE | 0 | 0x60 | 0xCF / 0xCE | tid, ec lo, ec hi | 6 | REPLY_VALUE (mc 0x64): buf[3] tid, data at buf[4] |
| CHANGE_VALUE | 0 | 0x68 | 0xCF / 0xCE | ec lo, ec hi, data[n] | 5 + n | TX_MSG_SENT (no reply) |
| CONNECT_SRC | 2 | 0x2C | 0xFF | codec LA, port, channel, tid | 7 | GENERIC_ACK |
| DEF_ACT_CHAN | 2 | 0x21 | 0xFF | (2.7), tid, channel | 9 | GENERIC_ACK |
| RECONFIG_NOW | 2 | 0x24 | 0xFF | tid, codec LA | 5 | GENERIC_ACK |
| CHAN_CTRL (remove) | 2 | 0x23 | 0xFF | `2<<6 \| (LA & 0x1F)`, tid, channel | 6 | GENERIC_ACK |
| DISCONNECT_PORT | 2 | 0x2E | 0xFF | codec LA, port, tid | 6 | GENERIC_ACK |

GENERIC_ACK = mc 0x25, mt 6: `buf[3]` tid, success when `buf[4] & 0x20` (WH :1279-1297, slim-msm.h:88). TIDs 0-255,
one table. Example, REPORT_SATELLITE as two words: 0xD9FF01C7, 0x000101C5.

### 2.6 Codec registers over SLIMbus

- No I2C or SPI register path on this board: the codec is a SLIMbus child (DTS:2062); its `wcd_spi` (SPI bus 2,
  DTS:2245-2252) is for the codec's own DSP (WDSP), which we skip. Android read the chip id over SLIMbus (dmesg:2594).
- Value element = **0x800 + (reg & 0xFF)** (WH wcd9xxx-core.c:35, 225, 265), on the generic device (LA 0xCF).
- **Pages:** registers are 16-bit `page<<8 | offset`; write the page to register 0x00 (element 0x800) when it changes
  (WH wcd9xxx-utils.c:451-494). Keep the last page; it is 0 after reset.
- Interface (port) registers: element 0x800 + reg on LA 0xCE, no paging (WH wcd9xxx-core.c:163-216).
- Reads: one REQUEST_VALUE of up to 16 bytes (e.g. 4 bytes at 0x821: ec = 0x821B). Writes: one CHANGE_VALUE per
  register; skip REPEAT_CHANGE_VALUE bulk writes. A read-modify-write keeps a 256-byte shadow per page.

### 2.7 The data channel for TX port 7 (WH wcd9xxx-slimslave.c:315-443, slimbus.c:1727-1868)

1. Interface device (LA 0xCE), elements 0x800 + reg:
   - 0x1F7 `SB_PGD_PORT_TX_OR_UR_CFG(7)` = 0x02 (Tavil auto-recovery);
   - 0x11C `TX_PORT_MULTI_CHANNEL_0(7)` = 0x80 (1 << shift 7), 0x11D `_1(7)` = 0x00;
   - 0x057 `PORT_CFG_BYTE_ADDR(0x50, 7)` = 0x05 (watermark 12 bytes, enable).
2. CONNECT_SRC `CF 07 87 tid` (codec LA, port 7, channel 135).
3. DEF_ACT_CHAN `0F 24 20 83 tid 87`, as WH ngd_allocbw (:1049-1095) builds it for 48 kHz, 16 bits, AUTO_ISO:
   - byte 0 `dataf<<5 | (LA & 0x1F)` = 0x0F (**not** the full 0xCF that L qcom-ngd-ctrl.c:1020 sends);
   - byte 1 `bits>>2 | coeff3<<5` = 0x24 (12 = 3·2², coefficient 3);
   - byte 2 `rootexp<<4 | proto` = 0x20 (exp 2, HARD_ISO 0: exact at superframe 4000);
   - byte 3 presence rate 0x83 (`slim_calc_prrate`: 4 kHz family, 12 = 3·2², exp 3, exact bit 7).
4. RECONFIG_NOW `tid CF`.
5. Down: CHAN_CTRL `8F tid 87` + RECONFIG_NOW (WH :472-496, slim-msm-ngd.c:1096-1130). Removing the channel
   disconnects its ports (WH slimbus.c:1440-1442); DISCONNECT_PORT `CF 07 tid` only after an error (WH :514-).

## 3. Codec bring-up

### 3.1 Power, reset, MCLK, IRQ

| Item | Value | Source |
|---|---|---|
| Supplies | all five (`cdc-vdd-buck`, `-buck-sido`, `-vdd-tx-h`, `-vdd-rx-h`, `-vddpx-1`) = **pm8998_s4 1.8 V**, static | DTS:2069-2085, 2781-2789; dmesg:2562-2566 |
| Reset | **TLMM gpio64**, WEST tile: CFG 0x03540000, IN_OUT 0x03540004 | DTS:15896-15902 (`qcom,cdc-rst-n-gpio`), L pinctrl-msm8998.c:1406 |
| Reset sequence | low 20 ms, high 20 ms, after the supplies settle 600 µs (skip the low pulse if already high: `avoid_cdc_rstlow`) | WH wcd9xxx-utils.c:653-699, wcd9xxx-core.c:1338-1351 |
| Reset pin config | active: function 0, 16 mA, pull-down, output high → CFG 0x3C1, IN_OUT bit 1 = 1; sleep: no pull, output low | DTS:9607-9632 |
| MCLK | **9.6 MHz** = RPM **DIV_CLK1** (XO/2), out of **PM8998 GPIO13** | DTS:2090, 15874-15886 (`clk_div_clk1` 0xAA1157A6, W/msm-clocks-8998.h:54); WH audio-ext-clk.c:238-266 (`audio_pmi_clk`, dummy ops, parent the RPM clock) |
| RPM vote | `clka` id **0xB** (DIV_CLK1_ID, W/msm-clocks-hwio-8998.h:45), key `swen` = 1, both sets: `sweet_rpm_xo_buffer(0xb)` (X/bsd/dev/sweet_rpm.c:520) | W/clock-gcc-8998.c:82 |
| PM8998 GPIO13 (SPMI 0xCC00, ppid 0x0CC) | output, CMOS, no pull (5), VIN0, low strength (1), **src-sel 3 = func2 (DIV_CLK1)**, enabled. LV/MV subtype: 0x40=0x01, 0x41=0x00, 0x42=0x05, 0x44=0x03, 0x45=0x01, 0x46=0x80; older subtype: 0x40 = mode<<4 \| src<<1 = 0x16 | DTS:893-905; X/bsd/dev/taimen_tas2557.c:628-681 handles both subtypes |
| Codec IRQ | TLMM gpio54 (`wcd9xxx-irq`, `qcom,gpio-connect`, pull-down input), 32 sources | DTS:15863-15873, 8948-8960 |

- The IRQ is not needed: MBHC, MAD, WDSP and SLIM port over/underflow use it. Poll the port status instead (M5).
- Register access needs no MCLK (Android read the chip id at dmesg:2594 before any capture). MCLK is for the digital
  path: on only while a session runs.

### 3.2 Init, once (M3/M4)

| Step | Writes (reg = value, [mask]) | Source |
|---|---|---|
| chip id | read 0x0021..0x0024 = **01 00 08 01** (minor 0x0001, major 0x0108 TAVIL = WCD9340 v1.1) | dmesg:2594; WH wcd934x.c:690-759, wcd934x-regmap.c:41-44 |
| bring-up | 0x0009 RPM_RST_CTL = 0x01; 0x071B SIDO_VOUT_A_STARTUP = 0x19; 0x071C _D_STARTUP = 0x15; 1 ms; 0x0011 PWR_CDC_DIG_HM_CTL = 0x05, = 0x07; 0x0009 = 0x03, = 0x07; 0x0011 = 0x03 | WH wcd934x.c:787-813; L mfd/wcd934x.c bring_up |
| MCLK rate | 0x0003 CODEC_RPM_CLK_MCLK_CFG [0x03] = 0x01 (9.6 MHz) | WH wcd934x.c:10044-10051 |
| defaults | `tavil_codec_reg_defaults` (36 masked writes); for TX: 0x0629 BIAS_VBG_FINE_ADJ = 0x75, 0x066C MICB1_TEST_CTL_2 [0x07] = 1, 0x0A33 + 0x10·n TXn_TX_PATH_CFG1 [0x01] = 0, 0x5022/0x5023 CPR SVS/SVS2 = 0x7C/0x58 | WH wcd934x.c:8514-8549 |
| common init | `tavil_codec_reg_init_common_val` (24) + `_1_1_val` (6): 0x021B CPE_SS_DMIC_CFG [0x80] = 0 (**DMICs out of reset**), 0x0D42 FS_CNT_CONTROL [0x01] = 1, 0x0002 RPM_CLK_GATE [0x08] = 0, 0x0D82 TOP_CFG1 [0x03] = 3, 0x0214 SVA_CFG [0x60] = 0, 0x021C CPAR_CFG [0x10] = 0x10 | WH wcd934x.c:8551-8624 |
| pdata | 0x0622/23/25/26 MICB1-4 [0x3F] = (1800-1000)/50 = 0x10; 0x803B PAD_DRVCTL_0 [0x0C] = 0 (DMIC clock 2 mA); 0x0218/19/1A DMIC0-2_CTL [0x0E] = 5<<1 (600 kHz MAD rate) | WH wcd934x.c:8903-9000 |
| CPR (v1.1) | with MCLK on: 0x5027 SVS2_MIN_CX_VDD = 0x2C, RPM_CLK_GATE [0x10] = 0, 16 pairs WR_DATA 0x5001-04 / WR_ADDR 0x5005-08 (`cpr_defaults`) | WH wcd934x.c:8558-8575, 8639-8670 |

### 3.3 Per session (M4 sets it, M5-M7 reuse it)

| Step | Writes | Source |
|---|---|---|
| SVS vote | 0x0202 CPE_SS_PWR_SYS_PSTATE_CTL_0 [0x01] = 1 | WH wcd934x.c:936-960 |
| MCLK | RPM DIV_CLK1 on (3.1) | WH wcd934x.c:8238-8260 |
| master bias | 0x0601 ANA_BIAS [0x80] = 0x80, [0x40] = 0x40, 1 ms, [0x40] = 0, [0x20] = 0 | WH wcd9xxx-resmgr-v2.c:167-190 |
| codec clock | 0x0711 CLK_SYS_MCLK_PRG [0x80] = 0x80, **[0x30] = 0 (no divider at 9.6 MHz; L's 0x10 is for 19.2)**, [0x02] = 0, [0x01] = 1; 0x0D42 [0x01] = 1; 0x0D41 MCLK_CONTROL [0x01] = 1; 0x0002 [0x03] = 0; 10 µs. (ANA_CLK_TOP is skipped on Tavil) | WH resmgr-v2.c:48-56, 218-298 |
| routing | 0x0D25 [0x0C] = 0; 0x0D29 [0x38] = dmic+1 << 3; 0x0D3B [0xC0] = 0x80 | 1.2 |
| rate | 0x0AA1 TX7_TX_PATH_CTL [0x0F] = 4 (48 kHz; table 8k 0, 16k 1, 32k 3, 48k 4, 96k 5, 192k 6) | WH wcd934x.c:7604-7690 |
| MIC BIAS1 | 0x0622 [0xC0] = 0x40 (enable), 1 ms (MIC BIAS3 0x0625 / 4 0x0626 for the census) | WH wcd934x.c:4497-4640 |
| DMIC clock | 0x0218 DMIC0_CTL [0x0E] = **2<<1 (÷4: 2.4 MHz**, because TX fs ≤ 48 kHz caps the DMIC at 2.4 MHz), [0x01] = 1 | WH wcd934x.c:4197-4300, 4319-4400 |
| decimator | 0x0AA1 [0x10] = 0x10 (mute), [0x20] = 0x20 (clock); 0x0AA2 TX7_TX_PATH_CFG0 [0x60] = 2<<5 (HPF 150 Hz), 0x0AA9 TX7_TX_PATH_SEC2 [0x02] = 1, 1 ms, = 0; 0x0AA4 = 0x04 | WH wcd934x.c:4043-4195 |
| unmute | after **40 ms** (`tx_unmute_delay`): 0x0AA1 [0x10] = 0 | WH wcd934x.c:511-513, 3995-4013 |
| settle | after **300 ms**: CFG0 [0x60] = the saved corner (default 0 = 4 Hz) with the SEC2 pulse | WH wcd934x.c:3960-3993 |
| down | mute, 0x0AA1 [0x20] = 0, DMIC0_CTL [0x01] = 0 and rate back to 5, MICB1 [0xC0] = 0, CLK_SYS_MCLK_PRG [0x81] = 0, ANA_BIAS [0xA0] = 0, PSTATE [0x01] = 0, DIV_CLK1 off | reverse of the above |

### 3.4 Skipped

MBHC (no 3.5 mm jack; it serves the USB-C analog headset switches, DTS:2097-2240), MAD and the "listen" paths
(MP:2024-2027), AANC, the codec DSP (WDSP, `wcd_spi`, dmesg:2630-2635), VBAT calibration (Android had none either,
dmesg:3429), efuse sensing (only tells WCD9340 from WCD9341), digital-core power collapse (we never collapse it), class-H
and every RX path. AFE `CDC_REGISTERS_CONFIG`, `CDC_REGISTER_PAGE_CONFIG` and `SLIMBUS_SLAVE_CONFIG` are only required for
HW MAD (WH q6afe.c:3024-3043); send SLIMBUS_SLAVE_CONFIG anyway (4.1), it costs one command.

## 4. ADSP side

APR as TA:1577-1749 (domains APPS 5 / ADSP 4; AFE 4, ASM 7, ADM 8). IDs from WH apr_audio-v2.h unless noted. The capture
gets **ASM session 2, stream 1: port 0x0201** (playback holds session 1, TA:2117; HK hakios.mic.c:1259-1260).

### 4.1 AFE: SLIMBUS_0_TX 0x4001

| # | Command | Payload |
|---|---|---|
| 0 | `AFE_SVC_CMD_SET_PARAM` 0x000100F3, port 0 (optional, once) | `{payload_size 28, 0, 0, 0}` + `{module AFE_MODULE_CDC_DEV_CFG 0x00010234, param AFE_PARAM_ID_CDC_SLIMBUS_SLAVE_CFG 0x00010235, u16 16, u16 0}` + `{minor 1, enum_addr_lsw 0x02500100, msw 0x00000217, u16 tx_port_offset 0, u16 rx_port_offset 16}` (WH wcd934x.c:8854-8870, msm8998.c:3337-3385) |
| 1 | `AFE_PORT_CMD_SET_PARAM_V2` 0x000100EF, port 0 | `{u16 port 0x4001, u16 payload_size 36, 0, 0, 0}` + `{module 0x0001020C, param AFE_PARAM_ID_SLIMBUS_CONFIG 0x00010212, u16 24, u16 0}` + `{u32 minor 1, u16 slimbus_dev_id 0 (DEVICE_1), u16 bit_width 16, u16 data_format 0, u16 num_channels 1, u8 shared_ch_mapping[8] = {135, 0...}, u32 sample_rate 48000}` (apr_audio-v2.h:2394-2467; msm-dai-q6-v2.c:1561-1620) |
| 2 | `AFE_PORT_CMD_DEVICE_START` 0x000100E5 | `{u16 0x4001, u16 0}`; stop 0x000100E6 |

No clock command: a SLIMbus port has no MI2S clock; the bus clock is the ADSP's. Answers are APR_BASIC_RSP_RESULT
0x000110E8 {opcode, status}.

### 4.2 ADM: a TX COPP

`ADM_CMD_DEVICE_OPEN_V5` 0x00010326 to ADM, APR port 0x4001 (as TA:2252 does for 0x1006):
`{u16 flags 0 (legacy), u16 mode 2 (TX live, ADM_CMD_COPP_OPEN_MODE_OF_OPERATION_TX_PATH_LIVE_COPP), u16 endpoint1 0x4001,
u16 endpoint2 0xFFFF, u32 topology 0x00010312 (NULL_COPP), u16 channels 1, u16 bits 16, u32 48000, u8 map[8] = {3 (FC)}}`
→ `ADM_CMDRSP_DEVICE_OPEN_V5` 0x00010329 `{status, copp_id}`. Mono maps to FC (WH q6adm.c:2005-2006). Routing:
`ADM_CMD_MATRIX_MAP_ROUTINGS_V5` 0x00010325, port 0, `{u32 matrix 1 (AUDIO_TX), u32 sessions 1, u16 session 2, u16 copps 1,
u16 copp_id, u16 0}` (apr_audio-v2.h:37-48; L q6adm.c:493-498). Close: `ADM_CMD_DEVICE_CLOSE_V5` 0x00010327, dest the COPP.

### 4.3 ASM: a read stream

| # | Command (to 0x0201 unless said) | Payload | Source |
|---|---|---|---|
| 1 | `ASM_CMD_SHARED_MEM_MAP_REGIONS` 0x00010D92, port 0 | pool 3, 1 region, `{lsw, msw, 0x8000}`: 8 periods x 4 KB from the pool, DSP address `pa \| 1<<32` (TA:2169-2175) → 0x00010D93 {handle} | TA:2228-2238 |
| 2 | `ASM_STREAM_CMD_OPEN_READ_V3` 0x00010DB4 | `{mode_flags 0 (legacy, no metadata, no timestamps), src_endpoint 0 (matrix), preproc topo 0x00010C68 (none), enc_cfg_id 0x00010DA5 (PCM v2), u16 bits 16, u16 0}` | L q6asm.c:1500-1535; apr_audio-v2.h:5867 |
| 3 | `ASM_STREAM_CMD_SET_ENCDEC_PARAM` 0x00010C10 | `{param_id ENC_CFG_BLK_V2 0x00010DA3, param_size 28, frames_per_buf 0, enc_cfg_blk_size 20, u16 ch 1, u16 bits 16, u32 48000, u16 signed 1, u16 0, u8 map[8] = {3 (FC)}}` | L q6asm.c:1396-1440; HK hakios.mic.c:1349 |
| 4 | ADM open + matrix map (4.2) | | |
| 5 | `ASM_SESSION_CMD_RUN_V2` 0x00010DAA | `{0, 0, 0}` | |
| 6 | `ASM_DATA_CMD_READ_V2` 0x00010DAC, x 8 | `{lsw, msw, handle, 1920 (20 ms mono), seq}`, APR token `0x40000000 \| period` | HK hakios.mic.c:1299-1305 |
| 7 | `ASM_DATA_EVENT_READ_DONE_V2` 0x00010D9A | `{status, lsw, msw, handle, size, offset, ts_lsw, ts_msw, flags, num_frames, seq}`: the samples are `size` bytes at buffer + `offset`; re-send READ for that period | apr_audio-v2.h:5234-5320; WH msm-pcm-q6-v2.c:183-200 |
| - | `ASM_SESSION_EVENT_TX_OVERFLOW` 0x00010C18 | count it (we were late) | apr_audio-v2.h:5585 |
| close | `ASM_STREAM_CMD_CLOSE` 0x00010BCD, then ADM close, `..._UNMAP_REGIONS` 0x00010D94 | | TA:2328-2334 |

- READ_DONE arrives on the GLINK thread: extend `asm_event()` (TA:2205-2222) to take session 2's opcodes by token, as
  HK `mic_events()` does (hakios.mic.c:1274-1296), and mark the period ready.
- The pool is mapped write-combined (TA:240-257): no cache maintenance, a `dmb` before reading a returned period. HK
  invalidates (`dc ivac`) because its memory is cacheable.
- The ADSP *writes* through stream 1 here; playback only proved reads. The identity bank (CB1, SCTLR.M = 0) does not
  distinguish; log FSR/FAR after the first session (`lpass_smmu_faults`, TA:1926).

### 4.4 Session order and teardown

Up (Android's order: BE prepare = AFE start before the codec's DAPM widgets and slim channel; HK's for the stream):
codec MCLK and TX path muted (3.3) → ASM map, OPEN_READ, ENC_CFG → ADM open, matrix → AFE config + DEVICE_START →
SLIMbus port registers, CONNECT_SRC, DEF_ACT_CHAN, RECONFIG_NOW (2.7) → RUN, 8 READs → unmute at 40 ms → HPF at 300 ms.
Down, in reverse: decimator mute → ASM CLOSE → ADM CLOSE → channel remove + RECONFIG_NOW → AFE DEVICE_STOP → codec down,
MCLK off → unmap. The NGD and the codec's reset stay up between sessions; POWER(ACTIVE) is not withdrawn.

## 5. `/dev/redwood-mic`

The contract (RA:829-880, AP:118-144): the plug-in opens `/dev/redwood-mic` `O_RDONLY | O_NONBLOCK` when the "Codec" input
starts (`start_io`, RA:883-905) and closes it when it stops; each I/O cycle `read(fd, buf, frames * 2)` must return at once
with what has come, **s16le, mono, 48 kHz**; a short read is padded with silence; there is no ioctl. HK hakios.mic.c:1450-1540
is the same ABI (`/dev/hakios-mic`), proven with this userland. Copy it:

- **Node:** `devfs_make_node` "redwood-mic", 0666, made at audio-thread start (before mediaserverd's HAL init, like
  `audio_dev_init`, TA:2417) so the plug-in finds it.
- **Ring:** 16384 frames (341 ms) of int16; the DSP side (`ring_put` from each READ_DONE) writes, one reader.
- **read():** never waits. More than **100 ms** (4800 frames) waiting: skip to **40 ms** (1920) behind the writer and count the
  skip. Until 40 ms have gathered after a start or a dry read, return 0 (priming). Then return min(asked, available) whole
  frames by `uiomove`. Count dry reads.
- **open()/close():** open sets `mic_wanted` (no waiting, the plug-in holds its lock in `start_io`); the last close clears it.
  The audio thread's loop (TA:2669-2756) brings the session up when wanted and down when not (4.4), so the microphone listens
  only while the device is open. After 3 sessions that broke off, stop trying until reboot (HK :1755-1790).
- **Formats:** we ask the DSP for exactly what iOS wants (16-bit, 1 channel, 48 kHz), so no conversion. If
  `taimenmicbits=24` is ever needed, PCM v2 24-bit comes as 32-bit words, LSB-aligned (assumed from WH msm-pcm-q6-v2's
  S24_LE handling): `s16 = clamp(s32 >> 8)`. For the census (2+ channels) take one channel out of the interleave.
- **Gain:** codec digital gain `taimenmicgain` (dB, -84..+40, default +4 = Android's `handset-mic`); no kernel scaling.
- **Logging:** per second for the first 3 s of a session, then every 30 s: frames, empty periods, level dBFS (rms), peak,
  mean, reads, TX overflows; at close: frames, skipped, dry reads (HK :1660-1680).

## 6. Milestones (boot-arg `taimenmic=N`)

**Why a new boot-arg:** `taimenaudio=9` is the everyday default and means "stages 1..9"; numbering mic stages 10+ would put
them on every default boot. The mic stages are orthogonal to the amps and the feeder, and their failures must not stop the
speaker. So `taimenmic=N` (default 0) runs mic stages 1..N inside the audio thread once M4 of the audio plan is reached
(TA:2680-2737): it needs `taimenaudio>=4` for M1-M5 (GLINK, QRTR, APR), `>=5` with the stream 1 bank for M7 and `>=7`
(the amps, for the test tone) for M6; the default 9 has all of it. Helpers:
`taimenmicdmic` (default 0), `taimenmicgain` (4), `taimenmicsecs` (M6 length, default 8), `taimenmicla5=0` (send the full LA
in DEF_ACT_CHAN byte 0 instead of `LA & 0x1F`, for a NACK). Prefix every line `TAIMEN MIC:`. All waits time out.

| M | taimenmic | Proves | Success line(s) |
|---|---|---|---|
| 1 | 1 | QMI and NGD up, capability exchanged | `1: census: tlmm64 cfg 0x.. io 0x.., tlmm54 cfg 0x.., pm8998 gpio13 subtype 0x.. mode 0x.. src 0x.. en 0x..` / `1: slimbus QMI 0x301 v1 inst 0 at node 5 port 0x..` / `1: SELECT_INSTANCE(0, MASTER) -> 0/0` / `1: POWER(ACTIVE) -> 0/0 in N ms` / `1: NGD hw 0x....; status 0x.. cfg 0x.. int_en 0x..` / `1: M1 NGD enabled (register mode); LADDR after N ms; MASTER_CAPABILITY (RL n); REPORT_SATELLITE sent` |
| 2 | 2 | codec on the bus | `2: codec reset (gpio64 was in/out 0x..): low 20 ms, high 20 ms` / `2: ADDR_QUERY 00 01 50 02 17 02 -> LA 0xcf (try n)` / `... 00 00 50 02 17 02 -> LA 0xce` / `... 00 01 a0 01 17 02 -> absent` / `2: M2 the WCD9340 is on the bus: generic 0xcf, interface 0xce` |
| 3 | 3 | codec registers | `3: chip id 01 00 08 01: WCD9340 v1.1` / `3: bring-up read back: RST_CTL 0x07 DIG_HM_CTL 0x03` / `3: interface 0x9f7 wrote 0x02 read 0x02` / `3: M3 N reads, N writes, 0 NACKs, 0 timeouts` |
| 4 | 4 | MCLK and the TX path set | `4: DIV_CLK1 (clka 0xb) on: ok; pm8998 gpio13 mode 0x01 src 0x03 en 0x80` / `4: defaults 36+24+6, CPR 16: ok` / `4: clock: ANA_BIAS 0x80 MCLK_PRG 0x81 MCLK_CONTROL 0x01` / `4: TX7: DMIC0 -> DEC7 -> TX7, MICB1 0x50, DMIC0_CTL 0x05, TX7 CTL 0x34 CFG0 0x50 VOL 0x04` / `4: M4 N registers read back as written` (then everything down again) |
| 5 | 5 | SLIMBUS_0_TX and channel 135 | `5: AFE slave cfg 0; SLIMBUS_0_TX cfg (16 bit, 1 ch, ch 135, 48000) 0; DEVICE_START 0` / `5: ifd port 7: 0x02 0x80 0x00 0x05` / `5: CONNECT_SRC 0xcf:7 -> 135 ack 0x20; DEF_ACT_CHAN 0f 24 20 83 ack 0x20; RECONFIG_NOW ack 0x20` / `5: M5 channel 135 active 2 s: codec port 7 status 0x.. (no overflow), NGD int 0x..` |
| 6 | 6 | samples reach our memory, the mic hears | `6: map 32 KB @0x8a0..000 (DSP 0x18a0..000) handle 0x..; ADM 0x4001 TX copp N; OPEN_READ 0, ENC_CFG 0, MATRIX 0, RUN 0` / per second `6: s1 DMIC0 50 periods 48000 frames 0 empty, -58.2 dBFS peak 310, 1 kHz -71 dB` ... / `6: M6 the microphone hears the speaker: DMIC0 1 kHz +38 dB over quiet; DMIC1 +37; DMIC2 +21; DMIC4 +18; 400 reads, 0 errors, 0 overflows` |
| 7 | 7 | iOS records | `TAIMEN MIC: /dev/redwood-mic ready` / mediaserverd (`sweetlogp=mediaserverd`): `redwood: audio: the microphone is listened to (/dev/redwood-mic: open)` / `7: iOS records (1): up in N ms` / `redwood: audio: the microphone's first samples, 0.xx s after the input started` / at stop: `redwood: audio: the microphone stops: X s asked for, Y s of it from the kernel (peak P ...)` and `7: recording 1 ended: N frames, N skipped, N dry` |

How to tell success from failure, per milestone:
- **M1.** QMI error → the TLV 0x02 code. No LADDR in 1 s → log CFG/STATUS/INT_STAT and suspect register mode (risk 1).
  LADDR but no MASTER_CAPABILITY → RX in register mode is wrong (INT_STAT bit 30 never set): try reading NGD_RX_MSG on
  bit 28 or a changed word 0. TX bits 25-29 → print them; 27 MSG_TX_INVAL = a malformed first word.
- **M2.** All-zero EA in ADDR_REPLY = codec absent: check gpio64 high, S4 (vote `smpa` 4 if needed), MCLK is *not* needed.
  The tasha query must say absent; if it answers, the EA bytes are mis-ordered.
- **M3.** REPLY_VALUE timeout → element-code encoding; chip id not `01 00 08 01` → wrong LA (interface vs generic) or page.
- **M4.** A register that does not read back → stop and log. No audio proof yet; M6 is the proof.
- **M5.** A GENERIC_ACK without 0x20 → log the code; retry DEF_ACT_CHAN with `taimenmicla5=0`. AFE start != 0 → the slimbus
  config payload. Codec port 7 overflow bit (interface 0x87 `PGD_PORT_INT_STATUS(7)`, L registers.h:577) = data produced,
  nobody consuming.
- **M6** needs **no listener**: while recording, the test plays the audio plan's 1 kHz tone at -20 dBFS (stage 8's loop,
  TA:2224-2335, its 3 s made 5 s) and, after one quiet second, steps `DMIC MUX7` through DMIC0, DMIC1, DMIC2, DMIC4 (one
  second each; MIC BIAS1/3/4 and the DMIC0-2_CTL clocks on). READ_DONE is handled in `asm_event`, so the capture runs
  inside stage 8's GLINK polling. Measure each 20 ms period's RMS and a Goertzel bin at 1 kHz (k = 20 of 960). Pass: ≥ 300 READ_DONE with status 0 and size 1920, samples neither all zero nor constant, and
  the bottom mic's 1 kHz bin ≥ 20 dB over the quiet second. Fail signs: no READ_DONE (stream or AFE), all zeros (channel 135
  carries nothing: M5 or the mux), constant non-zero (DMIC edge with no mic: try the other), noise without the tone
  (wrong mic or gain). Optional, short: `tools/taimen/attention.sh "say a word to the bottom of the phone"` with a 15 s
  window, judged by the per-second peak.
- **M7.** Voice Memos through `phonectl.py tap` (record 5 s, stop, play back); the microphone TCC prompt must be allowed
  (Microphone was denied to unentitled tcctest, NOTES:1056-1069; Voice Memos asks through the prompt). Pass: the plug-in's
  first-samples line within 0.5 s, its stop line with Y ≈ X and peak > 0, the session down after the stop, the memo's length
  > 0. Then make `taimenmic=7` the default (M8), with the speaker untouched.

## 7. Risks and unknowns, ranked

1. **NGD register-mode messaging.** Android ran both message queues through the BAM (dmesg:2265). Register TX and RX exist
   in the same driver (`msm_send_msg_buf`, `qcom,rxreg-access`), but this ADSP build was never seen with them on this phone.
   If M1 fails there, the fallback is the BAM (0x17184000; the APPS pipes in mask 0x1F80; DMA through an unknown SMMU path,
   since the slim nodes have no `iommus`), which is a BAM driver of its own (~300 lines) and an SMMU question.
2. **LPASS register access and clock pause.** The NGD block is the ADSP's to clock. Reading it before POWER(ACTIVE), or
   after the ADSP pauses the bus clock, may abort or hang the interconnect (HackiOS saw aborts on LPASS clock registers,
   AP:366; boot 157's SMMU hang, NOTES:1122-1128). Mitigation: first access only after the POWER response, never INACTIVE,
   re-send POWER(ACTIVE) before each session, NGD reads only in the audio thread, a log line before the first read.
3. **The codec MCLK.** DIV_CLK1 through `sweet_rpm_xo_buffer(0xb)` is unproven on this RPM (the call was written for the
   msm8996 LN_BB id 8), and PM8998 GPIO13 must be in func2. Without it the codec's digital path is dead, which only shows at
   M6 (zeros). M1's census reads GPIO13; M4 logs the RPM ack. Fallback: LN_BB_CLK2 (`clk_ln_bb_clk2`, the disabled
   `audio_ext_clk_lnbb` node, DTS:15887-15894) is 19.2 MHz and would need the ÷2 path, so it is not a drop-in.
4. **Channel activation details.** DEF_ACT_CHAN's first byte (`LA & 0x1F` downstream vs the full LA upstream), the presence
   rate 0x83 and coefficient bits are computed, not observed. A NACK names the problem; both forms are one boot-arg apart.
5. **Which DMIC edge.** Factory paths say DMIC0, everyday paths DMIC1 (1.1). M6's census settles it in one boot.
6. **The QMI handshake.** SELECT_INSTANCE may want the PDR/servreg state we do not serve; B156 shows 0x301 announced while
   our pd-mapper answered `avs/audio` with no domains, as Android's own lookup did (dmesg:3020), so this is low.
7. **SMMU writes from the DSP.** Stream 1's identity bank has only carried DSP reads (playback). Low; faults are logged.
8. **iOS side.** The microphone TCC prompt and VirtualAudio's input route for 'imic' (HK ran the same plug-in, so low);
   Camera has no driver, so video sound is out of scope.
9. Codec IRQ (gpio54) unused: SLIM port errors are not signalled; polled at M5 only. Low.
10. Two ASM sessions at once (playback 1, capture 2) and two AFE ports: independent on the ADSP. Low.

Could not determine from the sources: the NGD HW version value; whether ABL leaves gpio64 high and GPIO13 in func2; whether
INT_STAT bit 30 latches with INT_EN bit 30 clear; whether the ADSP clock-pauses the bus on its own while APPS is ACTIVE.

## 8. Files and estimate

| File | New/changed | What | Lines |
|---|---|---|---|
| X/bsd/dev/taimen_slim.c | new | NGD map, register-mode TX/RX, TID table, capability, ADDR_QUERY, value read/write with pages, CONNECT_SRC / DEF_ACT_CHAN / RECONFIG_NOW / remove / disconnect | ~450 |
| X/bsd/dev/taimen_wcd934x.c | new | reset gpio64, GPIO13 + DIV_CLK1, bring-up, defaults, CPR, MCLK, TX path up/down, ifd port registers, census mux | ~400 |
| X/iokit/Kernel/arm/TaimenAudio.cpp | changed | QMI client for 0x301 (QRTR DATA dispatch to our port), `taimenmic` ladder, AFE SLIMBUS_0_TX, ADM TX, ASM session 2 reads, READ_DONE in `asm_event`, ring + `/dev/redwood-mic`, levels and Goertzel | ~500 |
| X/bsd/conf/files | changed | the two new files (TAIMEN only) | 2 |
| port/xnu/patches/03xx | new | one per milestone | |

About 1,350 lines, no new firmware, no plug-in change. Realistic: **3-6 working days, 12-25 boots**, no hand-off before M7
(M6 hears the speaker). If M1 fails in register mode, add 2 days for the BAM path.

## 9. Cheat sheet

| Item | Value |
|---|---|
| SLIMbus | NGD block 0x171C0000 (+0x2C000), version = reg0 >> 16, **NGD1 regs 0x171C1000**: CFG 0x0 (=1), STATUS 0x4 (b1 LADDR), RX_MSGQ_CFG 0x8, INT_EN/STAT/CLR 0x10/0x14/0x18 (b30 RX, b31 TX sent, b25-29 errors), TX_MSG 0x30, RX_MSG 0x70; SPI 163; BAM 0x17184000 unused |
| QMI | svc 0x301 v1 inst 0 (B156: node 5 port 0xb); SELECT_INSTANCE 0x20 {0x01 u32 0, 0x10 u32 2}; POWER 0x21 {0x01 u32 2}; resp TLV 0x02 |
| Messages | word0 = RL \| MT<<5 \| MC<<8 \| LA<<16, RL = 3 + payload; user MT 2 to LA 0xFF, replies MT 6; MASTER_CAP 0x00, REPORT_SAT 0x01 (D9 C5 01 01), ADDR_QUERY 0x0D / REPLY 0x0E, DEF_ACT 0x21, CHAN_CTRL 0x23, RECONFIG 0x24, GENERIC_ACK 0x25 (b5 ok), CONNECT_SRC 0x2C, DISCONNECT 0x2E; core REQUEST_VALUE 0x60 / REPLY 0x64 / CHANGE_VALUE 0x68, ec = slice \| 8 \| elem<<4 |
| WCD9340 | EA generic 00 01 50 02 17 02 → LA 0xCF, interface 00 00 50 02 17 02 → 0xCE; chip id 0x21-0x24 = 01 00 08 01; element 0x800 + offset, page reg 0x00; reset TLMM gpio64 (0x03540000); IRQ gpio54; supplies pm8998_s4 1.8 V; MCLK 9.6 MHz = RPM clka 0xB → PM8998 GPIO13 func2 |
| TX path | DMIC0 (or 1) → ADC_MUX7_CFG0 0x0D29 → DEC7 (TX7_TX_PATH_CTL 0x0AA1: rate 4, b4 mute, b5 clock; CFG0 0x0AA2 HPF; VOL 0x0AA4 = 0x04; SEC2 0x0AA9) → IF_ROUTER_TX_MUX_CFG1 0x0D3B = DEC7 → TX port 7; MICB1 0x0622 = 0x50; DMIC0_CTL 0x0218 = 0x05 |
| Port 7 (ifd) | 0x1F7 = 0x02, 0x11C = 0x80, 0x11D = 0, 0x057 = 0x05; channel **135**; DEF_ACT `0F 24 20 83` |
| AFE | SLIMBUS_0_TX **0x4001**; SET_PARAM_V2 0x000100EF, module 0x0001020C, param 0x00010212 {1, dev 0, 16, 0, 1 ch, map {135}, 48000}; START 0x000100E5, STOP 0x000100E6; slave cfg 0x00010234/0x00010235 |
| ADM | OPEN_V5 0x00010326 (mode 2, ep 0x4001, topo 0x00010312, 1 ch FC, 16, 48000) → 0x00010329; MATRIX_MAP 0x00010325 (matrix 1, session 2); CLOSE 0x00010327 |
| ASM | port 0x0201; OPEN_READ_V3 0x00010DB4 (topo 0x00010C68, fmt 0x00010DA5, 16); SET_ENCDEC 0x00010C10 (ENC_CFG_BLK_V2 0x00010DA3: 1 ch, 16, 48000, FC); RUN 0x00010DAA; READ_V2 0x00010DAC (1920 B, token 0x40000000\|i) → READ_DONE 0x00010D9A; TX_OVERFLOW 0x00010C18; CLOSE 0x00010BCD |
| iOS | `/dev/redwood-mic`, 0666, read() s16le mono 48 kHz, never blocks; ring 16384, keep 1920, skip above 4800; plug-in device "Codec", source 'imic' |
