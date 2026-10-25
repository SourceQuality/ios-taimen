#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--pixel1]
--pixel1: Google Pixel 1 (sailfish, MSM8996) presented as an iPhone 8 instead.
--taimen: Google Pixel 2 XL (taimen, MSM8998), iPhone X identity, 4K-page kernel.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

# Google Pixel 1 (sailfish, MSM8996), from its live FDT and shim probes (NOTES.md).
PIXEL1 = dict(
    phys_base=0x100000000, mem_size=0x7e550000,  # upper DDR bank, no carve-outs
    gicd=0x9bc0000, gicr=0x9c00000, gicr_stride=0x40000,  # not the usual 0x20000
    vtimer=27,  # standard PPI numbering here (sweet: 19)
    fb=0xc0001000, fb_size=0x31ff000, fb_w=1080, fb_h=1920, fb_bpp=24,
    fb_kickoff=0x90201c,  # command-mode panel: MDP CTL0 START sends a frame
    ufs=0x624000, ufs_size=0x2500,
    # MPIDR, cluster, max clock: 2 x 1.6 GHz + 2 x 2.15 GHz Kryo
    cpus=[(0x0, 0, 1593600000), (0x1, 0, 1593600000),
          (0x100, 1, 2150400000), (0x101, 1, 2150400000)],
)

