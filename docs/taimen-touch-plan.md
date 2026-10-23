# Taimen touch: bare-metal ST FTM4 driver plan (2026-10-23)

Plan for an XNU driver for the Pixel 2 XL touch controller, ST FTM4 (`stm,ftm4_fts`) at I2C 0x49 on
BLSP1 QUP5 (`i2c@c179000`). It is written so the driver can be built from it without more research.
Nothing here has been run on the phone. Items marked **(unverified)** are inferences or come from a
source that does not fully settle them.

## 0. Short answers

- **Firmware: no host download.** The FTM4 runs from its own flash. Linux's probe has the
  firmware update commented out (`ftm4_ts.c:800-805`). An update happens only when userspace writes
  the `check_fw`/`upgrade` sysfs nodes (`ftm4_pdc.c:209-257`), and the wahoo `init.hardware.rc`
  never does (`init.hardware.rc:412-413` only chowns the `cmd` node). `stm,firmware_name =
  "ftm4_fw.ftb"` (live.dts:14891) is the image for that optional path. The XNU driver should only
  detect a missing or corrupt firmware (chip-id bytes 5/6 = 0, or error event 0x0F 0x03) and log it,
  never flash.
- **Controller:** BLSP1 QUP5 at 0x0C179000, not a BLSP2 QUP. Pins: SDA GPIO 87, SCL GPIO 88, reset
  GPIO 89, attention GPIO 125, I2C switch GPIO 75 (0 = AP, 1 = SLPI), charger hint GPIO 50.
- **Rails:** PM8998 L28 3.3 V (AVDD) then L6 1.8 V (DVDD), both RPM `ldoa` resources (ids 28 and 6).
  The vote uses the same GLINK RPM transport as the Pixel 1, with different addresses.
- **Clocks:** GCC 0x100000. BLSP1 AHB is voted with bit 17 of 0x152004 (status in 0x117004). The QUP5
  I2C core branch is CBCR 0x121008 and its RCG is 0x121020/0x121024 at XO 19.2 MHz.
- **Protocol:** byte commands plus `B6 hi lo` register access. Events are 8 bytes each and come from a
  FIFO of up to 32: count at register 0x0023, events read with 0x86 (all) or 0x85 (one). Touch events
  are id 3 enter, 4 leave and 5 motion, with x/y in 12 bits. There is no explicit interrupt ack: the
  line deasserts once the FIFO is empty.

## 1. Sources and citation keys

