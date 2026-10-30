# Compass heading on taimen: what locationd wants

2026-10-30. Read-only reverse engineering of the iOS 16 `locationd` (out/rot-strings/iOSRoot/usr/libexec/locationd,
arm64, unencrypted). Nothing built or run. Addresses are vmaddrs. Pointers in `__DATA,__const` are chained fixups
(DYLD_CHAINED_PTR_64_OFFSET); the scratch tool that decodes them is `scratchpad/compass/mo.py`.

## 0. Verdict

- **CLCompass7 is chosen by the device identity. Nothing we control flips it.** The factory takes CLCompass7 when
  `CLPlatformInfo::motionCaps()` has bit 4 or bit 5 set. That mask is a fixed per-platform table keyed by the
  MobileGestalt identity (iPhone10,3). There is no defaults key for these bits, and no device-tree or IOKit input.
- **CLCompass7 does not compute a heading.** It forwards the heading, accuracy, level and attitude it receives in one
  88-byte vendor-defined HID event from a service with usage pair {0xff0c, 8}. The "not ready" lines only mean that
  no such service exists.
- **Cheapest path:** add a SweetHID service with primary usage {0xff0c, 8}. It follows ReportInterval and emits
  `IOHIDEvent::vendorDefinedEvent(ts, 0xff0c, 8, 0, report, 88)`. The kernel computes the tilt-compensated heading
  from accel and mag (gyro optional). The layout is in section 3.
- **Location Services are not needed for magnetic heading.** They are needed only for `trueHeading`, which stays -1
  without a location fix (declination).

## 1. Which compass class (Q1)

| What | Where | Logic |
|---|---|---|
| Factory | 0x1006e42c8 | `if (CLCompass7::isAvailable()) new CLCompass7 (0x128 B); else if (CLCompass6::isAvailable()) new CLCompass6 (0xe0 B)`; result to 0x101cd9460 |
| CLCompass::isAvailable | 0x1006e4334 | C7 \|\| C6 |
| CLCompass7::isAvailable | 0x1006bd158 | `motionCaps() & 0x20` \|\| `motionCaps() & 0x10` |
| CLCompass6::isAvailable | 0x1003dc250 | `0x1003b15c0()` (always 0) ? 0 : `motionProviders() & 1` (0x10073b2a8 to 0x1003b6180) |
| CLPlatformInfo::motionCaps | 0x1003b5670 | dispatch_once wrapper around queryMotionCaps |
| CLPlatformInfo::queryMotionCaps | 0x1003b6944 | `switch (platform)` (jump table 0x1003b6e40, 0xe1 entries) gives a constant mask. The only override is CFPreference `OverrideMotionCapEclipseService`, which touches bit 8 alone |
| platform enum | 0x1003ae60c, 0x1003abe5c | `MGCopyAnswer("IsSimulator")` else a chain of `MGIsDeviceOfType` checks; stored at 0x101cdf0e8 |

RTTI route: "10CLCompass6" @0x10140e610 → typeinfo 0x101b5fa28 → vtable 0x101b5f9b0 → ctor 0x1003dbd80 (C1
0x1003dc1e8). "10CLCompass7" @0x101463c40 → typeinfo 0x101b77518 → vtable 0x101b774c8 → ctor 0x1006bced4 (C1
0x1006bd094).

motionCaps bits, from the assert sites at 0x100d05f1c, 0x100d05fdc and 0x100d06070:

- bit 3 = deviceMotion6Service (CLSensorFusionService6AxisSPU);
- bit 4 = deviceMotionService (CLSensorFusionService9AxisSPU). The CLCompass7 ctor asserts on it (0x1006bcf50);
- bit 5 = makes CLCompass7 use usage 8 instead of 4 (below);
- bit 7 = deviceMotion3Service;
- bit 8 = Eclipse.

D22 evidently has bits 4 and 5: no assert fires, and the log shows {65292, 8}. To get CLCompass6, both bits would have
to clear. That means faking a different product, which also switches the sensor-fusion services, CLSPU and every
other platform-keyed path. It is not viable without patching locationd.

Side facts:

