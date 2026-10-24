#!/usr/bin/env python3
"""Build the ROOTFIX payload blob: files the kernel writes into the mounted iOS root at every
boot, before launchd (xnu 0210, bsd/kern/taimen_rootfix.c). The Pixel 1 port's root tweaks
(tools/sweet-root-tweaks.py) without reflashing userdata or writing HFS+ on the PC.

  mkrootfix.py ROOT.img OUT.bin [--migrate] [--file /DEST=SRC ...] [--zfile /DEST=SRC ...]

ROOT.img is the raw root image (tools/simg2img.py of what was flashed); the original files the
tweaks edit are read from it with 7-Zip (7z.exe on Windows reads HFS+). Without --migrate the
first-boot data migration is skipped (LastSystemVersion in the ByHost com.apple.migration
prefs, sweet NOTES); with it the sweet sentinel makes it run once (~30 min on taimen, but it
registers the app extensions: wallpapers, widgets).

--file adds a file from the PC, written only if absent (op 2), mode 0755. Give it a versioned
name (sweetgl.dylib goes in as /usr/lib/sweetgl-HASH.dylib): a new build is then a new file, and
no boot rewrites a library a process may be loading (the records apply after launchd remounts
the root read-write, when backboardd starts). Such a blob is for the boot that installs the
file; later boots keep the small one: ABL starts no payload over ~35 MiB (boot 78), and a
development kernel takes ~17 MB of it. --zfile is --file zlib-compressed (op 5, xnu 0245; the
19 MB sweetgl dylib goes in at about a third).

Records: "RFX1", count, then {op, mode, uid, gid, path length, data length, path, data}
(op 1 write, 2 write if absent, 3 mkdir if absent, 4 remove if present, 5 as 2 with the data a
u32 size and a zlib stream; 4-byte padding).
"""
import argparse, os, plistlib, struct, subprocess, sys, tempfile, zlib

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501
WRITE, CREATE, MKDIR, REMOVE, CREATE_Z = 1, 2, 3, 4, 5
SEVENZIP = os.environ.get('SEVENZIP', '/mnt/c/Program Files/7-Zip/7z.exe')


def winpath(p):
    return subprocess.run(['wslpath', '-w', p], check=True, capture_output=True, text=True).stdout.strip()


def extract(img, paths, dest):
    """Copy root files (paths relative to /) out of the HFS+ image into dest."""
    names = ['iOSRoot\\' + p.replace('/', '\\') for p in paths]
    subprocess.run([SEVENZIP, 'x', '-y', '-o' + winpath(dest), winpath(img)] + names,
                   check=True, capture_output=True)
    out = {}
    for p in paths:
        f = os.path.join(dest, 'iOSRoot', p)
        if not os.path.exists(f):
            sys.exit(f'{p}: not in the image')
        out[p] = open(f, 'rb').read()
    return out


class Blob:
    def __init__(self):
        self.recs = []

    def add(self, op, path, data=b'', mode=0o644, uid=0, gid=0):
        self.recs.append((op, mode, uid, gid, path, data))

    def mkdirs(self, path, mode=0o755, uid=0, gid=0):
        """mkdir -p: every missing level gets mode/uid/gid."""
        parts = path.strip('/').split('/')
        for i in range(1, len(parts) + 1):
            self.add(MKDIR, '/' + '/'.join(parts[:i]), mode=mode, uid=uid, gid=gid)

    def plist(self, op, path, value, mode, uid, gid, fmt=plistlib.FMT_BINARY):
        self.add(op, path, plistlib.dumps(value, fmt=fmt), mode, uid, gid)

    def pack(self):
        pad = lambda b: b + b'\0' * (-len(b) % 4)
        out = struct.pack('<4sI', b'RFX1', len(self.recs))
        for op, mode, uid, gid, path, data in self.recs:
            p = path.encode() + b'\0'
            out += struct.pack('<6I', op, mode, uid, gid, len(p), len(data)) + pad(p) + pad(data)
        return out