| Key | Source |
|---|---|
| **G** | Google `kernel/msm` branch `android-msm-wahoo-4.4-android11`, `drivers/input/touchscreen/stm/ftm4_ts.c` / `ftm4_ts.h`: https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-wahoo-4.4-android11/drivers/input/touchscreen/stm/ftm4_ts.c |
| **L** | LineageOS mirror of the same kernel, branch `lineage-22.2`: https://github.com/LineageOS/android_kernel_google_wahoo/blob/lineage-22.2/ (files `drivers/input/touchscreen/stm/{ftm4_ts.c,ftm4_ts.h,ftm4_fwu.c,ftm4_pdc.c}`, `drivers/pinctrl/qcom/{pinctrl-msm.c,pinctrl-msm8998.c}`, `drivers/clk/msm/clock-gcc-8998.c`, `include/dt-bindings/clock/{msm-clocks-8998.h,msm-clocks-hwio-8998.h}`, `drivers/i2c/busses/i2c-msm-v2.c`, `arch/arm64/boot/dts/lge/{msm8998-taimen-touch-stm-ftm4.dtsi,msm8998-taimen-touch-sic-sw49408.dtsi,overlay-taimen-rev-1_0.dts}`). `ftm4_ts.c` and `ftm4_ts.h` are byte-identical in G and L (diffed 2026-10-23), so the line numbers below hold for both. |
| **D** | LineageOS `android_device_google_wahoo` `lineage-22.2`, `init.hardware.rc`: https://github.com/LineageOS/android_device_google_wahoo/blob/lineage-22.2/init.hardware.rc |
| **M** | Mainline Linux `master` as fetched 2026-10-23 (line numbers can drift): https://github.com/torvalds/linux/blob/master/ `drivers/clk/qcom/gcc-msm8998.c`, `arch/arm64/boot/dts/qcom/msm8998.dtsi`, `drivers/i2c/busses/i2c-qup.c`, `drivers/rpmsg/qcom_glink_rpm.c`, `drivers/mailbox/qcom-apcs-ipc-mailbox.c`, `drivers/regulator/qcom_smd-regulator.c` |
| live.dts | `research/taimen/live.dts` (this phone's DT as patched by ABL, dtbo entry 12) |
| local | `scratch/xnu/...` files of this repo |

## 2. Hardware facts

### 2.1 I2C controller and device

| Item | Value | Source |
|---|---|---|
| Controller | `i2c@c179000`, `qcom,i2c-msm-v2`, reg 0x0C179000 + 0x600, status ok | live.dts:14850-14869 |
| Which QUP | BLSP1 QUP5 (BLSP1 QUPs are 0xC175000..0xC17A000, BLSP2 are 0xC1B5000..0xC1BA000; alias i2c5) | live.dts:14766-14895, 16310; M msm8998.dtsi:2392-2410 (`blsp1_i2c5`, `qcom,i2c-qup-v2.2.1`) |
| QUP IRQ | SPI 99 = INTID 131 (not needed: polled transfers) | live.dts:14857 |
| Bus clock | out 400 kHz (0x61A80), in 19.2 MHz (0x124F800) | live.dts:14861-14862 |
| Same QUP as SPI | `spi@c179000` exists but is disabled (rev-1_0 overlay disables `spi_5`, enables `i2c_5`) | live.dts:15352-15372; L overlay-taimen-rev-1_0.dts |
| BAM | BLSP1 BAM 0xC144000, pipes 14/15 (not needed: FIFO mode) | live.dts:14750-14757, 14858 |
| Touch device | `ftm4_fts@49`, `stm,ftm4_fts`, 7-bit address 0x49 (8-bit 0x92 write / 0x93 read) | live.dts:14873-14876 |
| Coordinates | `stm,max_coords = <1439 2879>` | live.dts:14886 |
| Lines | `stm,num_lines = <32 16>` (sense, force) | live.dts:14885 |
| Interrupt | `interrupt-names = "tp_direct_interrupt"`, `interrupts = <0 216 0>` = GIC SPI 216 = INTID 248; `stm,irq_type = 0x2004` = IRQF_ONESHOT \| IRQF_TRIGGER_HIGH | live.dts:14877-14878, 14884 |
| Supplies | `stm,regulator_dvdd = "pm8998_l6"`, `stm,regulator_avdd = "pm8998_l28"` | live.dts:14887-14888 |
| `power-supplies` | phandle 0x24E = `qcom,qpnp-smb2` charger, used only for the charger notice (not a rail) | live.dts:14892, 1382-1393 |
| Other controller | The taimen kernel DT also has an LG SiW SW49408 node on `spi@c179000` (same QUP, same reset/irq GPIOs). Which board revisions use it is **unverified**. This phone's DT disables that SPI node and enables FTM4. | L msm8998-taimen-touch.dtsi, msm8998-taimen-touch-sic-sw49408.dtsi; live.dts:14873-14875, 15372 |

### 2.2 TLMM pins

MSM8998 TLMM base 0x03400000, size 0xC00000 (live.dts:8926-8934). It is **not** one uniform
0x1000-per-GPIO array from the base like MSM8996's 0x1010000. Each GPIO lives in one of three tiles,
`ctl_reg = tile + 0x1000 * n` with tiles NORTH 0x500000, WEST 0x100000 and EAST 0x900000
(L pinctrl-msm8998.c:28-31, 50-54). Per-GPIO registers: CFG +0x0, IN_OUT +0x4 (bit 0 in, bit 1 out),
INTR_CFG +0x8, INTR_STATUS +0xC. CFG bits: pull 1:0 (0 none, 1 down, 2 keeper, 3 up), function 5:2,
drive 8:6 = mA/2 - 1, OE bit 9. INTR_CFG bits: enable 0, polarity 1, detection 3:2, raw status 4,
target 7:5 (KPSS = 3), direct-connect enable 8 (L pinctrl-msm8998.c:55-69). This is the same layout
`sweet_touch.c:175-184` already writes.

| GPIO | Role | Tile | Register PA | DT state used by Linux | CFG to write | Source |
|---|---|---|---|---|---|---|
| 87 | SDA | EAST | 0x03D57000 | `i2c_5_active`: func `blsp_i2c5` (= 3), 2 mA, bias-disable | 0x00C | live.dts:9066-9078; L pinctrl-msm8998.c:1801 (funcs blsp_spi5=1, blsp_uart2_b=2, blsp_i2c5=3) |
| 88 | SCL | EAST | 0x03D58000 | same | 0x00C | live.dts:9066-9078; L pinctrl-msm8998.c:1803 |
| 89 | reset (active low) | EAST | 0x03D59000 | `ts_active`: gpio, 16 mA, pull-up; Linux drives it output | 0x3C3 (OE, pull-up, 16 mA); IN_OUT bit 1 = level | live.dts:10555-10567 (phandle 0x354 = `pinctrl-0`), 14889; L pinctrl-msm8998.c:1805 |
| 125 | attention (active low) | EAST | 0x03D7D000 (INTR_CFG 0x03D7D008, STATUS 0x03D7D00C) | `ts_active`: gpio, 16 mA, pull-up, input | 0x1C3 | live.dts:10555-10567, 14883; L pinctrl-msm8998.c:1858 |
| 75 | I2C switch: 0 = AP, 1 = SLPI | WEST | 0x0354B000 | no touch pinctrl; Linux `gpio_request_one(OUTPUT_LOW)` | 0x201 (OE, pull-down, 2 mA), IN_OUT = 0 | live.dts:14882; G ftm4_ts.c:1790-1807, 2469-2473, 2493-2497; L pinctrl-msm8998.c:1780 |
| 50 | charger ("TA") detect hint to the IC | NORTH | 0x03932000 | Linux `OUTPUT_LOW`, then = USB online | 0x201, IN_OUT = 0 (optional) | live.dts:14890; G ftm4_ts.c:114-128, 1648-1660; L pinctrl-msm8998.c:1734 |

- The sleep states (`ts_suspend`: 89/125 pull-down, 2 mA, live.dts:10592-10603; `i2c_5_sleep`) are not
  needed. The `pmx_ts_reset`/`pmx_ts_int` nodes (live.dts:11956-12003) have no phandle and are unused.
- GPIO 75 also appears in the generic audio states `wcd_gnd_mic_swap`, `tert_aux_pcm` and `tert_mi2s`
  (live.dts:9522-9549, 11078-11104, 11515-11540). On taimen the touch driver owns it; XNU starts no
  audio, so there is no conflict.
- GPIO 50 also appears in `spi_9` and `blsp2_uart3` states (live.dts:9952-9976, 10460-10483). The
  only user, `uart@c1b1000`, is disabled (live.dts:15718-15743).
- I2C bus recovery pins: `qcom,i2c-clk = gpio88`, `qcom,i2c-dat = gpio87`, `i2c_5_bitbang` = gpio
  function, 8 mA, no pull (live.dts:14870-14872, 9092-9103).
- SDA/SCL pull-ups are external (bias-disable). Pixel 1 found them reading 0 while the touch rails
  were off (`sweet_touch.c:11-13`). Expect the same here: idle SDA/SCL reading 1 is the first sign that
  L6 is on and GPIO 75 points at the AP **(unverified for taimen)**.

### 2.3 Interrupt routing (two options)

**A. TLMM summary interrupt (recommended first; it is what `sweet_touch.c` already does).** The
TLMM summary line is SPI 208 = INTID 240 (live.dts:8929), the same INTID as on the Pixel 1
(`sweet_touch.c:131-139`). GPIO 125 INTR_CFG (0x03D7D008) = 0x70 masked or 0x71 armed: target KPSS
(3<<5), raw-status enable (1<<4), level detection (0<<2), polarity low (0<<1), enable bit 0, and
**direct-connect bit 8 clear**. Clear INTR_STATUS (0x03D7D00C) by writing 0. The existing handler
`touch_irq`/`touch_irq_wait` (`sweet_touch.c:145-173`) carries over unchanged apart from the address.

**B. Direct connect, as Linux does it.** `msm_gpio_install_direct_irq(125, 0, 0)` (G ftm4_ts.c:1942)
does three things (L pinctrl-msm.c:1083-1105):
1. Sets OE (bit 9) in GPIO 125's CFG **(unexplained: OE on an input pin; do not copy this at first,
   only if option B gets no interrupts)**.