# Google Pixel 2 XL (taimen, MSM8998), from its live FDT and the stage-1 shim
# register dump (NOTES-taimen.md, docs/taimen-porting-map.md).
TAIMEN = dict(
    phys_base=0x100000000, mem_size=0x20000000,  # 512 MiB above 4 GB to start (4K kernels failed above that on the S8)
    gicd=0x17a00000, gicr=0x17b00000,  # redistributor stride 0x20000 (the kernel default)
    vtimer=19,  # Qualcomm PPI numbering, like sweet
    fb=0x9d400000, fb_size=0x1000000, fb_w=1440, fb_h=2880, fb_bpp=32,  # VIG0: BGRA, stride 0x1680; 1440*2880*4 = 0xfd2000 (the splash region is 36 MiB, map only the panel)
    fb_kickoff=0xc90201c,  # MDP CTL0 START: PP0 autorefresh is off, every frame is requested
    ufs=0x1da4000, ufs_size=0x2500,
    # MPIDR, cluster, max clock: 4 x Kryo 280 silver (A53-class) + 4 x gold (A73-class)
    cpus=[(0x0, 0, 1900800000), (0x1, 0, 1900800000), (0x2, 0, 1900800000), (0x3, 0, 1900800000),
          (0x100, 1, 2457600000), (0x101, 1, 2457600000), (0x102, 1, 2457600000), (0x103, 1, 2457600000)],
)

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')
# Pixel 1: iPhone 8 (iPhone10,1 / D20AP) userland, iPhone_4.7_P3 IPSW, same way:
#   tools/remotezip.py URL DeviceTree.d20ap DIR; tools/adt.py unwrap DIR/.../DeviceTree.d20ap.im4p d20ap.adt
D20AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-8/d20ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, pixel1=False, taimen=False):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    vtimer = QEMU_VTIMER_INTID if qemu else VTIMER_INTID
    if pixel1 or taimen:
        P = PIXEL1 if pixel1 else TAIMEN
        phys_base, mem_size, gicd, gicr, vtimer = P['phys_base'], P['mem_size'], P['gicd'], P['gicr'], P['vtimer']
        # TAIMEN_MEM (boot-xnu.sh): more of the free upper bank (Linux: System RAM up to 0x17e4bffff, ~2 GB
        # above 4 GB); 512 MiB is the S8 port's limit, untried above that on taimen (boot 61: iOS with
        # Mesa stalled, compressor busy)
        # TAIMEN_BASE=0xb1000000: XNU's RAM starts in the low bank, whose top runs on into the upper
        # bank (Linux System RAM b1000000-17e4bffff): ~3.2 GiB in one range (xnu 0251)
        if taimen and os.environ.get('TAIMEN_BASE'):
            phys_base = int(os.environ['TAIMEN_BASE'], 0)
            assert phys_base in (0xb1000000, 0x100000000), hex(phys_base)
        if taimen and os.environ.get('TAIMEN_MEM'):
            mem_size = int(os.environ['TAIMEN_MEM'], 0)
            assert 0x20000000 <= mem_size and phys_base + mem_size <= 0x17e4c0000 and mem_size % 0x200000 == 0, \
                hex(mem_size)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    # TAIMEN_ADT_ARGS: boot-args beyond the 607 bytes of the command line (xnu 0309 reads
    # /chosen sweet-boot-args for an argument the command line does not have)
    extra_args = os.environ.get('TAIMEN_ADT_ARGS', '').strip()
    if extra_args:
        print(f'/chosen sweet-boot-args: {extra_args}', file=sys.stderr)
    chosen = node('chosen', [
        ph(),
    ] + ([('sweet-boot-args', cstr(extra_args))] if extra_args else []) + [
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices); iPhone 8: 750x1334 @2x.
        ('display-scale', u32(2 if pixel1 else 3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # MAC addresses iBoot copies from syscfg (WMac/BMac). MobileGestalt's
        # WifiAddress (part of the A11 UniqueDeviceID) reads /chosen first,
        # else waits forever for an IONetworkController (Pixel 1: lsd's first
        # database seed hung there). Synthetic, locally administered.
        ('mac-address-wifi0', bytes.fromhex('02005e77ee10')),
        ('mac-address-bluetooth0', bytes.fromhex('02005e77ee11')),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        if pixel1 or taimen:
            break
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    # Pixel 1: 2 + 2 Kryo (same core, different clocks) as one SMP cluster too.
    # TAIMEN_NCPU=N (boot-xnu.sh SHIM_WATCH=1): only the first N cores; the shim's watcher keeps CPU 7.
    ncpu = int(os.environ.get('TAIMEN_NCPU') or 8) if taimen else 8
    for core, (mpidr, cluster, hz) in enumerate(PIXEL1['cpus'] if pixel1 else TAIMEN['cpus'][:ncpu] if taimen else []):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo280' if taimen else 'qcom,kryo') + cstr('ARM,v8')),
            ('reg', u32(mpidr)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(cluster)),  # one L2 per pair
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(hz)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ] + ([('redistributor-stride', u32(PIXEL1['gicr_stride']))] if pixel1 else []))
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(750 if pixel1 else 1125)),
            ('canvas-height', u32(1334 if pixel1 else 2436)),
        ])] + ([
        # Wi-Fi (bsd/dev/sailfish_wlan.c, QCA6174 on PCIe): launchd starts
        # wifid only if the tree has a node named wlan; SailfishWLAN (the
        # interface's controller) matches this nub.
        node('wlan', [
            ph(),
            ('device_type', cstr('wlan')),
            ('compatible', cstr('wlan-pcie,sailfish')),
        ])] if pixel1 else []))

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])
    if pixel1 or taimen:
        P = PIXEL1 if pixel1 else TAIMEN
        fbcon = node('sweet-fbcon', [
            ph(),
            ('reg', u64(P['fb'], P['fb_size'])),
            ('width', u32(P['fb_w'])),
            ('height', u32(P['fb_h'])),
            ('stride', u32(P['fb_w'] * P['fb_bpp'] // 8)),
            ('bits-per-pixel', u32(P['fb_bpp'])),
            # write 1 here after drawing: the panel only refreshes on request
            ('kickoff-reg', u64(P['fb_kickoff'])),
        ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(PIXEL1['ufs'], PIXEL1['ufs_size']) if pixel1 else u64(TAIMEN['ufs'], TAIMEN['ufs_size']) if taimen else u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(1 if pixel1 else 2)),  # iPhone 8: Home button
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product(D20AP_ADT if pixel1 else D22AP_ADT)
    # taimen too since boot 137 (TAIMEN_GAMUT=P3 keeps the iPhone X's): with P3, WebKit's GPU
    # process draws Safari's pages into 'w30r' / 'b3a8' wide-colour tiles, which sweetgl has no
    # texture format for, so every page stayed black (boot 136).
    if (pixel1 or (taimen and os.environ.get('TAIMEN_GAMUT', 'sRGB') != 'P3')) and product_props:
        # The Pixel panel is sRGB, and with "P3" iOS picks deep-color snapshot
        # surfaces (MaterialKit b3a8, CA w30r) that CoreAnimation's CPU renderer
        # cannot draw into: the dock / App Library backgrounds stayed white.
        product_props = [(k, cstr('sRGB') if k in ('artwork-display-gamut', 'display-gamut') else v)
                         for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    # Pixel 1 keeps 'xiaomi,sweet' for the same kernel matching (one platform expert).
    ident = (('D20AP', 'iPhone10,1', 'D20', 'google,sailfish', 'qcom,msm8996') if pixel1 else
             ('D22AP', 'iPhone10,3', 'D22', 'xiaomi,sweet', 'qcom,sm7150'))
    compat = cstr(ident[0]) + cstr(ident[1]) + cstr('xiaomi,sweet')
    if pixel1:
        compat += cstr(ident[3])
    if taimen:
        compat += cstr('google,taimen')
    return node('device-tree', [
        ph(),
        ('compatible', compat + cstr('qcom,msm8998' if taimen else ident[4]) + cstr('AppleARM')),
        ('model', cstr(ident[1])),
        ('target-type', cstr(ident[2])),
        ('target-sub-type', cstr(ident[0])),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv, pixel1='--pixel1' in sys.argv, taimen='--taimen' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