- `motionProviders()` (0x1003b6180) is a second per-platform table. On some platforms its bit 0 comes from
  `supportsCompass()` (0x1003b67b0): `IOServiceGetMatchingService({IOPropertyMatch: {name: "compass"}})`, which
  logs "Compass service missing" when absent.
- The factory checks CLCompass7 first, so none of this matters on D22.

## 2. The {0xff0c, 8} HID contract (Q2)

`CLCompass7::setDeviceMotionServiceUpdateInterval` (0x1006bd6a0, vtable[2] → 0x1006bd3ec):

- usage = (this+136 && motionCaps bit 5) ? 8 : 4;
- this+136 = 1 except during a visionOS AR session (onLocationNotification 0x1006be5bc, notification 0x20);
- the device is created with pair {0xff0c, usage, 0} (0x101316a34) and callback 0x1006bd9cc;
- it then sets `ReportInterval` = round(interval × 1e6) µs (0x1006b5d40): 20000 for the 0.02 s seen;
- interval 0 stops it;
- no other property is set on it.

Per HID/ (IOHIDFamily-870.1.10), usage 4 on 0xff0C is DeviceMotion. Usage 8 has no name there.

CLIoHidInterface:

- **"not ready"**: Device::initializeServiceRef (0x1006b5918) finds no service.
- **Matching:**
  - Device::matches (0x1006b3250) = `IOHIDServiceClientConformsTo(svc, 0xff0c, 8)` (DeviceUsagePairs) plus
    matching properties (nil for this device, so they always match);
  - event delivery uses `IOHIDEventSystemClientSetMatchingMultiple` with {PrimaryUsagePage, PrimaryUsage}
    (0x1006b2b4c);
  - so publish PrimaryUsagePage=0xff0c, PrimaryUsage=8 and DeviceUsagePairs [{0xff0c, 8}];
  - no built-in, LocationID or other property is checked.