2. Writes INTR_CFG = (old & ~(7<<5 | 1<<4 | 1<<0)) | (3<<5) | (1<<8), which comes to 0x160 from a
   clean register.
3. Writes the direct-connect slot register `TLMM + 0x9A1000 + 4*slot` = 0x03DA1000 (slot 0) = 125, with
   polarity bit 8 = 0 (L pinctrl-msm8998.c:1900-1901: offset 0x9a1000, 8 slots).

Linux then requests GIC SPI 216 level-high (`stm,irq_type` 0x2004). So slot 0 drives SPI 216, and
polarity 0 inverts the active-low pin into an active-high level at the GIC **(inference: the mapping
slot 0 -> SPI 216 and the meaning of bit 8 are not stated anywhere in the sources)**. In XNU:
`sweet_spi_attach(248, false /* level */, ...)` (local `pexpert/arm/pe_fiq.c:152-172`), mask with
`sweet_spi_mask(248, true)` in the handler, and unmask after the FIFO is drained. There is no TLMM
status bit to clear in this mode **(unverified)**.

### 2.4 Power rails (RPM)

| Rail | RPM resource | Voltage | Set | Source |
|---|---|---|---|---|
| AVDD | `ldoa` (0x616f646c) id 28 (0x1C) | 3.3 V (0x325AA0) | `qcom,set = <3>` (active and sleep) | live.dts:3392-3408 |
| DVDD | `ldoa` id 6 | 1.8 V (0x1B7740) | `qcom,set = <3>` | live.dts:2983-2999 |

- Both regulators have no phandle consumers. Linux gets them by name (`regulator_get(NULL, name)`, G
  ftm4_ts.c:1426, 1437), so nothing else is known to keep them on. Whether ABL leaves them on is
  **unknown**: vote anyway, since the vote is idempotent.
- Linux enable order: AVDD (L28) then DVDD (L6), select `on_state` pinctrl, wait 5 ms (G
  ftm4_ts.c:1448-1489). Linux sends no voltage or load call beyond the enable, so the vote is `swen` = 1
  plus `uv` (the DT init voltage). PM8998 L6 and L28 are both `pldo` (M qcom_smd-regulator.c:1281,
  1303). Keys: `swen` 0x6e657773, `uv` 0x00007675, `ma` 0x0000616d (M qcom_smd-regulator.c:38-40).

### 2.5 GCC clocks (GCC base 0x00100000, live.dts:2408-2417)

The DT clock ids are downstream hashes: iface 0x8CAA5B4F = `clk_gcc_blsp1_ahb_clk` and core
0xACAE5604 = `clk_gcc_blsp1_qup5_i2c_apps_clk` (live.dts:14864; L msm-clocks-8998.h:162, 171).