def launchd(plist):
    """sweet-root-tweaks.py launchd(): no panics on crashes, no baseband/FairPlay/diagnostics jobs."""
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    gone = ('com.apple.CommCenter', 'com.apple.fairplayd.H2', 'com.apple.voicemail.vmd',
            'com.apple.ReportCrash', 'com.apple.spindump', 'com.apple.tailspind',
            'com.apple.aggregated', 'com.apple.aggregated.addaily')
    removed = [k for k, j in jobs.items() if j.get('Label') in gone]
    for k in removed:
        del jobs[k]
    for j in jobs.values():
        if j.get('Label') == 'com.apple.backboardd':
            j.setdefault('EnvironmentVariables', {})['CA_BACKDROP_SCALE'] = '1'
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} jobs removed: '
          + ', '.join(sorted(jobs_label for jobs_label in gone)))
    return plist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('out')
    ap.add_argument('--migrate', action='store_true')
    ap.add_argument('--file', action='append', default=[], metavar='/DEST=SRC')
    ap.add_argument('--zfile', action='append', default=[], metavar='/DEST=SRC')
    a = ap.parse_args()
    b = Blob()
    srv = ['System/Library/Frameworks/MediaToolbox.framework/%s/SubServerConfiguration.plist' % d
           for d in ('D22', 'D221', 'Default')]
    with tempfile.TemporaryDirectory() as tmp:
        orig = extract(a.image, ['System/Library/xpc/launchd.plist'] + srv, tmp)

    b.plist(WRITE, '/System/Library/xpc/launchd.plist',
            launchd(plistlib.loads(orig['System/Library/xpc/launchd.plist'])), 0o644, 0, 0)
    for p in srv:      # camera filter prewarm calls a NULL OpenGL function without a GPU
        data = plistlib.loads(orig[p])
        data['StartRecorderServer'] = False
        b.plist(WRITE, '/' + p, data, 0o644, 0, 0, plistlib.FMT_XML)

    # containers live on the data volume on iOS
    b.mkdirs('/private/var/containers/Bundle', uid=33, gid=33)
    for d in ('Data/System', 'Shared/SystemGroup', 'Temp'):
        b.mkdirs('/private/var/containers/' + d)
    # app extensions: their point definitions need trust caches this kernel does not have
    b.mkdirs('/private/var/preferences/FeatureFlags/Domain')
    b.plist(WRITE, '/private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist',
            {'enforce_xpc_cache_code_signing': {'Enabled': False}}, 0o644, 0, 0, plistlib.FMT_XML)

    prefs = '/private/var/mobile/Library/Preferences'
    b.mkdirs(prefs + '/ByHost', uid=MOBILE, gid=MOBILE)
    if a.migrate:
        b.mkdirs('/private/var/datamigrator')
        b.add(CREATE, '/private/var/datamigrator/migrate', b'', 0o644, MOBILE, MOBILE)
    else:
        b.plist(WRITE, prefs + '/ByHost/com.apple.migration.@HOSTUUID@.plist',
                {'LastSystemVersion': BUILD}, 0o600, MOBILE, MOBILE)
        # boot 29: still migrating. datamigrator creates this sentinel when it starts and
        # removes it when done ("Migration sentinel present. Migration necessary.", sweet
        # NOTES); every run so far was cut short by a reset, so it stays behind.
        b.add(REMOVE, '/private/var/datamigrator/migrate')
    for name, values in (
            ('com.apple.springboard', {'BKNoWatchdogs': True,
                                       'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
                                       'MCForceShowCloudConfigurationUI': False}),
            ('com.apple.purplebuddy', {'SetupDone': True, 'SetupFinishedAllSteps': True,
                                       'SetupVersion': 11, 'daemonizeNeedsToRun': False}),
            ('com.apple.purplebuddy.notbackedup', {k: False for k in (
                'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy',
                'ShouldRunMesaMiniBuddy', 'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                'SilentICDPUpgrade')}),
            ('com.apple.managedconfiguration.notbackedup',
             {'LockdownActivationIndicatesCloudConfigurationAvailable': False})):
        b.plist(WRITE, f'{prefs}/{name}.plist', values, 0o600, MOBILE, MOBILE)

    for f in a.file:
        dest, src = f.split('=', 1)
        assert dest.startswith('/'), dest
        b.add(CREATE, dest, open(src, 'rb').read(), 0o755, 0, 0)
    for f in a.zfile:
        dest, src = f.split('=', 1)
        assert dest.startswith('/'), dest
        raw = open(src, 'rb').read()
        b.add(CREATE_Z, dest, struct.pack('<I', len(raw)) + zlib.compress(raw, 9), 0o755, 0, 0)

    blob = b.pack()
    open(a.out, 'wb').write(blob)
    print(f'{a.out}: {len(b.recs)} records, {len(blob)} bytes'
          + (' (data migration once)' if a.migrate else ' (data migration skipped)'))


main()