- **Dispatch**: CLIoHidInterface::onEvent (0x1006b4620) calls a device's callback when `sender == device's
  serviceRef` (event-type routing is off: Device+144 = -1).
- **"Could not set all properties ... Giving up"** is the ReportInterval write that had no service to go to. It also
  covers CLSPU's {0xff0c, 1}, which comes from CLSPU::CLSPU and plays no part in heading.

Event handler `CLCompass7::onDeviceMotionServiceIohidEvent` (0x1006bd9cc):

1. `IOHIDEventGetVendorDefinedData(event, &data, &len)`. The **top-level event must be kIOHIDEventTypeVendorDefined**.
   Otherwise it logs "Empty payload" and returns.
2. Timestamp = `IOHIDEventGetTimeStamp(event)` (mach time). **The sensor-time child is not read.** Attaching one does
   no harm.
3. 0x1006bdb14 parses the payload as `CMDeviceMotionReport`:
   - reader 0x1002152fc: byte 0 = type 1..4;
   - the length must equal sizes[type] exactly (table 0x101402e20: 1→5, **2→88**, 3→52, 4→52); otherwise the type
     becomes 0xff and no visitor runs;
   - visitor slot +24 (type 2, `CMDeviceMotionReport::Compass`) = 0x1006be77c, which copies the 88 bytes.
4. Check (this+132 is always 0, so never taken): if this+132 != 0 and u64@4 != this+132, the event is dropped.
5. `populateMotionType` (0x1006bdc60) builds `CLMotionTypeCompass` and logs "Type,GyroCompass,...".
6. **The sample is dropped unless flags bit 8 (`initialized`) is set** (0x1006bdbd4). Otherwise it notifies clients
   with a 0x70-byte CLCompass_Type::Sample, and CLHeadingSubscription::onCompass (0x100beacb0) sends the heading. Its
   only filter is the client's headingFilter. It checks no accuracy, level or authorization.

## 3. CMDeviceMotionReport::Compass (88 bytes, packed, little-endian)

| Off | Type | Field (from populateMotionType and its log format) |
|---|---|---|
| 0 | u8 | type = **2** |
| 1 | s8 | calibration level. ≥2 counts as calibrated (onProactiveCompass 0x100cd9ecc); use 3 |
| 2 | u16 | flags, listed after this table. Set at least 0x0100 |
| 4 | u64 | compared with CLCompass7+132 (always 0, so ignored); write 0 |
| 12 | f32 | copied to Sample+96. The legacy CLCompass6 path writes -1.0 there; write -1.0 |
| 16 | f32[3] | uncalibrated magnetic field x,y,z, µT, device frame |
| 28 | f32[4] | attitude quaternion x,y,z,w |
| 44 | f32[3] | rotation rate, rad/s (logged ×57.29578). Its norm goes to Sample+100 |
| 56 | f32[3] | user acceleration, g (probably; it goes only to the binary log). Zeros are fine |
| 68 | f32 | **magnetic heading, degrees [0,360)**, passed straight through to CLHeading.magneticHeading |
| 72 | f32 | **heading accuracy, degrees**. -1 = invalid. locationd clamps it to [10,180] (0x1006be548) and adds asin(5/\|H\|) only when a cover is attached |
| 76 | f32[3] | magnetometer bias, µT. CLHeading.x/y/z = field(16) − bias(76) |

Bits of the flags field at offset 2:

| Bit | Meaning |
|---|---|
| 0 | gyroSaturated |
| 1 | accelSaturated |
| 2 | gravityCorrection |
| 3 | yawCorrection |
| 4 | biasEstimation |
| 5 | courseCorrection |
| 6 | visualLocalizationCorrection |
| **8** | **initialized (required)** |
| 9 | gyroCalibration |
| 10 | compassCalibration |
| 11 | course |
| 12 | gmm |
| 13 | north |
| 14 | visualLocalization |

What locationd derives (helpers 0x1007b41e4 and 0x1007b4274):

- With R = R(q) (standard unit-quaternion matrix), gravity = −row 2 and the north unit vector = row 0.
- So q is the device→reference rotation of CoreMotion's XMagneticNorthZVertical frame: rows are North, West, Up
  expressed in device coordinates. Identity = flat, face up, device +X pointing north.
- Gravity, inclination and field magnitude are derived from q and the calibrated field. The heading is not.
- trueHeading = wrap360(heading + declination) only after the geomagnetic model has a declination. That needs a
  location; until then trueHeading = -1.

## 4. Kernel recipe (accel + mag; gyro optional)

Use iOS device axes (X right, Y top, Z out of the screen), the same axes the accelerometer and magnetometer services
already use.

```
g  = low-passed accel (iOS convention: (0,0,-1) g face up)    up = -normalize(g)
mc = mag_uT - bias                                             (bias = hard-iron estimate, or 0)
E  = normalize(cross(mc, up))      N = cross(up, E)            rows of R: N, -E (west), up  -> q (Shepperd)
heading = atan2(E.y, N.y)                     when |up.z| >= 0.5   (device +Y, roughly flat)
        = atan2(-E.z, -N.z)                   otherwise            (back camera, -Z, held upright)
heading_deg = fmod(deg(heading) + 360, 360)
report: type 2, level 3, flags 0x0100, u64 0, f32@12 -1.0, field@16 = raw uT, q@28, gyro@44 (rad/s or 0),
        0@56, heading@68, accuracy@72 (e.g. 15; -1 when |mc| is outside about 20..70 uT), bias@76
event:  IOHIDEvent::vendorDefinedEvent(mach_absolute_time(), 0xff0c, 8, 0, report, 88, 0)
service: SweetHID sensor, PrimaryUsagePage 0xff0c, PrimaryUsage 8, DeviceUsagePairs [{0xff0c,8}],
         streams at ReportInterval (us; 0 = stop), like the other SweetHID sensors
```

The upright switch to −Z copies the usual iPhone behaviour. It is unverified for this build; +Y alone already works
for the flat case.

## 5. Q3: gyro, device motion, Location Services

- No locationd fusion runs on the CLCompass7 path. "GyroCompass" is only the log label. Nothing asks for locationd's
  device-motion or gyro services. The CLCompass6 path does ("Compass is requesting device motion"), but it is not
  used here. Gyro only improves our own kernel filter.
- Location Services:
  - the heading subscription started while location was "disabled system-wide";
  - neither CLCompass7 nor CLHeadingSubscription::onCompass checks authorization;
  - Location Services matter only for trueHeading (declination).
  - Unverified: what the Compass app shows when "Use True North" is on and trueHeading = -1. Its binary is not in
    out/rot-strings.