| Register | Offset | PA | Use | Source |
|---|---|---|---|---|
| APCS_CLOCK_BRANCH_ENA_VOTE | 0x52004 | 0x00152004 | set **bit 17** = BLSP1 AHB (voted branch, do not write its CBCR enable) | L msm-clocks-hwio-8998.h:61; L clock-gcc-8998.c:1218-1229; M gcc-msm8998.c:1448-1459 |
| BLSP1_AHB_CBCR | 0x17004 | 0x00117004 | poll bit 31 (CLK_OFF) = 0 | L hwio:127; M gcc-msm8998.c:1449 |
| BLSP1_BCR | 0x17000 | 0x00117000 | block reset, do not touch | L hwio:128; M gcc-msm8998.c:3236 |
| BLSP1_QUP5_BCR | 0x21000 | 0x00121000 | QUP5 reset, do not touch | M gcc-msm8998.c:3211 |
| BLSP1_QUP5_I2C_APPS_CBCR | 0x21008 | 0x00121008 | set bit 0, poll bit 31 = 0 | L hwio:138; L clock-gcc-8998.c:1327-1337; M gcc-msm8998.c:1605-1620 |
| BLSP1_QUP5_I2C_APPS_CMD_RCGR | 0x21020 | 0x00121020 | bit 0 UPDATE, bit 31 ROOT_OFF | L hwio:80; M gcc-msm8998.c:598-610 |
| BLSP1_QUP5_I2C_APPS_CFG_RCGR | 0x21024 | 0x00121024 | 19.2 MHz = SRC_SEL (10:8) 0 = XO, SRC_DIV (4:0) 1 (= divide by 1) | M gcc-msm8998.c:411-414 (P_XO = 0), 469-473; L clock-gcc-8998.c:309-313 |

Compared with the Pixel 1 the offsets are the same pattern on another base (vote 0x352004 bit 17 and
QUP3 0x31d008 there, `sweet_touch.c:40-41`). QUPn I2C on MSM8998 sits at 0x19008 + 0x2000*(n-1).
Keep the 400 kHz divider: i2c-msm-v2 uses fs_div 28, ht_div 14 at 400 kHz from 19.2 MHz
(L i2c-msm-v2.c:414-418, register format 461-462), and `QUP_CLK_400K` in `sweet_touch.c:90` has the
same fs/ht values (plus hs_div 3).

## 3. What `sweet_touch.c` gives us

Local `scratch/xnu/bsd/dev/sweet_touch.c` (Pixel 1, Synaptics RMI4):

| Part | Lines | Taimen |
|---|---|---|
| `touch_rd`/`touch_wr` (barriers), `touch_wait` | 106-129 | reuse as is |
| TLMM summary IRQ handler and arm/wait (INTID 240, INTR_CFG constants) | 131-173 | reuse; only the GPIO 125 tile address changes (option A) |
| `touch_gpio` CFG/IN_OUT writer | 175-184 | reuse; same bit layout on 8998 |
| `touch_clocks` | 186-202 | reuse the logic; new addresses (vote 0x152004 bit 17, AHB status 0x117004, CBCR 0x121008, RCGR 0x121020/24) and also wait for the AHB status |
| QUP register map, `qup_state`, `qup_msg`, `qup_init` (FIFO mode, v1 tags, 400 kHz) | 52-90, 204-309, 339-358 | reuse. Both SoCs have the same QUP (mainline `qcom,i2c-qup-v2.2.1` on both). Make the slave address a parameter (`TOUCH_ADDR` is baked into `qup_msg`:228); 0x20 becomes 0x49 |
| `touch_report` (delta to `sweet_hid_touch`, 60 Hz move limit) | 428-466 | reuse unchanged; feed it a mask and x/y arrays built from FTM4 events |
| thread skeleton, boot-args, error/backoff loop | 504-641 | reuse the shape; new init steps |
| `rmi_write`/`rmi_read`, `bits`, `rmi_init`, `touch_poll` | 311-337, 360-426, 468-502 | **RMI4-only: replace** with the FTM4 code in §5 |
| Rails table (PM8994 L6/17/18/19/23) and the PM8994 SPMI LDO readback | 529-552 | **replace**: RPM L28/L6 only. The SPMI read path (`sweet_pmic_read`) uses MSM8996 arbiter addresses, so leave it out on taimen (audit item 4) |
| Reset timing (RMI4: 2 ms low, 150 ms boot) and mux GPIO 80 | 556-564 | **replace** with §5.3 timings and GPIO 75 (same polarity: 0 = AP) |

Design choice: write a new `bsd/dev/taimen_touch.c` compiled only under `ARM64_BOARD_CONFIG_TAIMEN`.
Copy the QUP and TLMM helpers rather than share them, which keeps the Pixel 1 driver untouched. Add it
to `bsd/conf/files` next to `sweet_touch.c` (local `bsd/conf/files:112`).

## 4. RPM (`sweet_rpm.c`) on taimen

Pixel 1 and taimen both use the **GLINK** native RPM transport, not SMD. On Pixel 1 that is
`qcom,glink-rpm-native-xprt` (`sweet_rpm.c:2-9`). On taimen it is
`qcom,glink-smem-native-xprt-rpm@778000`, compatible `qcom,glink-rpm-native-xprt` (live.dts:3554-3561),
with `qcom,rpm-smd` being `qcom,rpm-glink`, edge `rpm`, channel `rpm_requests` (live.dts:2697-2700).
The protocol in `sweet_rpm.c` (VERSION/OPEN/TX_DATA, `req` framing, `msg#`/`err` parsing) carries over
unchanged. Change these under `#if defined(ARM64_BOARD_CONFIG_TAIMEN)`:

