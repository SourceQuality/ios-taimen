# Sound on taimen (TAS2557 speakers through the ADSP): implementation plan

2026-10-25. Plan only, nothing built or run. Reference keys used below:
`DTS` = research/taimen/live.dts, `dmesg` = hw/dmesg.txt (LineageOS boot on this phone),
`ADT` = `tools/adt.py dump research/ipsw-x/d22ap.adt` (iPhone X), `ROOT` = research/ipsw-x/ios16-root.img
(read with 7-Zip, as tools/taimen/mkrootfix.py does), `DSC` = research/dsc-x (shared cache, tools/sweetgl/dsc-*.py),
`W/` = research/dl/msm-wahoo/, `X/` = scratch/xnu/, `TZ/` = ref/ios-topaz/xnu/patches/,
`TU/` = ref/ios-topaz/userland/audio/, `M/` = research/dl/mainline-6.1/, `NOTES` = port/NOTES-taimen.md.
Not stored locally (read online on 2026-10-25, cite by file name):
- `HK/` = HackiOS kernel, github.com/oosexgang/hackios-xnu-kernel, `xnu/files/bsd/dev/redwood_{adsp,sound,speaker,audio}.c`.
  It plays iOS 16's sound through an Elite (APR) ADSP on SM7325 with the same iPhone X userland we run.
- `WH/` = the wahoo kernel (LineageOS android_kernel_google_wahoo, lineage-22.2):
  `sound/soc/msm/msm8998.c`, `sound/soc/codecs/tas2557/tas2557-core.c`, `drivers/soc/qcom/qdsp6v2/{apr_tal_glink,msm_audio_ion}.c`.
- `L/` = Linux v6.1: `sound/soc/qcom/qdsp6/{q6afe,q6asm,q6adm}.c`, `q6afe.h`, `drivers/pinctrl/qcom/pinctrl-msm8998.c`.

## Status (2026-10-25, boots 156-160): the speakers play iOS's audio, on by default

| Stage | Patch | Result |
|---|---|---|
| 1 firmware | reader init, boot-xnu.sh AUDIOFW | adsp.mdt + 8 segments, TAS2557 firmware and calibration as payload blobs (~9.4 MB) |
| 2 ADSP by PAS | xnu 0317 | READY 12-13 ms after AUTH_AND_RESET (relocatable image into the 0x8B200000 carve-out) |
| 3 GLINK, QRTR, APR | xnu 0318 | lpass edge up; the ADSP opens apr_audio_svc and five more channels; pd-mapper answers no domains |
| 4 AVCS ready | xnu 0318 | core state 1; AFE 0x260000, ASM 0x70002, ADM 0x70000 |
| 5 LPASS SMMU | xnu 0319-0324 | no non-secure bypass (S2CR bypass reads back as fault); the top bank (CB12) hung the bus; the lowest free bank (CB1) as an identity bank works; TZ MP_RESTORE_SEC_CFG answers -2 and is not needed |
| 6 AFE port 0x1006 | xnu 0319, 0323 | S24_LE in 32-bit slots at 3.072 MHz as Android's mixer paths; SCK/WS toggle |
| 7 TAS2557 | xnu 0323 (taimen_i2c.c, taimen_tas2557.c) | PG2.1 on both, program 0 / configuration 0 / calibration 0 in 2.1 s, no CRC mismatch; up only while the clock runs |
| 8 ADM + ASM stream | xnu 0321 | the 3 s tone: 159 periods out and back, 0 underflows (now only with taimenaudiotone=1, 0327) |
| 9 /dev/redwood-audio, feeder | xnu 0323 | sessions open on frames and close after 3 s; 0 dropped, 0 DSP underflows |
| 10 RedwoodAudio plug-in | tools/phone/RedwoodAudio, mkrootfix --hal-plugin | loads in mediaserverd on the install boot; Safari's MP4 audible |

