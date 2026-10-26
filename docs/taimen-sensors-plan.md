# Motion and environment sensors on taimen (accelerometer, gyro, compass, light, proximity): plan

2026-10-25. Research and plan only, nothing built or run. Reference keys:
`DTS` = research/taimen/live.dts, `dmesg` = hw/dmesg.txt (LineageOS boot on this phone),
`FW` = out/taimen-xnu-r150-abi/reader/fw-list.txt (the reader's `ls -l /vendor/firmware`),
`PERSIST` = backup/persist.img (ext4, `debugfs -R 'ls -l /sensors'`), `ADT` = `tools/adt.py dump research/ipsw-x/d22ap.adt`,
`ROOT` = research/ipsw-x/ios16-root.img (7-Zip, as mkrootfix.py `extract()`), `M/` = research/dl/mainline-6.1/,
`X/` = scratch/xnu/, `AUDIO` = docs/taimen-audio-plan.md. Read online on 2026-10-25 (not stored locally):
- `PAS/` = drivers/remoteproc/qcom_q6v5_pas.c, `REG/` = drivers/soc/qcom/qcom_sns_reg.c, `SMGR/` =
  drivers/iio/common/qcom_smgr/{qcom_smgr.c,qmi/qmi_sns_smgr.[ch]}, all at gitlab.com/msm8996-mainline/linux tag
  v6.17.1-msm8996 (GPL-2.0; SMGR by Yassine Oudjana, REG ported to the kernel by Alexey Minnekhanov from Oudjana's
  user-space sns-reg).
- `LKML` = the series "QRTR bus and Qualcomm Sensor Manager IIO drivers" (cover letter 2025-04-06, quoted at
  lkml.iu.edu/hypermail/linux/kernel/2504.1/00718.html; review at lkml.rescloud.iu.edu/2506.2/05186.html). Not merged
  upstream as far as found.
- `HID/` = Apple's IOHIDFamily-870.1.10 `IOHIDFamily/AppleHIDUsageTables.h` (github.com/apple-oss-distributions).

## 0. Verdict

- **Taimen side.** The motion and environment sensors hang off the Snapdragon Sensor Core (SSC). Its own processor, the
  SLPI, owns their bus. The AP cannot reach them (section 1). So the path is the ADSP recipe again, on another edge:
  1. boot the SLPI by TZ PAS (id 12);
  2. SMP2P (pid 3) and GLINK on the "dsps" edge, then QRTR;
  3. **serve the sensor registry** (QMI `SNS_REG`, 0x10F) from the phone's own /persist/sensors/sns.reg;
  4. be a client of the **Sensor Manager** (QMI `SNS_SMGR`, 0x100), the pre-SEE API that MSM8996/8998 use.

  The open-source description of both services is good enough for accelerometer, gyro, magnetometer and proximity
  samples. It does not cover light (section 2.3).
- **iOS side.** Rotation needs one kernel IOHIDEventService: an accelerometer with Apple's vendor usage pair
  0xff00/0x03, like the iPhone X's AOP `accel` node. The chain is:
  1. locationd's CLAccelerometer opens it through the HID event system;
  2. its CLOrientationNotifier derives the device orientation;
  3. backboardd's BKOrientationManager takes the orientation from CoreMotion (CMDeviceOrientationManager).

  Gyro (0xff00/0x09), compass (0x0A), ambient light (0x04) and proximity (0x08) are more services of the same kind.
  SweetHID (xnu 0058) already has the base class and the event constructors.
- **First milestone:** M0. A synthetic accelerometer driven from the PC proves the whole iOS half before any DSP work.
  Then the SLPI chain M1-M6.

## 1. Hardware (question 1)

| Sensor | Chip | Bus |
|---|---|---|
| accel + gyro | not named locally. Candidates: Bosch BMI160 or ST LSM6DSM (unverified) | SSC (SLPI) |
| magnetometer | AKM AK09915 (unverified) | SSC |
| pressure | Bosch BMP285/BMP280 (unverified) | SSC |
| light + proximity | an ams TMD-series part (unverified) | SSC |
| squeeze (Active Edge) | strain gauges, `PERSIST /sensors/elmyra/` | SSC (unverified) |
| laser ToF | ST VL53L0 `wahoo_laser@29` on i2c@c1b7000 (BLSP2 QUP3; DTS:15130, 15153-15165; dmesg:700-718) | AP |

- **No local file names the SSC chips.**
  - The DTS has no node for any of them: the SSC owns them.
  - `PERSIST /sensors` holds `sns.reg` (28,174 B, a binary registry), `sns.reg.factory`, `sensors_settings`,
    `calibration/` (an ARCore IMU rig XML, no part names) and `elmyra/`.
  - Public sources give the sensor types (accelerometer, gyroscope, compass, barometer, light, proximity) but no
    authoritative part list for taimen.
  - **M4 settles it:** SMGR's single-sensor info returns each sensor's name and vendor strings (section 2.2).
- **The only AP-reachable "proximity" chip is the rear laser.** It is camera autofocus, facing backwards: useless for
  calls or face detection.
- **The AP cannot reach the SSC bus, as far as anything shows:**
  1. The HLOS tree has no SSC QUP, I2C or SPI controller and no SSC clock controller. Every I2C node is BLSP1/2
     (0x0C175000-0x0C1BA000). The SSC appears only as the PIL node, the sensors loader and the IPC edges (below).
  2. Mainline's msm8998 GCC lists `SSC_XO` and `SSC_CNOC_AHBS_CLK` as protected clocks (M/arch_arm64_boot_dts_qcom_msm8998.dtsi:813-827).
     Its comment says the hypervisor maps them "read-only for the HLOS", and touching them reboots.
  3. LKML: on MSM8996/8998 the SSC sensor peripherals are "generally inaccessible from the AP".
  4. Android itself only goes through the SLPI: dmesg:2046-2049, 2153, 2160-2163, then the sensors HAL (2945, 2960).

  TZ's XPU setup for the SSC cannot be read without probing. A probe from EL1 risks an access-violation reset
  (unverified). No open source found describes HLOS driving the SSC QUP on this SoC.
  **Verdict: only through the SLPI firmware.**

## 2. The SLPI (question 2)

### 2.1 How Android/Linux load it (all values from this phone's tree unless marked)

| Item | Value |
|---|---|
| PIL node | `qcom,ssc@5c00000`, `qcom,pil-tz-generic`, **PAS id 12** (`0xC`), smem-id 424 (crash text), sysmon-id 3, ssctl-instance 0x16, wdog SPI 390 (DTS:4437-4465) |
| Mainline | `qcom,msm8998-slpi-pas` uses PAS/ `msm8996_slpi_resource_init`: PAS 12, crash reason 424, ssctl 0x16, sysmon "slpi", ssr "dsps", proxy power domain "ssc_cx". Node at 0x05800000 (M/...dtsi:1476-1509) |
| Firmware | **slpi_v2.mdt + .b00-.b14** in /vendor/firmware: 16 files, 3,908,015 B, .b14 empty (FW:88-103). The PIL name is "slpi" (DTS:4454), but `qcom,msm-ssc-sensors` renames it to `slpi_v2` (DTS:4661-4666; dmesg:2046-2047). Not pulled yet |
| Carve-out | `pil_slpi_region` **0x94B00000 + 0xF00000** (DTS:16393-16398; mainline has 0x94300000). It is below XNU's RAM at 0xb1000000 |
| Proxy votes | vdd_cx = `pm8998_l27_level`, RPM resource **"rwsc"** id 0 at level 0x180 (TURBO) (DTS:3360-3380, 4441-4446); vdd_px = `pm8998_lvs2`, RPM "vsa" id 2, 1.8 V (DTS:3429-3434); clocks xo (`cxo_pil_ssc_clk`, a CXO voter) and aggre2 (the RPM aggre2 NoC clock); `keep-proxy-regs-on`; proxy timeout 10 s |
| SMP2P | remote pid 3. Out **430** `master-kernel` (b0 force-stop), in **481** `slave-kernel` (b0 fatal, b1 ready, b2 proxy-unvote, b3 stop-ack). Also an out entry `sleepstate`. APCS 0x17911008 **bit 26**, SPI 178 (DTS:320-326, 481-519; M/...dtsi:434-452) |
| GLINK | edge **"dsps"**, APCS **bit 27**, SPI 179 (DTS:3497-3504, 3546-3553; M/...dtsi:1503-1508). SMEM 478/479/480 in the APPS<->SLPI partition (hosts 0/3), as on lpass (unverified until M1) |
| QRTR | `IPCRTR` channel on "dsps" (DTS:3668-3676) |
| Timing on Android | load starts 2.428 s, out of reset 2.643 s, SSCTL up and "SLPI image is loaded" 2.674 s (dmesg:2049-2163) |

### 2.2 The sensor protocol: SMGR, not SEE

- **MSM8998 runs the SMGR-era SSC.**
  - PERSIST has the SMGR-era binary registry `/sensors/sns.reg` and no SEE `registry/` directory.
  - Android starts the `vendor.sensors` daemon (dmesg:2960), the AP end of the registry (unverified).
  - LKML names MSM8996 and MSM8998 as the SSC/SLPI SMGR platforms.
  - SEE (`sns_client` protobuf; postmarketOS libssc) belongs to later SoCs and does not apply here.
- **`SNS_REG`: we are the server.** svc 0x010F, version 2, instance 0, so the QRTR instance word is 0x0002
  (REG/; encoding `version | instance << 8`, as our pd-mapper's 0x101).
  - REG/ announces it only after the SLPI is up.
  - Only one message: group read 0x04.
    - Request: TLV 0x01, u16 group id.
    - Response: result 0x02, group id 0x03, then 0x04 = u16 length + at most 256 bytes.
  - The data is `size` bytes at `offset` in sns.reg, from a static table {group id, offset, size} in REG/. The table
    is facts, not code, and is re-typed. Two of its entries have a FIXME (groups 20 and 2980, size 0x7e on some SoCs).
  - An unknown group gets a failure result and zero length. Without a registry the SMGR may not configure the sensors
    (unverified).
- **`SNS_SMGR`: we are a client.** svc 0x0100, QMI version 1. The instance is per SoC (SMGR/ match table):
  - 50 on MSM8996/SDM660;
  - 0 on MSM8974/8226;
  - MSM8998 is not in the table: log every NEW_SERVER (expect 50, unverified).

  | Msg | ID | TLVs (SMGR/ qmi_sns_smgr.c) |
  |---|---|---|
  | ALL_SENSOR_INFO | 0x05 | resp: result 0x02; items 0x03 = [{u8 id, u8 len, type string}] |
  | SINGLE_SENSOR_INFO | 0x06 | req: 0x01 u8 sensor id. Resp: 0x03 data types [{sensor id, data type, name, vendor, max rate Hz, current uA, range, resolution}], 0x13 native rates |
  | BUFFERING | 0x21 | 0x01 u8 report id; 0x02 u8 action (1 add, 2 delete); 0x03 u32 report rate; 0x04 items [{u8 sensor id, u8 data type, u16 3, u16 sample rate Hz, u16 1}]; optional 0x10 {u16 proc type, u16 send during suspend} |
  | BUFFERING_REPORT (indication) | 0x22 | 0x01 report id; 0x02 {u32, u8 sample count, u32 timestamp, u32}; 0x03 samples [{u32 values[3], u8, u8, u16}] |

  - **Values:** signed Q16.16.
  - **Report rate:** Hz x 65536 gives one sample per report (SMGR/: `sample_rate * 32768 * 2`). Timestamps count
    32,768 Hz SSC ticks.
  - **Report id:** SMGR/ reuses the sensor id.
  - **Proximity:** a boolean.
  - **Axes:** SMGR/'s mount matrix calls the frame north-east-down. What that means for the device axes is checked at
    M5 (unverified).
  - The constants 3 and 1 in the item are unexplained; SMGR/ calls them "known to be working".

### 2.3 How complete the open sources are

| Piece | Source | Completeness |
|---|---|---|
| Boot (PAS, SMP2P, GLINK edge) | PAS/, M/ dtsi, downstream DTS | complete; our ADSP code (X/iokit/Kernel/arm/TaimenAudio.cpp) is the same flow |
| Registry | REG/ (kernel), sns-reg (user space) | group read only, static group map; enough to serve a phone's own sns.reg |
| Sensor Manager | SMGR/, LKML | accel, gyro, mag, prox, pressure work in it. Light and temperature "close to being implemented", so not decoded. Many fields unnamed (`val*`). Message sizes partly guessed. Hall-effect seen but skipped. No test on 8998 found |
| Fusion, gestures (SAM) | none | not needed: iOS fuses on the AP (locationd) (unverified) |

- **Provenance:** the SMGR authors built their description by watching Android's daemons talk to the SSC (LKML). We use
  only that published description: no capture of our own, no diag, nothing from the modem.
- **License:** the sources are GPL-2.0, and this tree is XNU (APSL). Write our code from the protocol facts above,
  never by pasting theirs.

## 3. iOS side (question 3)

### 3.1 What the iPhone X has

- **The sensors sit under `ADT /arm-io/aop/iop-aop-nub`** (AOP, `iop-nub,rtbuddy-v2`, firmware 'iphone10baop'):

  | Node | device-usage-page / device-usage |
  |---|---|
  | `accel` | 0xff00 / 0x03 |
  | `gyro` | 0xff00 / 0x09 |
  | `compass` | 0xff00 / 0x0A (plus `compass-orientation`, `compass-acc-compensation`, `charger-compensation-vers` 2) |
  | `prox` | 0xff00 / 0x08 |
  | `pressure` | 0x14 / 0x31 (as stored) |

  The same node also has `spherecontrol`, `SPUApp`, `aop-button` and `aop-audio`. Calibration comes from syscfg
  placeholders (ARot, GRot, CPAS, PxCl, ...). There is no ALS node.
- **HID/ names the usages.** Page 0xff00: Accelerometer 3, AmbientLightSensor 4, ProximitySensor 8, Gyro 9, Compass 0x0A,
  DeviceMotionLite 0x10. Page 0xff0C (AppleVendorMotion): DeviceMotion 4, DeviceMotion6 5.
- **The kernel side is AppleSPU (in the kernelcache, not read).** ROOT has its user-space parts:
  - `AppleSPU.kext/PlugIns/{AppleSPULib,FastpathLib,GNSSPassthroughLib}.plugin`;
  - HID plug-ins that match `Transport` = "SPU":
    - AppleSPUAccCompassPlugin and AppleSPUDispCompassPlugin: {65280, 10} plus a `compass-acc-compensation` or
      `compass-disp-coex` property;
    - AppleWirelessChargingServiceFilter: {65280, 54}.

### 3.2 Who consumes what (strings and plists only)

- **The HID event system runs in backboardd.** It loads `ROOT /System/Library/HIDPlugins/ServicePlugins/IOHIDEventServicePlugin.plugin`
  for every kernel service with `HIDServiceSupport` = true. SweetHIDTouch, SweetHIDButtons and SweetHIDKeyboard already
  arrive this way.
- **locationd (`ROOT /usr/libexec/locationd`):**
  - `CLIoHidInterface` is an IOHIDEventSystemClient. It logs "Adding match entry for usage pair {%d, %d}" and
    "Checking matching property %@ = %@ for service ref", and sets `ReportInterval` and `BatchInterval`.
  - `CLAccelerometer::onAccelerometerIohidEvent` takes the accelerometer events. Matching uses "IMULocationID" and
    `LocationID` ("Setting Accel matching properties with IMULocationID %d"; the same for Gyro and Mag).
  - `CLOrientationNotifier` / `CLOrientationDetector` derive the orientation: "No accelerometer; not detecting
    orientation!", "Starting orientation detection with accelFrequency %f, form factor, %d".
  - Also: CLGyro (`setGyroBatchInterval`), CLCompass/CLCompass7, CLProximityNotifier, CLAmbientLightSensorInterface.
  - `CLSPUHIDDriverInterface` sends SPU commands through AppleSPULib. `FastPathDevice` uses
    `IOHIDServiceClientFastPathInit/CopyEvent`.
- **backboardd (`ROOT /usr/libexec/backboardd`):**
  - **Rotation:** BKOrientationManager links `CMDeviceOrientationManager` ("Updating device orientation from CoreMotion
    to: %@", "Effective device orientation changed to: %@").
  - **App accelerometer clients:** BKAccelerometerInterface / BKSAccelerometer (update interval, x/y/z thresholds).
  - **Proximity:** BKHIDProximityEventProcessor, `ProxDetectionMode`, "discrete prox service %llX top placement".
  - **ALS:** settings `ALSSupported`, `ALSEnabled`, BKHIDALSInternalSettings.

### 3.3 The minimum the kernel must publish

**For rotation**, one service, `SweetHIDAccel`, on the SweetHID base (X/iokit/Kernel/arm/hid/SweetHID.cpp:60-100). The base
already sets HIDServiceSupport, `IOUserClientClass` IOHIDEventServiceUserClient, Transport "SPU", VendorID 0x05ac,
Built-In, ReportInterval and DeviceUsagePairs.

| Property / behavior | Value |
|---|---|
| PrimaryUsagePage / PrimaryUsage | 0xff00 / 0x03, so DeviceUsagePairs [{65280, 3}] |
| LocationID | the base sets it to the product id; change it only if locationd's matching log asks (IMULocationID, unverified) |
| setProperties | today ignored (:244). Must take `ReportInterval` / `BatchInterval` (us; 0 = stop) and start or stop the source |
| copyEvent | today NULL (:226). Return the last sample (locationd "orientationBlocking", "Fetching historical events"; unverified need) |
| events | `IOHIDEvent::accelerometerEvent(ts, x, y, z)` (X/iokit/Kernel/arm/hid/IOHIDEvent.cpp:425). Event type 13, IOFixed in **g**, motion type normal (locationd discards "shake" data), `mach_absolute_time` timestamps |
| sign | iOS reads about z = -1 g lying face up; Android about +9.81 m/s² (unverified for this SMGR, M5). Expected x,y,z = -android/9.80665 |

**For CoreMotion raw data** (CMMotionManager goes through locationd), more services of the same kind:

| Service | Usage pair | Event |
|---|---|---|
| gyro | 0xff00/0x09 | `gyroEvent` (type 20), rad/s (unverified) |
| compass | 0xff00/0x0A | `compassEvent` (type 21), uT (unverified) |
| ambient light | 0xff00/0x04 | `ambientLightSensorEvent` (type 12), level + illuminance |
| proximity | 0xff00/0x08 | `proximityEvent` (type 14), detection mask |

- Proximity also takes backboardd's `ProxDetectionMode` property, and probably a placement property (unverified).
- DeviceMotion (attitude) may want an AOP-fused service (0xff00/0x10, 0xff0C/4-5). The fallback is locationd's own
  fusion (unverified; "CLCompass7 requires deviceMotionService").
- **Not to publish:**
  - the `compass-acc-compensation` / `compass-disp-coex` properties (they load Apple's SPU compass plug-ins);
  - any AppleSPU user client: a closed ABI. Without it, CLSPUHIDDriverInterface should just log "Service doesn't exist"
    (unverified).
- **No orientation service.** No `kIOHIDEventTypeOrientation` service shows up in the chain above (unverified).

## 4. Options and effort

| Option | What | Effort | Verdict |
|---|---|---|---|
| A | sensor HID services + a synthetic accelerometer from the USB control port (`accel X Y Z`, `orient left`) | ~350 lines, 1 day, 2-4 boots | **do first**; stays as the test hook |
| B | SLPI by PAS, SNS_REG server, SMGR client feeding A | ~1,500 lines, 4-7 days, 15-25 boots | **recommended** hardware path |
| C | AP drives the SSC bus with the SLPI off | unknown | ruled out: no register map, protected clocks, TZ |
| D | Apple's AOP/SPU protocol (AppleSPULib, Fastpath) | n/a | ruled out: closed ABI, and not needed by HID clients |
| E | SEE / libssc | n/a | wrong generation for MSM8998 |

**Total:** about 2,000 lines, 5-8 working days, 20-30 boots, and one short hand-off (turn the phone, M6).

## 5. Milestones (boot-arg `taimensensors=N`, each one boot; `TAIMEN_ADT_ARGS` with `sweetlogp=locationd,backboardd`)

| M | N | Proves | Passes when |
|---|---|---|---|
| 0 | 1 | iOS half, no DSP | `TAIMEN SENSORS: SweetHIDAccel up (0xff00/3)`; `SWEET HID: SweetHIDAccel ReportInterval=...` when locationd asks; locationd "New IOHIDServiceClientRef for usage pair {65280, 3}" and "Starting orientation detection"; with Safari open, `phonectl.py run 'orient left' 'sleep 2' 'shot s.png'` shows Safari in landscape |
| 1 | 2 | census, read only | `smem apps<->slpi part @..; 430/481/478/480 absent; pas supported(12)=1; slpi_v2.mdt 7452 + 15 segs, reloc Y, fits 0x94b00000+15M; sns.reg 28174` |
| 2 | 3 | SLPI runs | `rwsc turbo ok, lvs2 ok, aggre2 ok; pas init 0/0 mem 0/0 auth 0/0; 481 at +N ms, READY at +M ms` |
| 3 | 4 | GLINK, QRTR, registry | `dsps glink v1, chans IPCRTR ..; qrtr HELLO node N; svc 100 v1 inst ..; lookup 10f answered; sns_reg G groups served, U unknown` |
| 4 | 5 | SMGR census | one line per sensor: `smgr id 0x.. 'accel' name '..' vendor '..' max .. Hz range ..` (answers section 1) |
| 5 | 6 | accel stream | `accel 50 Hz: 50/s, x .. y .. z .., abs 9.8` at rest; flat face up gives one axis about +9.8 (fixes sign and axis map) |
| 6 | 7 | real rotation | the synthetic source is off; flat on the desk, backboardd logs "Effective device orientation changed to: ...FaceUp..."; hand-off (attention.sh, timeout): turn the phone with Safari open, and it rotates |
| 7 | 8 | gyro, CoreMotion | `tools/phone/motiontest`: `accel avail 1 gyro avail 1`, rates near 0 at rest |
| 8 | 9 | mag, prox (ALS if decoded) | motiontest heading moves; covering the top logs a backboardd proximity event; ALS only once its data type is understood |
| 9 | default | everyday | firmware from the root; streams only while ReportInterval > 0; survives a locationd restart; SLPI left running |

- One SLPI boot per phone boot until PAS shutdown is tested.
- Refuse to boot over stale state: stop if item 481 exists.
- Timeouts on every wait.
- Touch only partitions APPS is a host of (NOTES boot 109, AUDIO stage 2).

## 6. Files

| File | New/changed | What | Lines |
|---|---|---|---|
| X/iokit/Kernel/arm/hid/SweetHID.cpp | changed | `SweetHIDSensor` base (copyEvent = last event, setProperties ReportInterval/BatchInterval to a rate callback); Accel/Gyro/Compass/ALS/Prox registered on first use; `sweet_hid_motion(kind, x, y, z)`, `sweet_hid_als()`, `sweet_hid_prox()`, `sweet_hid_sensor_rate()` | ~300 |
| X/bsd/dev/sweet_usb.c | changed | control-port commands `accel X Y Z` (milli-g) and `orient portrait\|left\|right\|upside\|faceup` (next to `type`, :1191) | ~50 |
| port/tools/taimen/phonectl.py | changed | `accel`, `orient` ops | ~15 |
| X/iokit/Kernel/arm/TaimenSensors.cpp | new | N = 2-9: census, PAS 12, SMP2P pid 3, rails, GLINK dsps, QRTR, SNS_REG server, SMGR client, Q16 to g, rate control | ~1,300 |
| X/iokit/Kernel/arm/taimen_glink.{h,cpp} | new (optional) | TaimenAudio.cpp's GLINK/QRTR/QMI-TLV code (:841-1609) with an edge struct; copied, so audio keeps running untouched until it moves over | ~700 |
| X/bsd/dev/sweet_ufs.c | changed | `taimen_sensors_init()` after `taimen_audio_init()` | 3 |
| X/bsd/conf/files, X/iokit/conf/files.arm64 | changed | the new files | 2 |
| port/tools/taimen/reader/init | changed | copy `/vendor/firmware/slpi_v2.*` (3.9 MB) and `/vendor/etc/sensors/` if present (next to adsp.*, :80) | ~6 |
| port/tools/taimen/boot-xnu.sh | changed | `SENSORSFW=DIR` → blobs SLPI-MDT, SLPI-Bnn, SNS-REG | ~15 |
| port/tools/taimen/mkrootfix.py | changed | `--sensors-fw DIR` → /usr/standalone/firmware/taimen-sensors/ (as `--audio-fw`) | ~20 |
| port/tools/phone/motiontest.m | new | CMMotionManager accel/gyro/mag availability and 1 s prints (phone-tools CI) | ~80 |
| port/xnu/patches/03xx-taimen-sensors-*.patch | new | one per milestone | |

sns.reg comes from `debugfs -R 'dump /sensors/sns.reg ...' backup/persist.img`. It and slpi_v2.* are private
(research/taimen/firmware), never git.

## 7. Risks and unknowns

1. **Registry fit.** REG/'s group map may not match this SLPI build. Log every group id asked for and every miss.
   Serve the phone's own sns.reg, never a generic one.
2. **SMGR on 8998.** The instance and the meaning of the item's 3/1 constants are unverified. Google's SLPI image may
   differ from the msm8996 builds the open source was tested on.
3. **Sensor power.** Who powers the sensor chips: the SLPI through the RPM (unverified), or something the AP must vote?
   An empty ALL_SENSOR_INFO at M4 would point here.
4. **PAS.** If slpi_v2.mdt is not relocatable, it must load at its link address. Check that the address is the
   0x94B00000 carve-out at M1.
5. **Extra lookups.** The SLPI may look up services on other nodes (ADSP, modem) or AP services from Google's add-ons
   (Active Edge, CHRE; unverified). We do not route between edges. Answer "none" and log.
6. **sleepstate.** Its expected value is unknown; leave it unset at first (unverified).
7. **locationd matching.** It may require LocationID or the fast path (`IOHIDServiceClientFastPathInit`; our user client
   has no fast-path selectors). M0's locationd log shows what it checks before any DSP work.
8. **The orientation chain** (locationd → CoreMotion → backboardd) comes from strings. If M0 shows backboardd matching the
   service directly instead, follow its log (unverified).
9. **Units and axes** (Q16 m/s², NED frame, iOS g with a sign flip) are unverified until M5.
10. **ALS.** No open decoding of the SMGR prox_light's light data type exists. It may stay synthetic or off.
11. **Polling cost.** A third polled edge: start at 5 ms, move to SPI 178/179 through `sweet_spi_attach` if it shows in
    SWEET TOP.
12. **Constraints kept.** No diag, nothing from the modem. Only QMI services that open sources describe. No Apple
    binaries or device identifiers in git.

## 8. Cheat sheet

| Item | Value |
|---|---|
| SLPI | PAS 12; slpi_v2.mdt + b00-b14 (3,908,015 B); carve-out 0x94B00000 + 0xF00000; crash SMEM 424; wdog SPI 390; ssctl 0x16 |
| Votes | RPM "rwsc" id 0 level 0x180; "vsa" id 2 (LVS2 1.8 V); XO; aggre2 NoC clock |
| SMP2P (dsps) | pid 3; out 430 `master-kernel` (b0 stop), in 481 `slave-kernel` (b0 fatal, b1 ready, b2 handover, b3 stop-ack); `sleepstate` out; APCS bit 26, SPI 178 |
| GLINK (dsps) | APCS bit 27, SPI 179; SMEM 478/479/480 in APPS<->SLPI; channel `IPCRTR` |
| SNS_REG (ours) | svc 0x10F, v2, inst 0 (QRTR 0x0002); msg 0x04 group read: req 0x01 u16; resp 0x02 result, 0x03 id, 0x04 data ≤ 256 B |
| SNS_SMGR | svc 0x100, v1, inst 50 on 8996 (8998 unverified); msgs 0x05 all info, 0x06 single info, 0x21 buffering, 0x22 report; Q16.16; 32,768 Hz ticks |
| iOS usages | page 0xff00: accel 3, ALS 4, prox 8, gyro 9, compass 0x0A, DeviceMotionLite 0x10; page 0xff0C: DeviceMotion 4/5 |
| iOS events | accelerometer 13, gyro 20, compass 21, ALS 12, proximity 14 (X/.../IOHIDEventTypes.h) |
| iOS consumers | locationd CLIoHidInterface (ReportInterval, BatchInterval, LocationID); backboardd BKOrientationManager ← CMDeviceOrientationManager; IOHIDEventServicePlugin matches HIDServiceSupport |