| | Pixel 1 (`sweet_rpm.c`) | Taimen | Source |
|---|---|---|---|
| Message RAM | 0x68000, 0x6000 (`:37-38`) | **0x778000, 0x7000**: TOC `grt0` at 0x77EF00 (last 256 bytes) | live.dts:3556; M msm8998.dtsi:845-848; M qcom_glink_rpm.c:25-26, 231 |
| Doorbell | page 0x9820000, write 1 at +0x10 (`:39`, `:99`) | **page 0x17911000, write 1 at +0x8** (0x17911008, bit 0) | live.dts:3556-3558 (`irq-reg-base`, `qcom,irq-mask = 0x1`); M qcom-apcs-ipc-mailbox.c:40-42, 160 (msm8998 offset 8; 8996 offset 16 at :44-46) |
| RPM-to-AP IRQ | (polled) | SPI 168 edge = INTID 200 (still polled) | live.dts:3559 |
| Regulator PMIC | PM8994 | PM8998. The type is still `ldoa`, with ids 28 and 6 | live.dts:3392-3395, 2983-2986 |
| Sets | active only (`sweet_rpm_ldoa` set 0, `:359`) | send set 0 **and** set 1 (DT `qcom,set = <3>`) | live.dts:2992, 3401 |

- 0x17911008 is shared: SMP2P uses bit 14 (modem), bit 10 (ADSP) and bit 26 (SLPI) of the same
  register (live.dts:306-325). Write only `0x1`.
- The MPM's vMPM region 0x7781B8 lies inside the message RAM (live.dts:5283-5285). Never write outside
  the FIFOs the TOC names. An `rpm-code-ram` node also overlaps (0x778000+0x5000, live.dts:5272-5276).
  That is harmless, since the transport node is the authority.
- Do not call `sweet_rpm_clk_scaling` on taimen. Its handoff table (`:471-496`) lists MSM8996 clocks.
- Add `sweet_rpm_ldoa_set(unsigned ldo, uint32_t uv, uint32_t set)` (or loop sets inside
  `sweet_rpm_ldoa`) for the sleep-set vote.
- Expected log: `SWEET RPM: tx len .. rx len ..`, `link up`, `L28 3300000 uV: ok`,
  `L6 1800000 uV: ok`. A bad TOC magic means the address or size is wrong. Stop there and do not try
  anything else.

## 5. FTM4 protocol (from G `ftm4_ts.c` / `ftm4_ts.h`)

### 5.1 Transfers

- **Write** = one I2C write of N bytes (G ftm4_ts.c:183-218).
- **Read** = write of the command bytes, then a read of M bytes. Linux sends both messages in one
  `i2c_transfer`, i.e. with a **repeated start** (ftm4_ts.c:220-261; i2c-msm-v2 builds START without
  STOP for the first message, L i2c-msm-v2.c:184-194).
- `sweet_touch.c`'s `qup_msg` sends a STOP between the two, as mainline's v1-tag path does for every
  message (M i2c-qup.c:483-512, 969-979). The FTM4 most likely accepts write-STOP-read
  **(unverified)**. Fallback if reads come back stale or as 0xFF: a combined v1-tag run in one QUP
  RUN, sketched under §6 step 12.
- Register access through `B6 hi lo` (16-bit address, big endian). A read returns **one dummy byte
  first**, so the data starts at buf[1] (chip id ftm4_ts.c:495-512; event count :1344-1353). The
  `D0 00 xx` system-info reads have the same dummy byte (:430-446).

### 5.2 Commands used

| Bytes | Meaning | Source |
|---|---|---|
| `B6 00 2C 08` | interrupt disable (`INT_DISABLE` 0x08) | ftm4_ts.c:315-327; ftm4_ts.h:131-132 |
| `B6 00 2C 48` | interrupt enable (`INT_ENABLE` 0x48) | same |
| `B6 00 1E 20` | enable warm-boot CRC ("WBCRC"), then wait 10 ms | ftm4_ts.c:302-306 |
| `B6 00 28 80` | system reset, then wait 10 ms | ftm4_ts.c:301, 308-310 |
| `85` + read 8 | read one event (pops it) | ftm4_ts.h:135; ftm4_ts.c:540-543 |
| `86` + read 8*n | read n events | ftm4_ts.h:136; ftm4_ts.c:1360-1361 |
| `84` + read 4 | read status (diagnostic) | ftm4_ts.h:134; ftm4_ts.c:858-864 |
| `B6 00 23` + read 2 | event count: n = buf[1] >> 1, capped at 32 | ftm4_ts.c:1344-1356; `FTS_FIFO_MAX` 32, ftm4_ts.h:52 |
| `B6 00 04` + read 7 | chip id: buf[1] = 0x36, buf[2] = 0x70; buf[5] = buf[6] = 0 means no firmware | ftm4_ts.c:493-528; ftm4_ts.h:44-45 |
| `D0 00 56` + read 7 | versions: fw = b1 \| b2<<8, config = b3 \| b4<<8, main = b6 \| b5<<8 | ftm4_ts.c:426-446 |
| `B2 00 14 02` | request 2 bytes of reg 0x0014; the answer arrives as event 0x12 (e1 = 0x00, e2 = 0x14, e3 = sense lines, e4 = force lines) | ftm4_ts.c:604-634 |
| `93` / `92` | sense on / sense off | ftm4_ts.h:138-139 |
| `A1` | flush event FIFO | ftm4_ts.h:154 |
| `A2` | force calibration | ftm4_ts.h:155 |