Defaults since xnu 0326: taimenaudio=9 with the stream 1 bank; the default GPU ROOTFIX carries the plug-in.
Level cap taimenaudiobelow=160 (-20 dB) until it has been heard. Since xnu 0330 the firmware comes from
/usr/standalone/firmware/taimen-audio in the iOS root (installed once with `mkrootfix.py --audio-fw`, boot 162),
so boot-xnu.sh adds the payload blobs only with AUDIO=1 (a phone without them in its root).
Haptics: the DRV2624 behind /dev/redwood-haptic (xnu 0328) plays iOS's Actuator stream since boot 165
(mkadt.py's /product/haptics, MobileGestalt cache rebuilt once).
Not yet: microphones (WCD9340 over SLIMbus), earpiece routing for calls.

## 0. Verdict in one paragraph

This is feasible, and both halves have been done before.
- **iOS side.** Do not imitate Apple's audio kexts. Install a user-space HAL plug-in (AudioServerPlugIn) in mediaserverd:
  HackiOS's RedwoodAudio, already in TU/. It presents the devices the iPhone X's VirtualAudio expects ("Speaker",
  "Hawking", "Actuator", "Codec") and writes the mixed 48 kHz s16 stereo to a character device, `/dev/redwood-audio`.
  All the kernel owes iOS is that device. No IOKit class is needed.
- **Taimen side.** The speakers are two TI TAS2557 smart amps (dual mono, I2C 0x4C/0x4D on BLSP2 QUP1). They take I2S from
  LPASS quaternary MI2S, and the ADSP drives that port. The WCD9340 codec and SLIMbus are **not** on the speaker path.
  So the chain is:
  1. Boot the ADSP by TrustZone PAS (id 1), as we already boot the A540 zap shader.
  2. Bring up SMP2P and GLINK on the "lpass" edge, then APR on the `apr_audio_svc` channel.
  3. AFE: start port 0x1006 (quaternary MI2S RX).
  4. ADM COPP and an ASM PCM stream, fed from a shared buffer the ADSP reaches through the LPASS SMMU (stream 1).
  5. Load the TAS2557 DSP firmware over I2C and power the amps up while the bit clock runs.

  HackiOS's `redwood_sound.c` is a working APR recipe (TDM there, MI2S here). Topaz's 0144/0146 supply the PAS/SMP2P/GLINK
  skeleton and the `/dev` node. Topaz's AudioReach (GPR/APM) graph code does not apply: msm8998 runs the older Elite APR firmware.

**Hardest stages:**
- the LPASS SMMU stream for the shared buffers (stage 5): ABL's state is unknown, and TZ may own some of it;
- the TAS2557 firmware loader and clock format (stage 7).

**First milestone:** M0, a read-only census (section 5).

## 1. The iOS side: what iOS 16 needs from the kernel

### 1.1 The iPhone X's audio hardware, as its device tree describes it

| ADT node | compatible / type | What it is |
|---|---|---|
| `/arm-io/spi2/audio-codec` | `audio-control,cs42l75` | Cirrus codec: mics, receiver, haptics PDM, "Hawking" ADC, MCLK 24 MHz, ASP routing |
| `/arm-io/i2c2/audio-speaker-top` | `audio-control,cs35l26` @0x40 | speaker amp (`primary-speaker 'TopS'`, `secondary-speaker 'BotS'`, `speaker-calib` from syscfg) |
| `/arm-io/mca-switch`, `/arm-io/mca0`..`mca5` | `mca-switch,t8015`, `mcaCluster,t8015`, `mca,t8015` | Apple's I2S clusters with SIO DMA (`dma-parent`) |
| `mca0a/audio-codec-voice` | `audio-data,mikeybus-secondary` | data source "Codec" |
| `mca1a/audio-codec` | `audio-data,cs42l75` | codec data |
| `mca2a/audio-speaker-top` | `audio-data,cs35l26` | speaker data |
| `mca2b/audio-loopback` | `audio-data,audio-loopback` | "Loopback" |
| `mca3a/audio-baseband-voice` | `audio-data,baseband-voice` | "Baseband" |
| `mca3b/audio-halogen` | `audio-data,halogen` | "Halogen" |
| `mca4b/audio-codec-hawking` | `audio-data,hawking` | Hawking (a codec input) |
| `mca5a/audio-haptic` | `audio-data,actuator` | "Actuator" (Taptic Engine) |
| `mca0b`, `mca4a` | `audio-data,dsp-debug1/2` | debug taps |
| `/arm-io/aop/iop-aop-nub/aop-audio` | `aop-audio` (+ `audio-aop-mca2`, `-bhap`, `-buttonout`) | always-on processor: "Hey Siri" |
| `/product/audio` | `wireless-splitter`, `supports-always-listening` | product flags |

Apple's closed kexts match these nodes. They are in the kernelcache, which is not extracted locally. If the names are
ever needed, tools/remotezip.py can take the kernelcache out of the IPSW in research/ipsw-x/URL.txt.

What user space expects of those kexts is visible in the shared cache:
- CoreAudio's HAL has `HALS_IOA2Device` / `HALS_IOA2UCDevice` and `AMCP::IOAudio2::{Driver,Device,Engine,Stream,Control}`.
  Its own error strings say "no IOAudio2Device" and "this is not an IOAudio2Device".
- So built-in audio arrives as **`IOAudio2Device` services from IOAudio2Family**. Each has engine user clients and a shared
  engine status (`IOAudio2EngineStatus`, `IOAudio2Notification`).
- VirtualAudio also uses Apple-embedded property selectors (`kAppleEmbeddedAudio_DevicePropertyInputEnableDevice`).
- None of IOAudio2Family is open source. Re-creating its user-client ABI would mean reverse engineering the HAL's
  IOAudio2 code with no reference. **Ruled out.**

### 1.2 How iOS 16.7 (20H392) finds audio devices

- **The audio server is mediaserverd.** iOS 16 has no audiomxd, which arrived in iOS 17: ROOT has only `/usr/sbin/mediaserverd`
  and `audioclocksyncd`. mediaserverd's job (ROOT `System/Library/LaunchDaemons/com.apple.mediaserverd.plist`) owns
  `com.apple.audio.AudioSession`, `AURemoteIOServer`, `SystemSoundServer-iOS`, `hapticd` and others. The HAL runs inside it
  (CoreAudio.framework, AudioServerDriver/AudioServerApplication, MediaExperience, AudioSession(Server) in DSC).
- **HAL plug-in directories** (CoreAudio strings): `/System/Library/Audio/Plug-Ins/HAL`, `/Library/Audio/Plug-Ins/HAL`,
  `/Library/Apple/Audio/Plug-Ins/HAL`. ROOT has:
  - `/System/Library/Audio/Plug-Ins/HAL/{AirPlayHalogen,AppleAOPAudioPlugin,AppleTimeSyncAudioClock,AppleUSBAudio,
    BTAudioHALPlugin,BasebandVoice,CarPlayHalogen,NetworkUplinkClock,OctaviaHalogen}.driver`;
  - `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin`.
- **Plug-in Info.plist keys** (the Apple bundles above): `CFPlugInTypes` `443ABAB8-E7B3-491A-B985-BEB9187030DB` (the
  AudioServerPlugIn type), `CFPlugInFactories`, and optionally:
  - `AudioServerPlugIn_LoadingConditions` (AppleAOPAudioPlugin: `IOService Matching` `AppleAOPAudioController`);
  - `AudioServerPlugIn_IOKitUserClients`;
  - `AudioServerPlugIn_MachServices`.

  A plug-in with no loading condition always loads (AirPlayHalogen, CarPlayHalogen).
- **VirtualAudio.plugin (the "VAD")** is the router every app goes through. It knows an iPhone's devices by their UIDs.
  Its strings include `Speaker`, `Codec`, `Hawking`, `Actuator`, `Receiver`, `Halogen`, `Baseband` and `Null_Device`, and it
  reads `IODeviceTree:/product/audio`. It builds the speaker route only when there is a "Speaker" device *and* its mandatory
  companion: a 'vhaw' virtual device on a "Hawking" input (TU/audio_plugin.c:22-26). The route runs through the D22 tunings,
  `ROOT /Library/Audio/Tunings/D22/VAD/d22_speaker_general.austrip` and the others there.

### 1.3 The choice: a HAL plug-in plus a character device (the HackiOS ABI)

- TU/audio_plugin.c (1,041 lines, MIT, HackiOS) and TU/audio_plugin.plist are the plug-in, with CFBundleExecutable `RedwoodAudio`
  and factory `RedwoodAudio_Create`. It was proven on iOS 16 with the D22 userland (HackiOS on redwood; topaz, README:13).
- It presents four built-in (`'bltn'`) devices:
  - "Speaker": stereo out to `/dev/redwood-audio`, plus a silent stereo in;
  - "Hawking": one silent input;
  - "Actuator": one channel out to `/dev/redwood-haptic`;
  - "Codec": one input from `/dev/redwood-mic`, data source `'imic'`.
- The clock is mach time at exactly 48,000 frames/s, with a timestamp period of 16,384 frames.
- The speaker has a 48 dB volume control. VirtualAudio keeps the D22 speaker in software-volume mode, so iOS scales the
  samples itself and sets the control once, to 0 dB (TU/audio_plugin.c:44-58).
- HackiOS needed no `/product/audio`: its loader copies only `/product`, `/product/haptics` and `/product/camera` from the
  real tree (HK xnuloader/mkidentity.py). `/product/haptics` (`closed-loop`) only matters for the Actuator.

**What the kernel must provide** (TU/redwood_audio.h; topaz TZ/0146 + TZ/0935):

| Node | ABI | Needed for |
|---|---|---|
| `/dev/redwood-audio` (char, 0666) | `write()` whole frames, s16le L/R, 48 kHz; **never blocks** (the HAL's real-time thread writes); what the sink cannot take is dropped. `ioctl(_IOW('r', 1, struct {u32 below; u32 muted}))`: volume, eighths of a dB under the loudest | speaker output (M6, M7) |
| `/dev/redwood-mic` | non-blocking `read()`; the kernel listens only while it is open | later (microphones, section 7) |
| `/dev/redwood-haptic` | `write()` of the actuator waveform | later (DRV2624 motor, section 7) |

The plug-in copes with the last two missing: the mic input is then silence and the actuator writes are dropped.
Nothing else is needed from IOKit.

### 1.4 Install, signing, sandbox, logging on taimen

- **Taimen already has what the plug-in sits under.** VirtualAudio.plugin is in place: mkiosroot.sh and mkrootfix.py leave it.
  Only the Pixel 1's sweet-root-tweaks.py:146-152 moved it aside. mediaserverd runs once per boot without crashing
  (out/taimen-xnu-r133-*: one `exec mediaserverd`, no `CRASH mediaserverd`). StartRecorderServer is already off (mkrootfix.py:214-226).
- **Install by ROOTFIX** (no reflash): `--mkdir /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver`, then `--write` its
  Info.plist (op 1, rewritten every boot) and `--zfile` the binary.
  - Unlike sweetgl, **do not version the bundle directory**: the HAL loads every `.driver` it finds, so two versions would both
    load. Version the executable (`RedwoodAudio-HASH`) and point `CFBundleExecutable` at it in the rewritten Info.plist.
  - Remove the old executable with `--remove`.
  - ROOTFIX records apply when launchd remounts the root (mkrootfix.py:14-17), and mediaserverd may scan its plug-ins before
    that. So on the install boot the plug-in may first load only after mediaserverd restarts; from the next boot it is there.
- **Build** with the phone tools: tools/phone/build.sh on the phone-tools CI (Xcode iPhoneOS SDK). Make it an `MH_BUNDLE`
  (`-bundle -framework CoreFoundation`), signed ad hoc.
- **Signing.** Ad hoc code already loads into platform processes on this kernel: sweetgl in backboardd (NOTES:517), and tcctest
  reads csops `VALID|SIGNED|PLATFORM` (NOTES:1055). So no library-validation failure is expected (open question 10).
- **Sandbox.** The permissive SWEET Sandbox policy lets mediaserverd open `/dev/redwood-audio`.
- **Logging.** The plug-in writes its `redwood: audio: ...` lines to stderr.
  - `sweetlogp=mediaserverd` (xnu 0308) tees them, with mediaserverd's os_log, into the kernel log.
  - `sweetiolog=mediaserverd` (X/iokit/Kernel/IOUserClient.cpp:2606) logs what the HAL looks for in IOKit.
  - The command line is full: put both in `TAIMEN_ADT_ARGS` (xnu 0309, NOTES:1044).

## 2. Facts that fix the taimen design (the speaker path)

- **The sound card and its BE** (DTS:15792-15823): the card is `msm8998-tavil-taimen-snd-card` with
  `wahoo,use-external-amp`.
  - Its BE stream is `"Quaternary MI2S Playback"`, and the codec DAI is `"tas2557 Stereo ASI1"`.
  - `qcom,msm-mi2s-master = <1 1 1 1>`: the SoC is clock master.
  - The muxsel registers are `lpaif_{pri,sec,tert,quat}_mode_muxsel` at 0x1711A000/B000/C000/**D000**.
  - WH msm8998.c writes **0** (I2S mode) to the quat muxsel at the first startup.
  - The bit clock is `48000 x 2 x bits`: S16_LE (the default `mi2s_rx_cfg[QUAT_MI2S]`) gives 1.536 MHz; S24/S32 give 3.072 MHz.
  - The clock goes out through `afe_set_lpass_clock_v2` with `Q6AFE_LPASS_CLK_ID_QUAD_MI2S_IBIT`, attribute COUPLE_NO, as
    `SND_SOC_DAIFMT_CBS_CFS`.
- **The quaternary MI2S DAI** (DTS:12269-12278): dev-id 3, `rx-lines 0x2` (playback on SD1), `tx-lines 0x1` (SD0 carries the
  amps' I/V feedback).
- **Pins** (DTS:11624-11718), all in the TLMM WEST tile at 0x03500000 + 0x1000·n:
  - gpio58 SCK and gpio59 WS: active = `qua_mi2s`, 8 mA, output-high;
  - gpio60 SD0 and gpio61 SD1.
  - Function select is 1 for all four (L/ pinctrl-msm8998.c PINGROUP 58-61).
- **The amps** (DTS:15069-15084; dmesg:818-827, 1533-1563, 2167-2171, 2264, 2579-2580):
  - node `ti,tas2557s` at `i2c@c1b5000` (= i2c-7, BLSP2 QUP1, DTS:14936-14955, SPI 101, 400 kHz from XO);
  - left 0x4C, right 0x4D;
  - shared reset = PM8998 GPIO 6 (SPMI 0xC500; at boot an input with the 30 µA pull-up, DTS:813-824);
  - IRQs on PM8998 GPIO 7/8;
  - `ti,i2s-bits = 24`, `ti,cal-file-name = "tas2557_cal.bin"`.
  - Android found **PG2.1**, loaded `tas2557s_PG21_uCDSP.bin` (51,609 bytes, PPC 0x5C000, driver version 0x0300) and the
    550-byte calibration. It chose program 0 "Tuning Mode" (blocks 0x0D, 0x01, 0x08), configuration 0 "MusicPlayback"
    (blocks 0x04, 0x0B, 0x03, 0x0A), then calibration (0x03, 0x0A).
  - The same bus carries the smb138x parallel charger at 0x08 (unused by us).
  - The DTS tfa98xx nodes belong to walleye and are disabled (DTS:15051-15068).
- **The ADSP** (DTS:3906-3929):
  - `qcom,pil-tz-generic`, PAS id 1, firmware "adsp";
  - proxy votes: `vdd_cx` level 0x180 (TURBO) and XO;
  - `qcom,smem-id` 423 (crash text);
  - carve-out `pil_adsp_region` **0x8B200000 + 0x1A00000** (DTS:16417-16422; below XNU's RAM at 0xb1000000, NOTES:713);
  - firmware in **/vendor/firmware**, not modem_a: `adsp.mdt` (7,260 B = b00 + b01) plus b02-b06, b08, b09 and b11
    (b07 and b10 absent); 9,711,112 bytes in all (research/taimen/firmware/list.txt).

  Android loads it by PAS: dmesg:2044-2048 ("subsys-pil-tz 17300000.qcom,lpass: adsp: loading"), 2172 ("Brought out of reset"),
  2173-2177 (`apr_tal_link_state_cb: edge[lpass]`), 2228 (SSCTL).
- **The audio service is in the root PD.** Android's PDR lookup of `avs/audio` finds 0 domains (dmesg:3013-3021), and our
  pd-mapper already answers the modem the same way (NOTES:832).
- **SMP2P and GLINK** (DTS:313-319, 3529-3538; M/arch_arm64_boot_dts_qcom_msm8998.dtsi:390-412, 2450-2480):

  | | Value |
  |---|---|
  | SMP2P | remote pid 2; in 443, out 429; `slave-kernel` (b0 fatal, b1 ready, b2 handover, b3 stop-ack), `master-kernel` (b0 stop); APCS IPC 0x17911008 **bit 10**; SPI 158 |
  | GLINK edge "lpass" | APCS **bit 9**, SPI 157; SMEM 478/479/480 in the APPS<->ADSP partition (SMEM + 0x1cf000, 0x30000 bytes, NOTES:794) |
  | wdog | SPI 162 |

- **APR** rides GLINK channel **`apr_audio_svc`** on edge "lpass" (WH apr_tal_glink.c). Packets are never split, and the
  DTS `apr_apps2` glinkpkt is a user-space device we ignore. IDs (HK redwood_adsp.c, redwood_audio.h; the same as Linux dt-bindings qcom,apr.h):

  | Kind | Values |
  |---|---|
  | Domains | APPS 5, ADSP 4 |
  | Services | ADSP core 3, AFE 4, ASM 7, ADM 8 |

- **Shared memory goes through the LPASS SMMU.**
  - msm-audio-ion (DTS:12522-12528) is `smmu-version 2` with `iommus = <lpass_q6_smmu 1>`.
  - The LPASS Q6 SMMU is at **0x05100000 + 0x40000**: `qcom,skip-init`, `qcom,tz-device-id "LPASS"`, vdd
    `gdsc_hlos1_vote_lpass_adsp` (GCC 0x17D034), clock `hlos1_vote_lpass_adsp_smmu_clk` (GCC CBCR 0x17D014)
    (DTS:5331-5346, 562-577; W/include_dt-bindings_clock_msm-clocks-hwio-8998.h:224).
  - WH msm_audio_ion.c maps buffers at IOVA 0x10000000+ and hands the DSP **`iova | (1 << 32)`** (SID 1 in bits 32 and up).
    With the SMMU off it hands it the plain physical address. There is no hyp_assign.

## 3. Bring-up chain, stage by stage

Each stage lists what it touches, what the tree already has, what to write (rough lines) and the risks. One boot-arg,
`taimenaudio=N` (level, section 5), goes in `TAIMEN_ADT_ARGS`.

### Stage 1. Firmware delivery

| File | Source | Size |
|---|---|---|
| `adsp.mdt`, `adsp.b02..b11` | /vendor/firmware | 9.26 MiB |
| `tas2557s_PG21_uCDSP.bin` | /vendor/firmware | 51,609 B |
| `tas2557_cal.bin` | /persist/audio (DTS name; a symlink in /vendor/firmware) | 550 B |

- Not pulled yet. Add them to the reader's vendor copy (tools/taimen/reader/init:70-77, next to the WLAN files). Also copy
  `/vendor/etc/mixer_paths*.xml` and `audio_platform_info.xml` for open question 5.
- The calibration comes out of `../backup/persist.img` (ext4, `audio/tas2557_cal.bin`; 7-Zip reads it). It is phone-specific:
  `research/taimen/firmware`, never git.
- **For the bring-up: payload blobs.** boot-xnu.sh gets `AUDIOFW=DIR` → `--blob ADSP-MDT`, `ADSP-B02`..`ADSP-B11`, `TAS-FW`,
  `TAS-CAL`, the way GPU-* and WLAN-* are passed (tools/taimen/boot-xnu.sh:94-114). The kernel reads them with the same
  `/chosen/memory-map` lookup as X/bsd/dev/sweet_gpu.c:400 `blob()`.
  - Budget: the payload was 21.4 MB with the WLAN blobs (NOTES:827). +9.3 MiB is about 31 MB, under ABL's ~35 MiB
    (NOTES:628-630).
  - **Never combine it with a sweetgl ROOTFIX install.**
- **For the default boot (M8): from the root.** ROOTFIX `--zfile` copies the files once into `/usr/standalone/firmware/taimen-audio/`.
  The audio thread starts after the root is mounted and reads them through the vnode KPIs (`vnode_open`/`vn_rdwr`), as
  X/bsd/kern/taimen_rootfix.c already writes files. Keep the blob path as the override.
- **Risk:** low.

### Stage 2. ADSP boot by PAS

1. **Proxy votes:**
   - CX TURBO through `sweet_rpm_rail_level` (X/bsd/dev/sweet_rpm.c:418), as M3 does for the modem;
   - XO stays on while APPS runs (cxo_pil_lpass_clk is only a voter on cxo, W/clock-gcc-8998.c:79);
   - keep both for the run instead of dropping them at handover (bit 2).
2. **Refuse to boot over stale state** (topaz lesson, TZ/0144 header): if item 443 or 480 exists in the APPS<->ADSP partition,
   log it and stop.
3. **SMP2P out:** create item 429 (header `$SMP`, v1, local 0, remote 2, features SSR_ACK, 16 entries) with
   `master-kernel` = 0. Kick bit 10. HackiOS: swapping 429/443 makes the DSP never report up.
4. **PAS** (svc 2; X/bsd/dev/sweet_gpu.c:574-717 is the same flow for the zap, id 13):
   1. `IS_SUPPORTED(1)` (cmd 7, log only);
   2. `INIT_IMAGE` (cmd 1: id 1, the pa of the mdt copied 4K-aligned into the audio pool, below);
   3. `MEM_SETUP` (cmd 2: 1, 0x8B200000, 0x1A00000);
   4. copy each PT_LOAD non-hash segment `.bNN` to its address, relocated into the carve-out if
      `MDT_RELOCATABLE` (bit 27, W/drivers_soc_qcom_peripheral-loader.c:344-391, 481-530), zero-filling up to memsz;
   5. `AUTH_AND_RESET` (cmd 5: 1).

   Map the carve-out only while copying, then unmap it.
5. **Wait** for item 443 `slave-kernel` bit 1 (READY), at most 5 s. HackiOS: the item appears about 17 ms after TZ returns
   and READY about 41 ms after. On bit 0, print SMEM item 423.

- **Audio pool:** 0x8A000000 + 1 MB, Normal-NC like the modem pool (X/iokit/Kernel/arm/TaimenModem.cpp:67-68, 156-180).
  - It is inside the 28 MB of Linux RAM that XNU leaves alone (0x88f00000-0x8aafffff, below SPSS 0x8AB00000).
  - It is separate from the modem pool, so sound does not depend on `taimenwifi`.
  - Check at M0 that ABL does not touch it (pattern test).
- **Have:**
  - SCM `scm()` and `scm_assign()` (TaimenModem.cpp:183-255);
  - SMEM partition/lookup/alloc with the TCSR hwlock (260-354, 1006-1058);
  - SMP2P helpers (1063-1147);
  - the APCS doorbell (1060-1075).

  All are `static`: export them as `taimen_smem_partition/get/alloc`, `taimen_apcs_kick(bit)`, `taimen_scm()`, mapping lazily
  so they work without the modem thread, as TZ/0144:1345-1525 did for TopazModem.
- **Write:** about 220 lines (PAS loader with split segments, SMP2P for pid 2, votes).
- **Risks:**
  - Only touch partitions APPS is a host of: reading another pair's header hung a CPU (NOTES:788-796).
  - Share the hwlock with the modem thread.

### Stage 3. GLINK "lpass", minimal QRTR, the APR channel

- **GLINK** over SMEM in the APPS<->ADSP partition:
  - descriptor 478, our 16 KB TX FIFO 479, the DSP's FIFO 480 (look in the cached list too, as 0279 learned);
  - VERSION 1, features INTENT_REUSE.
  - Use **TZ/0144:383-915**, topaz's self-contained multi-channel GLINK (channels by name, an RX intent table, a TX queue),
    with the lpass constants. Keep it inside TaimenAudio.cpp instead of reworking TaimenModem's single-edge globals
    (TaimenModem.cpp:1149-1887), so Wi-Fi keeps working; merge the two later.
- **Channels:**

  | Channel | Our intents (HackiOS) | Notes |
  |---|---|---|
  | `IPCRTR` | 0x800 x 5, 0x2000 x 3, 0x4400 x 2 | |
  | `apr_audio_svc` | 0x200 x 8, 0x1000 x 4, 0x2000 x 2 | |
  | anything else | none | open-ack it and leave it idle (0290's lesson on the modem edge, NOTES:908-910) |

  Send DONE_REUSE after each packet. Send INTENT_REQUEST when the DSP has no intent that fits; wait at most 5 s.
- **QRTR:**
  - answer the DSP's HELLO;
  - log NEW_SERVER (expect SSCTL 0x2b instance 0x14, DTS:3921);
  - answer a servreg-locator lookup with "0 domains" (TaimenModem.cpp:1993-2040 `pdm_rx`, reused);
  - no other services. APR does not use QRTR.
- **APR header** (20 bytes):
  - `hdr_field = (2 << 8) | (5 << 4)` = 0x0250 (SEQ_CMD, 5 words), then `pkt_size`;
  - src svc/domain 5/port, dest svc/domain 4/port, token, opcode;
  - replies are `APR_BASIC_RSP_RESULT` 0x000110E8 {opcode, status} or a command-specific response.
  - One call helper: `apr_call(svc, port, opcode, payload, len, wait_ms, &answer)`, matching on the token (HK redwood_audio.h API).
- **Write:** about 650 lines (GLINK 450 from TZ/0144, QRTR 80, APR 120).
- **Risk:** low. The modem edge already runs this protocol on this phone (NOTES:827-831).

### Stage 4. ADSP core ready

- Send `AVCS_CMD_ADSP_EVENT_GET_STATE` 0x0001290C (svc 3, port 0) until `..._RSP` 0x0001290D says 1, every 50 ms for at most 5 s.
- Then `AVCS_GET_VERSIONS` 0x00012905 → 0x00012906: log the AFE/ASM/ADM service versions (open question 4 depends on them).
- **Write:** about 40 lines.
- **Risk:** low.

### Stage 5. The shared buffer the DSP can read (LPASS SMMU stream 1)

- **Power:** enable the GDSC `gdsc_hlos1_vote_lpass_adsp` (0x17D034: clear bit 0, poll bit 31, as X/bsd/dev/sweet_gpu.c:322-328
  `gdsc_on`), then the CBCR 0x17D014. Never touch the SMMU without them.
- **At M0, read only:** sCR0, IDR0/IDR1, and every SMR/S2CR that matches stream 1. Record what ABL left. The DTS says
  `skip-init` and `tz-device-id "LPASS"`; downstream restores secure state through TZ.
- **Mode A (first), an identity bank:**
  - generalize `smmu_identity()` (TaimenModem.cpp:3299-3360, now fixed to the anoc2 base) to take a base;
  - put SID 1 (mask 0) on a free CB with SCTLR M=0;
  - hand the DSP `pa | (1 << 32)` (pool pa < 4 GB).
- **Mode B:** if the bank cannot be programmed, or ABL already bypasses SID 1 (S2CR type 1), try the plain physical address
  (msm_audio_ion's no-SMMU path).
- **Mode C:** stage-1 tables mapping IOVA 0x10000000 to the pool, HackiOS's `redwood_sound.c` SMMU code:
  - 3 table pages;
  - TCR 39-bit VA, MAIR0 0xf404ff44;
  - PTE `3 | 1<<6 | 2<<8 | 1<<10 | 1<<11`;
  - SMR `0x80000000 | sid`;
  - restore the original SMR/S2CR/SCTLR on teardown.
- **Choosing the mode:** a test map plus one ASM write shows which mode works. Use one boot-arg digit to force the mode.
- **Write:** about 150 lines.
- **Risk: medium-high.** An unmatched or faulting stream crashes the DSP on its first read. TZ may own some banks. Log FSR,
  FAR, FSYNR0 and SGFSR on every failure.

### Stage 6. AFE: quaternary MI2S RX (port 0x1006)

1. **Pins** (TLMM WEST tile, `ctl` = 0x03500000 + 0x1000·n, as X/bsd/dev/taimen_touch.c and sweet_keys.c map tiles):
   - gpio58 and gpio59: function 1, 8 mA, output-high;
   - gpio60 and gpio61: function 1, as the DTS active states say;
   - save the old configs and restore them at the end.
2. **Muxsel:** write 0 to 0x1711D000 (I2S). Write only: HackiOS saw external aborts on reads of LPASS clock registers.
3. **Bit clock:** `AFE_SVC_CMD_SET_PARAM` 0x000100F3 (port 0), in-band, module `AFE_MODULE_CLOCK_SET` 0x0001028F, param
   `AFE_PARAM_ID_CLOCK_SET` 0x00010290. Payload `{1, 0x106 (QUAD_MI2S_IBIT), 1536000, 1 (COUPLE_NO), 0 (root), 1}` (L/ q6afe.c, q6afe.h).
4. **I2S config:** `AFE_PORT_CMD_SET_PARAM_V2` 0x000100EF for port 0x1006, module 0x0001020C, param `AFE_PARAM_ID_I2S_CONFIG`
   0x0001020D. Payload:
   - minor 1, bit_width 16;
   - channel_mode `AFE_PORT_I2S_SD1` 2, stereo 1;
   - WS internal 1;
   - 48000, linear 0.
5. **Start:** `AFE_PORT_CMD_DEVICE_START` 0x000100E5 `{0x1006, 0}`.
6. **Check:** sample gpio58/59's TLMM IN bit 20,000 times (HackiOS: pass at more than 100 bit-clock and 4 frame-sync changes).
   Proof that LPASS drives the pins without any amp involved.

- **Stop:** `DEVICE_STOP` 0x000100E6, the clock with enable 0, the pins restored.
- **Write:** about 200 lines.
- **Risk:** medium. This ADSP build may want 24-bit or 32-bit slots (open question 5). The DSP names its refusals in
  BASIC_RSP status.

### Stage 7. TAS2557 amps (I2C 0x4C left, 0x4D right)

- **The bus:** factor the QUP master out of X/bsd/dev/taimen_touch.c (registers at :57-93, `qup_state`/`qup_msg`/`qup_init`/
  `qup_xfer` at :223-400) into a parameterized `taimen_qup.c`. Touch keeps BLSP1 QUP5; sound adds BLSP2 QUP1:

  | | Value |
  |---|---|
  | Controller | 0x0C1B5000 |
  | GCC | vote 0x152004 bit 15 (BLSP2 AHB), BLSP2_AHB_CBCR 0x125004, QUP1_I2C_APPS_CBCR 0x126008, RCGR 0x126020/24 on XO (W/clock-gcc-8998.c:1411-1434, hwio :87, 144-147) |
  | Pins | gpio55/56, function 3 (`blsp_i2c7`), 2 mA, no pull |
  | Speed | 400 kHz |

- **Reset:**
  - M0 reads PM8998 GPIO 6 (ppid 0x0C5) through `sweet_pmic_read`. Expect input with pull-up, i.e. reset released, and the
    amps ACKing.
  - If they do not ACK, drive it as Android does (low, 5 ms, high) with `sweet_pmic_write` (X/bsd/dev/sweet_keys.c).
  - Then a software reset of each chip (book 0 page 0 reg 1 = 0x01, 1 ms).
- **Firmware** (WH tas2557-core.c; big-endian):
  - header magic "5552", fw size, checksum, PPC, fw and driver versions, timestamp, 64-byte DDC name, description; family 0,
    device 3;
  - then PLLs, programs (name, description, app mode, boost, data set), configurations (devices if driver version ≥ 0x300,
    program, PLL, sample rate, data set) and calibrations;
  - blocks: u32 type, 4 checksum bytes (driver version ≥ 0x200), u32 count, then 4-byte commands {book, page, offset, value}:
    - offset ≤ 0x7F: a write;
    - 0x81: sleep `(book<<8)|page` ms;
    - 0x85: burst of `(book<<8)|page` bytes to the next command's address.
  - Block types:

    | Type | Goes to |
    |---|---|
    | 0x00 | PLL |
    | 0x01 / 0x08 | program, left / right |
    | 0x0D | program, both |
    | 0x04 / 0x0B | pre-power coefficients, left / right |
    | 0x03 / 0x0A | coefficients, left / right |

    Write "both" to each address in turn instead of using broadcast mode.
- **Order, as Android** (dmesg:1554-1563, 2167-2171, 2264, 2579-2580):
  1. default data;
  2. program 0 (Tuning Mode);
  3. configuration 0 (MusicPlayback) PLL + pre + coefficients;
  4. calibration 0 from TAS-CAL;
  5. set the ASI word length to the AFE's (16 at first);
  6. the chips stay powered down.
- **Power up only while the bit clock runs**, after AFE DEVICE_START:
  - startup: GPI_PIN 0x15, GPIO1/2 = 0x01 (BCLK/WCLK), POWER_CTRL2 0xA0 → 0xA3, POWER_CTRL1 0xF8, 2 ms, CLK_ERR_CTRL 0x2B;
  - unmute: MUTE 0, SOFT_MUTE 0;
  - read SAFE_GUARD and the flag registers (clock error, brownout, over-current) on both chips.
- **Power down before the clock stops:** CLK_ERR_CTRL 0, SOFT_MUTE 1, 10 ms, MUTE 3, POWER_CTRL1 0x60, 2 ms, POWER_CTRL2 0,
  POWER_CTRL1 0, GPIOs 0. The register names and books are in WH tas2557.h; copy the numbers from there.
- **Write:** about 550 lines (QUP factoring 60, firmware parser 250, block loader 100, sequences and checks 140).
- **Risks:**
  - Wrong BCLK/word length for the configuration's PLL shows as a clock-error flag; then try 24 in 32-bit slots at 3.072 MHz.
  - The calibration is applied only in tuning mode; without it the amps still play (WH tas2557-core.c).

### Stage 8. The stream: ADM COPP + ASM session (tone test)

HackiOS redwood_sound.c's order, with taimen's port:

| # | Call | To | Payload / result |
|---|---|---|---|
| 1 | `ASM_CMD_SHARED_MEM_MAP_REGIONS` 0x00010D92 | ASM, port 0, token `(1<<8)\|dir` | pool 3 (SHMEM8_4K), 1 region, property 0, `{lsw, msw, size}` of the period buffer (address form from stage 5) → 0x00010D93, handle |
| 2 | `ADM_CMD_DEVICE_OPEN_V5` 0x00010326 | ADM, port 0x1006 | flags 0, mode 1 (RX), endpoint 0x1006 / 0xFFFF, topology **0x00010312** (null COPP), 2 ch, 16 bit, 48000, map FL/FR → 0x00010329 {status, copp id} |
| 3 | `ASM_STREAM_CMD_OPEN_WRITE_V3` 0x00010DB3 | ASM, port 0x0101 (session 1 << 8 \| stream 1) | mode 0, sink 0 (matrix), 16 bit, POPP **0x00010C68** (none), format **0x00010DA5** (multi-channel PCM v2) |
| 4 | `ADM_CMD_MATRIX_MAP_ROUTINGS_V5` 0x00010325 | ADM, port 0 | matrix 0 (RX), 1 session {id 1, 1 COPP, the id from 2} |
| 5 | `ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2` 0x00010D98 | 0x0101 | 2 ch, 16 bit, 48000, signed, map FL/FR |
| 6 | `ASM_SESSION_CMD_RUN_V2` 0x00010DAA | 0x0101 | {0, 0, 0} |
| 7 | `ASM_DATA_CMD_WRITE_V2` 0x00010DAB, repeated | 0x0101 | {addr lsw, msw, handle, 3840, seq, 0, 0, 0}, token `0x80000000\|period`; done = `ASM_DATA_EVENT_WRITE_DONE_V2` 0x00010D99; count `ASM_SESSION_EVENT_RX_UNDERFLOW` 0x00010C17 |

- **Periods:** 8 x 20 ms (960 frames, 3840 bytes, one 4 KB page each), in the pool. Clean each filled line (`dc cvac; dsb sy`)
  if the pool mapping is not Normal-NC.
- **Order:** the AFE port (stage 6) before step 1, and the amps up between steps 6 and 7.
- **Tone:** 1 kHz at -20 dBFS on both channels for 3 s, then silence until every period is back. Fail if a period is not back
  within 1 s.
- **Close:** amps down, `ASM_STREAM_CMD_CLOSE` 0x00010BCD, `ADM_CMD_DEVICE_CLOSE_V5` 0x00010327, `..._UNMAP_REGIONS` 0x00010D94,
  AFE stop.
- **This milestone needs the user's ears.** Run `tools/taimen/attention.sh "listen for a 3 s beep from the speakers at ..."`,
  write the same steps in the reply, keep the hand-off to one tone, and use a timeout. Also check objectively: write-done
  count and underflows, amp flags clean, BCLK toggling.
- **Write:** about 400 lines.
- **Risk:** medium (stage 5's address form; AFE/ADM bit-width mismatches are refused, not silent).

### Stage 9. `/dev/redwood-audio` and the feeder

- **Port** TZ/0146 (cdevsw, `devfs_make_node` "redwood-audio" 0666, non-blocking write into a ring, volume ioctl). Replace its
  DSP-pulled AudioReach ring with HackiOS's level-4 feeder:
  - a 32,768-frame (0.68 s) ring that `write()` fills and never waits on (overflow dropped);
  - a feeder thread that refills each returned period from the ring, zero-filling what is missing.
- **Amp on/off:** on at the first non-silent write, with the stream already running silence. Off after 3 s of silence,
  amps first.
- **Level cap:** a kernel gain, boot-arg `taimenaudiobelow` in eighths of a dB, default 160 (-20 dB), as HackiOS's
  `sound_below`. Raise it only after listening. The ioctl's `below`/`muted` add to it.
- **Test without iOS:** `tools/phone/pcmplay.c` (phone tools CI) writes a tone or a raw file into the device, run through
  netsvcd's command port.
- **Write:** about 300 lines.
- **Risk:** low.

### Stage 10. The HAL plug-in in mediaserverd

- **Copy** TU/audio_plugin.c, TU/audio_plugin.plist and TU/redwood_audio.h to tools/phone/RedwoodAudio/ (keep HackiOS's MIT
  notice; credit them in the README).
- **Change:** the manufacturer string, and leave the Actuator and Codec devices as they are (VirtualAudio needs Hawking and
  can live with the rest).
- **Build and install** per 1.4.
- **Boot with** `sweetlogp=mediaserverd,SpringBoard sweetiolog=mediaserverd` (in `TAIMEN_ADT_ARGS`).
- **Expect:**
  - "redwood: audio: the speaker is with the HAL (/dev/redwood-audio: open)";
  - VirtualAudio taking "Speaker" after the null device;
  - the volume ioctl once at 0 dB;
  - frames arriving when sound plays: a Settings > Sounds tone preview through `tools/taimen/phonectl.py tap`, or a web page's
    audio in Safari.
- **Write:** about 30 changed lines, plus build.sh (about 30) and mkrootfix (about 30).
- **Risks:**
  - The plug-in not loading (signing, open question 10).
  - System sounds muted by the ringer state (open question 9): test with media first.

## 4. Files

| File | New/changed | What | Lines |
|---|---|---|---|
| X/iokit/Kernel/arm/TaimenAudio.cpp | new | stages 2-6 and 8-9: PAS, SMP2P pid 2, GLINK lpass, QRTR, APR, AVCS, LPASS SMMU, AFE, ADM/ASM, the feeder, `/dev/redwood-audio`, level ladder, `taimen_audio_init()` | ~1,900 |
| X/bsd/dev/taimen_tas2557.c | new | stage 7: firmware parser, block loader, startup/shutdown, flags | ~500 |
| X/bsd/dev/taimen_qup.c | new | the QUP I2C master moved out of taimen_touch.c, bus-parameterized (BLSP1 QUP5, BLSP2 QUP1) | ~260 moved |
| X/bsd/dev/taimen_touch.c | changed | use taimen_qup.c | ~-200 / +30 |
| X/bsd/sys/redwood_audio.h | new | TU/redwood_audio.h (ABI part, plus the kernel sink API) | ~60 |
| X/iokit/Kernel/arm/TaimenModem.cpp | changed | export SMEM/hwlock/APCS/SCM helpers with lazy mapping; `smmu_identity(base, ...)`; no Wi-Fi behavior change | ~80 |
| X/bsd/dev/sweet_ufs.c | changed | call `taimen_audio_init()` after `taimen_modem_init()` (:764) | 3 |
| X/bsd/conf/files, X/iokit/conf/files.arm64 | changed | the new files (TAIMEN only) | 3 |
| port/xnu/patches/0315-0322 | new | one per milestone (section 5), written in the scratch tree as usual | |
| port/tools/phone/RedwoodAudio/ | new | the plug-in (from TU/), Info.plist template | ~1,100 (copied) |
| port/tools/phone/pcmplay.c | new | test writer for `/dev/redwood-audio` | ~80 |
| port/tools/phone/build.sh, .github/workflows/phone-tools.yml | changed | build the bundle executable | ~30 |
| port/tools/taimen/mkrootfix.py | changed | `--hal-plugin DIR`: mkdir, Info.plist write, versioned executable, removal of the old one | ~40 |
| port/tools/taimen/boot-xnu.sh | changed | `AUDIOFW=DIR` → ADSP-*/TAS-* blobs; size warning | ~20 |
| port/tools/taimen/reader/init | changed | copy adsp.*, tas2557*, mixer paths from vendor | ~10 |
| port/tools/mkadt.py | maybe | `/product/haptics` (and `/product/audio`) children only if VirtualAudio asks (open question 11) | ~10 |

## 5. Milestones (each proven by a log line; boot-arg `taimenaudio=N`)

| M | Level | Proves | Log line to look for |
|---|---|---|---|
| 0 | 1 | census, no state changed except clock votes | `TAIMEN AUDIO: smem apps<->adsp part @0x1cf000, items 429/443/478/480 absent; pas supported(1)=X; blobs mdt 7260 + N segs (reloc Y, 0x8b200000..); lpass smmu sCR0 .. IDR0 .. SID1 SMR../S2CR..; pool 0x8a000000 clean; gpio55..61 cfg ..; pm gpio6 ..; tas 0x4c rev .. 0x4d rev ..` + mediaserverd's own lines (sweetlogp/sweetiolog), ROOTFIX `--list /Library/Audio/Plug-Ins/HAL:1` |
| 1 | 2 | ADSP runs | `TAIMEN AUDIO: pas init 0/0 mem 0/0 auth 0/0; 443 at +17 ms, slave-kernel READY at +41 ms` |
| 2 | 3 | GLINK + APR | `TAIMEN AUDIO: lpass glink v1, chans IPCRTR apr_audio_svc ...; AVCS state 1; versions AFE .. ASM .. ADM ..` |
| 3 | 4 | AFE port and clocks | `TAIMEN AUDIO: quat mi2s: clk 1536000 ok, i2s cfg ok, start ok; bclk 3xxxx changes, ws 6xx` |
| 4 | 5 | amps load and power with the clock | `TAIMEN AUDIO: tas2557 fw 51609 'tas2557s_PG21_uCDSP' P1/C1/K1; prog 0 conf 0 cal 0 loaded; power up L ok R ok, flags 0/0` |
| 5 | 6 | the tone (the user hears it) | `TAIMEN AUDIO: smmu mode A; map handle ..; copp ..; asm 0x0101 run; tone 150/150 periods back, 0 underflows` |
| 6 | 7 | `/dev/redwood-audio` | `TAIMEN AUDIO: /dev/redwood-audio ready`; pcmplay plays; `frames in N dropped 0` |
| 7 | 7 + plug-in | iOS plays | mediaserverd `redwood: audio: the speaker is with the HAL`; a tone preview heard; `frames in` rising only while it plays |
| 8 | default | everyday | firmware from the root, amps off in silence, no pops, survives a mediaserverd restart, default-on at level 7 |

- One ADSP boot per phone boot until PAS shutdown and restart are tested. Never boot it over stale SMEM state.
- Timeouts on every wait.
- At the run's deadline, amps down before anything else.

## 6. Estimate

| Work | Lines |
|---|---|
| Ported (TZ/0144 PAS/SMP2P/GLINK, TZ/0146 node, HK's APR sequence and SMMU, TU/ plug-in) | about 2,600 |
| New (TAS2557 loader, MI2S/AFE specifics, QUP factoring, tooling) | about 900 |
| **Total** | about 3,500 lines touched |

Calendar:
- Topaz went from ADSP boot to iOS sound in under a day with topaz-woa as its reference (TZ/0144-0146 timestamps).
- Here the APR path is new to this tree and the amps need their firmware.
- Realistic: **4-7 working days, 20-35 boots, plus two short listening hand-offs** (M5, M7). If stage 5 stalls, add 1-2 days
  of SMMU bisection.

## 7. Open questions

1. `adsp.mdt` phdrs: relocatable? Does the image fit 0x8B200000 + 26 MB? (M0, once the files are pulled.)
2. `PAS_IS_SUPPORTED(1)`: expected 1, since Android boots it by PAS (dmesg:2048).
3. Does the ADSP want anything over QRTR before AVCS answers (servreg, sysmon)? Log every lookup.
4. LPASS SMMU:
   - What did ABL leave for SID 1?
   - Can HLOS program a bank there, given `tz-device-id "LPASS"`?
   - Which address form does this firmware take: `pa | 1<<32` through an identity bank, plain pa, or IOVA tables?
5. Word length: does Android run quaternary MI2S at S16 (1.536 MHz, the machine driver default) or S24 (3.072 MHz)? Which
   does the TAS2557 MusicPlayback configuration's PLL expect? `/vendor/etc/mixer_paths*.xml` will say.
6. Does the muxsel write at 0x1711D000 need anything powered first? Write only, after the ADSP is up.
7. Is the amps' reset released at our boot (PM8998 GPIO 6 pull-up), or must we drive it?
8. Which chip is the top speaker and which the bottom? (Not in the DTS for taimen; listen at M5.)
9. Ringer state: the Pixel has no ring/silent switch. If system sounds stay silent while media plays, look at what backboardd
   reports for the ringer (sweetlogp=SpringBoard).
10. Does mediaserverd load an ad hoc plug-in? Expected yes (sweetgl in backboardd). Otherwise see what dyld logs under
    sweetlogp.
11. Does VirtualAudio or MediaExperience want `/product/audio` (VirtualAudio reads it)? HackiOS ran without it. Do not copy
    `supports-always-listening`: it points iOS at the AOP voice trigger.
12. Polling cost: start with polled GLINK at 2-5 ms. Move to SPI 157/158 through `sweet_spi_attach` (touch already does) if
    the thread shows in SWEET TOP.

## 8. Ruled out, and things for later

- **An IOAudio2Device of our own:** a closed ABI with nothing to copy (1.1).
- **Removing VirtualAudio** (the Pixel 1's fix for a mediaserverd crash): it is the route builder the plug-in needs.
- **WCD9340 / SLIMbus for the speakers:** not on the path. The amps are on quaternary MI2S.
  - The tasha codec node is another board's: its probe fails (dmesg:2560).
  - The microphones need WCD9340 over SLIMbus: the NGD at 0x171C0000, whose bus the ADSP manages, the codec's register
    init, and AFE SLIMBUS_0_TX 0x4001. That is a later plan of its own, for `/dev/redwood-mic`.
- **Driving LPAIF DMA from APPS without the ADSP:** the ADSP owns LPASS on msm8998. There is no reference for HLOS LPAIF on
  this SoC (mainline's lpass-cpu covers other SoCs), and HackiOS saw aborts reading LPASS clock registers.
- **Topaz's AudioReach graph (GPR/APM/SPF, TZ/0145-0146):** a different DSP framework. msm8998 is Elite APR (dmesg
  `apr_tal`, DTS `apr_apps2`).
- **Mainline msm8998.dtsi:** no APR or sound nodes. Mainline's q6 drivers are a protocol reference only.
- **Later:**
  - Haptics: TI DRV2624 at i2c-6 0x5A (dmesg:1556-1558, `drv2624.bin`), for `/dev/redwood-haptic` + `/product/haptics`.
  - USB-C audio adapters: device-mode DWC3 today; would need host mode and AppleUSBAudio.driver.
  - Earpiece and call audio: no cellular.

## 9. Address and ID cheat sheet

| Item | Value |
|---|---|
| ADSP | PAS 1; carve-out 0x8B200000 + 0x1A00000; CX TURBO + XO proxy; crash text SMEM 423; wdog SPI 162 |
| SMP2P (adsp) | out 429 `master-kernel` (b0 stop), in 443 `slave-kernel` (b0 fatal, b1 ready, b2 handover, b3 stop-ack); APCS 0x17911008 bit 10; SPI 158 |
| GLINK (lpass) | SMEM 478/479/480 in APPS<->ADSP (SMEM + 0x1cf000, 0x30000); APCS bit 9; SPI 157; channels `IPCRTR`, `apr_audio_svc` |
| APR | hdr 0x0250, 20 bytes; APPS 5, ADSP 4; core 3, AFE 4, ASM 7, ADM 8; basic rsp 0x000110E8 |
| AVCS | GET_STATE 0x0001290C → 0x0001290D; GET_VERSIONS 0x00012905 → 0x00012906 |
| AFE | QUAT MI2S RX 0x1006 (TX 0x1007); START 0x000100E5, STOP 0x000100E6, SET_PARAM_V2 0x000100EF, SVC_SET_PARAM 0x000100F3; dev-if module 0x0001020C / I2S cfg 0x0001020D (SD1 = 2, stereo 1, WS internal 1); clock module 0x0001028F / param 0x00010290, QUAD_MI2S_IBIT 0x106, COUPLE_NO 1 |
| ADM | OPEN_V5 0x00010326 → 0x00010329; CLOSE_V5 0x00010327; MATRIX_MAP_V5 0x00010325; null COPP 0x00010312 |
| ASM | MAP 0x00010D92 → 0x00010D93 (pool 3), UNMAP 0x00010D94; OPEN_WRITE_V3 0x00010DB3; PCM v2 0x00010DA5; POPP none 0x00010C68; FMT_UPDATE_V2 0x00010D98; RUN_V2 0x00010DAA; WRITE_V2 0x00010DAB → done 0x00010D99; underflow 0x00010C17; CLOSE 0x00010BCD; port (session<<8)\|stream |
| LPASS | muxsel quat 0x1711D000 = 0 (I2S); Q6 SMMU 0x05100000 + 0x40000, audio SID 1, DSP address `iova \| 1<<32`; GDSC vote 0x17D034, SMMU CBCR 0x17D014 |
| MI2S pins | TLMM 0x03500000 + 0x1000·n: 58 SCK, 59 WS, 60 SD0 (I/V in), 61 SD1 (play), function 1 |
| TAS2557 | i2c-7 = BLSP2 QUP1 0x0C1B5000 (SPI 101), 400 kHz, gpio55/56 function 3; GCC vote 0x152004 b15, AHB 0x125004, QUP1 CBCR 0x126008, RCGR 0x126020; 0x4C left / 0x4D right; reset PM8998 GPIO 6 (0xC500), IRQ GPIO 7/8; fw `tas2557s_PG21_uCDSP.bin` (51,609), cal `/persist/audio/tas2557_cal.bin` (550) |
| Audio pool | 0x8A000000 + 1 MB (XNU RAM starts 0xb1000000) |
| iOS | plug-in dirs /System/Library/Audio/Plug-Ins/HAL, /Library/Audio/Plug-Ins/HAL, /Library/Apple/Audio/Plug-Ins/HAL; type 443ABAB8-E7B3-491A-B985-BEB9187030DB; device UIDs Speaker, Hawking, Actuator, Codec; `/dev/redwood-audio` 48 kHz s16 stereo, ioctl `_IOW('r', 1, {below, muted})` |