Not needed: hover (94/95), charger (A8/AB), scan rate (`C3 xx`), lockdown (C4), the flash commands
F0-FC (ftm4_ts.h:140-178, 465-486).

### 5.3 Power-on, reset and init sequence in Linux

1. `fts_parse_dt`: reset GPIO 89 requested **output low**, TA GPIO 50 output low (ftm4_ts.c:1634-1660);
   switch GPIO 75 output low = I2C to AP (:1790-1807).
2. `fts_power_ctrl(on)`: enable L28, enable L6, pinctrl `on_state` (89/125 gpio, 16 mA, pull-up),
   wait 5 ms (:1448-1489).
3. `fts_probe`: wait 15 ms, reset GPIO high (:1850-1856). Then `fts_init` runs immediately, with no
   delay after reset release:
4. `fts_init` (:769-870): INT disable; system reset (WBCRC write, 10 ms, reset write, 10 ms);
   `fts_wait_for_ready`: repeat `85` + read 8 until e0 = 0x10 (CONTROLLER_READY), waiting 20 ms between
   tries and giving up after `FTS_RETRY_COUNT` 10 (+1) tries (~220 ms). Error events (e0 = 0x0F) do not
   count against the tries but abort after 32; e1 = 0x03 is flash corruption with e2 = 1/2 config or
   3 CX (:530-602). Then version info, chip id (abort if not 0x36 0x70), channel info, `93` sense on,
   `A2` force calibration, INT enable, `84` status read.
5. Android then opens the input device: `fts_input_open` -> `fts_start_device` -> `fts_reinit`
   (:2087-2105, 2306-2365, 2141-2169): INT disable, system reset, wait ready, `93`, **50 ms**, `A1`
   flush, INT enable. This is the state Android runs in. The reset discards the probe-time forced
   calibration.
6. Recovery (`fts_reset_work`, :2215-2233): rails off, 100 ms, rails on, `fts_reinit`. It is triggered
   by status event 0x16 with e1 = 0xED (ESD reboot, :972-975).
7. Display blank: `fts_suspend` sets GPIO 75 = 1 (bus handed to the SLPI) after stopping; resume sets
   it to 0 (:2452-2502). XNU never hands the bus over.

### 5.4 Reading events

- On attention (or a poll tick): read the count (`B6 00 23`, 2 bytes, n = buf[1] >> 1, at most 32),
  then read `86` with 8*n bytes in one transfer (ftm4_ts.c:1341-1366). Up to 32 events / 256 bytes
  per interrupt.
- XNU constraint: `qup_msg` reads at most `rx_max` bytes, half the input FIFO with v1 tags
  (`sweet_touch.c:350-357`). Plan: if 8*n <= `rx_max`, one `86` read. Otherwise loop `85` (one event
  per read, which certainly pops one, ftm4_ts.c:540-593) n times. Do not do partial `86` reads: whether
  a short `86` read pops only the events clocked out is **unverified**.
- **Interrupt acknowledge:** none on the device. Attention is a level, low while the FIFO holds events,
  and it releases when the FIFO is drained. Linux uses a ONESHOT threaded IRQ: masked from the hard
  IRQ until the thread has read the FIFO (ftm4_ts.c:1324-1366, 1950-1952). XNU does the same: the ISR
  masks (TLMM INTR_CFG enable = 0 in option A, GIC mask in option B), clears the TLMM status (option A),
  and wakes the thread. The thread drains and re-reads the count until 0 (or attention, GPIO 125 IN
  bit, reads high), then unmasks.

### 5.5 Event record (8 bytes e0..e7, ftm4_ts.c:989-1292)

- For touch events the event id is `e0 & 0x0F` and the touch id is `e0 >> 4`:
  `0x3` ENTER, `0x4` LEAVE, `0x5` MOTION (ftm4_ts.h:64-67; ftm4_ts.c:1024-1032).
  - x = `(e1 << 4) | (e3 >> 4)`, y = `(e2 << 4) | (e3 & 0x0F)`: 12 bits each, 0..1439 / 0..2879
    (:1125-1132)
  - pressure z = `e4` (0..255). **z == 255 = palm**: Linux releases all fingers and ignores events until
    the next LEAVE (:1134, 1145-1158, 1189-1195)
  - orientation = `(int8_t)e5`, major = `(e6 << 2) | (e7 >> 6)`, minor = `(e7 & 0x3F) * major / 63`
    (:1136-1143)
  - touch ids 0..9 are used (`FINGER_MAX` 10, ftm4_ts.h:60); ignore ids >= 10.
  - MOTION for an id whose last state was LEAVE is dropped (:1114-1120).
- Other events use the whole byte e0 (:1033-1041): 0x00 none; 0x0F error (e1 type, ftm4_ts.h:89-96);
  0x10 controller ready; 0x12 register-read result; 0x16 status (e1 code, ftm4_ts.h:107-129; 0xED
  means reset needed); 0x1E lockdown; hover 0x07/0x08/0x09 (different packing, ignore).
- Caveat: 0x13, 0x14 and 0x15 have low nibbles 3/4/5. Linux would misparse them as touch id 1 events,
  but they only answer commands this driver never sends (status request, release info).
- Orientation: panel-native portrait, no axis swap or flip. Linux reports x/y directly with ranges
  1439/2879 (:1888-1891) and the panel is 1440x2880 (live.dts:12996-12997). Confirm by touching
  corners **(unverified on device)**.

## 6. Driver design: `bsd/dev/taimen_touch.c`

One kernel thread, as in `sweet_touch.c`. Boot-args: `taimentouch` (default **0** for the first boots,
then 1), `sweettouchdelay`, `sweettouchhz`, `sweettouchirq` (0 poll, 1 summary, 2 direct),
`taimentouchcal` (send `A2`, default 0).

Init steps, logging every register "before/after" in the style of `sweet_touch.c:524-528`:

1. Map (one 4 KiB `ml_io_map` each): GCC 0x152000, 0x117000, 0x121000; QUP 0xC179000; TLMM pages
   0x3D57000, 0x3D58000, 0x3D59000, 0x3D7D000, 0x354B000, 0x3932000; for option B also 0x3DA1000.
   Log the CFG/IN_OUT of all six pins.
2. GPIO 89: IN_OUT = 0 first, then CFG 0x3C3 (reset asserted). GPIO 75: IN_OUT = 0, CFG 0x201.
   GPIO 50: IN_OUT = 0, CFG 0x201.
3. RPM: `ldoa` 28 3300000 then `ldoa` 6 1800000, active and sleep sets (§4).
4. GPIO 125 CFG 0x1C3; GPIO 87/88 CFG 0x00C (function 3).
5. Wait 20 ms (5 + 15, as Linux). Log SDA/SCL/attention IN bits (expect 1/1/1, see §2.2).
6. Reset high (IN_OUT bit 1 = 1). Wait 20 ms **(margin added; Linux waits 0 before the first write)**.
7. Clocks: OR bit 17 into 0x152004 and poll 0x117004 bit 31 = 0. Read CFG_RCGR 0x121024: if
   SRC_SEL != 0, write 0x1, set CMD_RCGR bit 0 and poll it clear. OR bit 0 into CBCR 0x121008 and poll
   bit 31 = 0. Log vote/CBCR/RCGR.
8. `qup_init`: log `QUP_HW_VERSION` (+0x30), IO_MODE and FIFO sizes; require `rx_max >= 16`.
9. `B6 00 2C 08` (INT disable). A NACK here (`ENXIO`) means: check GPIO 75, the rails, the address.
   Retry up to 5 times at 10 ms.
10. System reset: `B6 00 1E 20`, 10 ms, `B6 00 28 80`, 10 ms.
11. Wait ready: up to 12 x (`85`, read 8; 0x10 -> ready; 0x0F 0x03 xx -> log "flash corrupt" and
    continue; else 20 ms).
12. `B6 00 04` read 7: require 0x36 0x70; if b5 = b6 = 0 log "no firmware in the FTM4 flash" and stop.
    If this read returns garbage (0xFF.. or the bytes shifted) while writes succeed, switch reads to a
    combined repeated-start QUP run: output tags `START|0x92, DATA|cmd.., START|0x93, REC|len` packed two
    16-bit tags per word, low half first; MX_WRITE_CNT = ncmd + 3; MX_READ_CNT = len; QUP_CONFIG without
    NO_INPUT. The pre-mainline downstream msm `i2c-qup.c` issued combined messages this way
    **(unverified)**.
13. `D0 00 56` read 7: log fw/config/main versions. `B2 00 14 02` then `85` loop (31 x 5 ms) for event
    0x12 00 14: log sense/force (expect 32/16).
14. `93` sense on; wait 50 ms. If `taimentouchcal`: `A2`. Then `A1` flush, `B6 00 2C 48` INT enable,
    `84` read 4 (log).
15. Interrupt setup:
    - Option A: INTR_CFG 0x70, STATUS 0, `sweet_spi_attach(240, false, ...)`.
    - Option B: CFG of 125 stays 0x1C3, INTR_CFG 0x160, slot 0 = 125, `sweet_spi_attach(248, false, ...)`.
    - Fallback in either case: poll every 10 ms on GPIO 125 IN = 0, and read anyway every 200 ms
      (`sweet_touch.c:584-615`).

Service routine (replaces `touch_poll`):
- Read the count; read the events (§5.4); for each record update `active` (bitmask), `x[id]`, `y[id]`.
  ENTER/MOTION set the bit (MOTION only if the id is active or was just entered). LEAVE clears it. Palm
  (z = 255) clears all and sets `palm`, which drops events until a LEAVE.
- After the batch: `touch_report(active, x, y)` (unchanged).
- Handle e0 = 0x16 / e1 = 0xED and repeated errors: release all (`touch_report(0, ...)`), then full
  recovery (L6/L28 off is optional; at least reset GPIO low 20 ms, high, steps 9-14).
- I2C errors: as `sweet_touch.c:592-604` (`qup_init`, release after 10 bad reads). If SDA reads 0 with
  the bus idle, recover the bus: 87/88 to GPIO function (bitbang state), pulse SCL 9 times at ~100 kHz,
  generate a STOP, back to function 3.

Call site: `scratch/xnu/bsd/dev/sweet_ufs.c:663-671` currently starts no input driver on taimen (all
Pixel 1 drivers poke MSM8996 addresses). Add `taimen_touch_init()` in the TAIMEN branch next to the
log line there, after the root is mounted, as on Pixel 1 (:672-678).

## 7. SweetHID mapping (`scratch/xnu/iokit/Kernel/arm/hid/SweetHID.cpp`)

Today: `SWEET_TOUCH_PANEL_W/H` 1080/1920 (:359-360); `start()` sets `fLogicalW/H` to those and
`fOffX/fOffY` = 0 (:442-445); SurfaceWidth/Height 62/111 mm (:448-449); raw clamp to the panel
(:466-467); normalisation `nx = (x + fOffX) / fLogicalW` (:471-473); edge test against the panel
(:479-482). (The brief's ":446-447" for the clamp is :466-467 in the current tree.)

Taimen geometry: SweetFramebuffer presents the 1125x2436 canvas 1:1 in the middle of the 1440x2880
panel with `fOffX = (1125 - 1440) / 2 = -157` and `fOffY = (2436 - 2880) / 2 = -222` (C truncation;
`SweetFramebuffer.cpp:457-471`), and maps logical to panel with `panel = logical - fOffX`
(`:1014-1016`). The canvas therefore covers panel x 157..1281 and y 222..2657. Changes, under TAIMEN:

1. Raw clamp to the FTM4 range: x 0..1439, y 0..2879 (`stm,max_coords`).
2. `lx = x + fOffX` (x - 157) and `ly = y + fOffY` (y - 222), **clamped to 0..1124 / 0..2435**. Then
   `nx = (lx << 16) / 1125`, `ny = (ly << 16) / 2436`. These are SweetHID's existing fields and
   formula with fLogicalW/H = 1125/2436 and fOffX/fOffY = -157/-222. The sign convention matches
   SweetFramebuffer, so the numbers can be shared.
3. Take the numbers from the framebuffer rather than hardcoding them, since `sweetfb_native=1` gives a
   1440x2880 logical display with offset 0. Add a C accessor in SweetFramebuffer, e.g.
   `sweet_fb_touch_geometry(int *lw, int *lh, int *offx, int *offy)`, filled in `start()`. SweetHID
   registers before AppleCLCD (NOTES boot 3 order), so query it on each touch-down, falling back to
   1440x2880/0/0.
4. Edge flag on the clamped logical coordinates. A touch that starts in the black border is clamped
   onto the canvas edge and counts as an edge touch, so swipes from the physical bottom work as the
   home gesture. 3 mm at 21.2 px/mm (1440 px / 68 mm, live.dts:13052) is ~64 px: use 64 instead of 48
   on taimen.
5. SurfaceWidth/Height = the visible canvas: 1125 / 21.18 = **53 mm** and 2436 * 136 / 2880 =
   **115 mm**. The panel is 68 x 136 mm (live.dts:13052-13053). The effect on UIKit is small
   **(unverified)**.

## 8. Bring-up order (each step is a boot with a log)

1. RPM only (`taimentouch=1` stops after step 5): link up, two acks, SDA/SCL/attention read 1.
2. Clocks + QUP: CBCR/vote status bits clear, sane `QUP_HW_VERSION` and FIFO size.
3. FTM4 handshake: ready event 0x10, chip id 0x36 0x70, versions, 32/16 lines.
4. Polled events (`sweettouchirq=0`): `SWEET TOUCH: down` lines with plausible coordinates. Check
   corners: top-left ~0,0 and bottom-right ~1439,2879.
5. Summary interrupt (option A); option B only if A is starved by another TLMM pin.
6. HID mapping in SpringBoard: tap targets line up; home swipe from the bottom edge.

## 9. Open risks

1. **Write-STOP-read** may not be accepted by the FTM4; Linux uses a repeated start. Fallback in §6
   step 12 (the combined v1-tag run is itself unverified).
2. **Rail and switch state at handoff are unknown.** GPIO 75 stuck at 1 (bus at the SLPI) or L6 off
   both look like a NACK. Step 5's IN-bit log tells them apart.
3. **RPM on taimen is untested from XNU.** Only the addresses change, but there is no proof yet that
   the RPM answers an EL1 GLINK client after ABL. A wrong TOC is detected before any write.
4. **QUP FIFO size and FIFO-mode limits on 8998** (`rx_max`). They decide between the bulk `86` read and
   the `85` loop; both paths are planned.
5. **Direct-connect details** (slot -> SPI 216 mapping, polarity bit, Linux setting OE on an input)
   are inferred. Option A avoids them, but the TLMM summary line can be held by another pin left
   enabled by firmware (handled by the existing fallback, `sweet_touch.c:157-160`).
6. **No firmware in flash / corrupt config:** the driver can only report it. Reflashing would need the
   `ftm4_fwu.c` flash path and `ftm4_fw.ftb` from `/vendor/firmware`, which is out of scope.
7. **Calibration:** Android runs without a forced calibration after `fts_reinit`. If baselines drift
   (ghost touches), try `taimentouchcal=1` (sends `A2`).
8. **Other hardware revisions** that use the SW49408 SPI node would not work with this driver. This
   phone's DT says FTM4.
9. Corner and orientation mapping still has to be confirmed by touching the screen.
