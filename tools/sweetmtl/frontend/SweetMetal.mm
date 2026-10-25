/*
 * SweetMetal.bundle: taimen's Metal driver plug-in (docs/taimen-metal-plan.md 4.0, M1), from flame's SweetMetal2.mm
 * (ref/ios-flame phone/metal2, MIT, dipa-studio; see LICENSE here).
 *
 * The kernel names this bundle on the IOAcceleratorES when boot-arg taimenmtl=SweetMetal-<hash8> is set (xnu 0329):
 * MetalPluginName SweetMetal-<hash8>, MetalPluginClassName SweetMTLDevice. Metal.framework then loads
 * /System/Library/Extensions/SweetMetal-<hash8>.bundle in every process that asks for a device and calls
 * -[SweetMTLDevice initWithAcceleratorPort:]. That hands back the frontend's device (NativeBridgeMetalFrontend.mm over
 * the bridge, sweetmtl-runtime and Gallium freedreno through SweetGPU), once per process, and only to the processes
 * /private/var/sweetmtl/clients names (one process name per line, '#' starts a comment). Every other process gets nil,
 * which Metal skips: that process has no Metal device, and backboardd keeps drawing with GLES (sweetgl).
 *
 * Per listed process:
 *  - waits for the GPU service (SweetGPU), SWEETMTL_GPU_WAIT seconds at most (default 20), then gives up (nil);
 *  - KEY=VALUE lines of /private/var/sweetmtl/env go into its environment first (debugging);
 *  - its shader cache is /private/var/sweetmtl/cache/<process> (a sandboxed process: its own caches directory);
 *  - logs to stderr, one line per write: "SweetMetal[pid]: ..." (taimen's kernel tees a process's writes to /dev/null
 *    into its log for the names boot-arg sweetlogp or sweetnull lists: xnu 0308, 0048).
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

/* tools/sweetgl/sweet_drm.c (in libsweetgallium.a): a dup of the device "fd", or -1 while SweetGPU is not up */
extern "C" int sweet_drm_open(void);

#define CLIENTS "/private/var/sweetmtl/clients"
#define ENVFILE "/private/var/sweetmtl/env"
#define CACHE "/private/var/sweetmtl/cache"

extern "C" void sweetmtl_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "SweetMetal[%d]: %s\n", getpid(), s);
	if (n > (int)sizeof(buf) - 1) {
		n = (int)sizeof(buf) - 1;
		buf[n - 1] = '\n';
	}
	if (n > 0)
		(void)write(STDERR_FILENO, buf, (size_t)n);
}

static void sm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sweetmtl_logs(buf);
}

/* Selectors the frontend lacks, each logged once (the frontend then raises the usual unrecognised-selector exception) */
static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm_log("MISSING %s -%s", cls, sel);
}

@interface NBDeviceMethods : NSObject
@end

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines itself. */
@interface NBDeviceMethods (SweetMetal)
@end
@implementation NBDeviceMethods (SweetMetal)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	(void)path;
	if (error)
		*error = [NSError errorWithDomain:@"SweetMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
@end

/* Is this process named in the clients file? One name per line; blanks and '#' comments are ignored. */
static bool listed(char *why, size_t size)
{
	FILE *f = fopen(CLIENTS, "r");
	if (!f) {
		snprintf(why, size, "no %s (%s)", CLIENTS, strerror(errno));
		return false;
	}
	const char *me = getprogname();
	char line[256];
	bool yes = false;
	while (!yes && fgets(line, sizeof(line), f)) {
		char *hash = strchr(line, '#');
		if (hash)
			*hash = 0;
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		size_t n = strcspn(s, " \t\r\n");
		s[n] = 0;
		yes = n && !strcmp(s, me);
	}
	fclose(f);
	if (!yes)
		snprintf(why, size, "not in %s", CLIENTS);
	return yes;
}

/* KEY=VALUE lines of the env file into this process's environment (debugging: SWEETMTL_TRACE, SG_DEBUG, FD_MESA_DEBUG, ...). */
static void read_env(void)
{
	FILE *ef = fopen(ENVFILE, "r");
	if (!ef)
		return;
	char line[256];
	while (fgets(line, sizeof(line), ef)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (line[0] != '#' && eq && eq != line) {
			*eq = 0;
			setenv(line, eq + 1, 1);
			sm_log("env %s=%s", line, eq + 1);
		}
	}
	fclose(ef);
}

/* SweetGPU comes up 10-20 s into boot: wait for it, a bounded time (eagl.c's gpu_ready, oneshot.h's wait). */
static bool gpu_up(void)
{
	const char *w = getenv("SWEETMTL_GPU_WAIT");
	int secs = w ? atoi(w) : 20;
	CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
	for (int i = 0;; i++) {
		int fd = sweet_drm_open();
		if (fd >= 0) {
			close(fd); /* sweet_drm.c keeps its connection; the shim opens its own fd */
			if (i)
				sm_log("GPU service up after %.1f s of waiting", CFAbsoluteTimeGetCurrent() - t0);
			return true;
		}
		if (i >= secs * 4) {
			sm_log("no GPU service (SweetGPU) after %d s: no Metal device", secs);
			return false;
		}
		usleep(250000);
	}
}

/* The shader cache: /private/var/sweetmtl/cache/<process>, or a sandboxed process's own caches directory. */
static void cache_dir(char *cache, size_t size)
{
	mkdir("/private/var/sweetmtl", 0755);
	mkdir(CACHE, 0777);
	chmod(CACHE, 0777);
	snprintf(cache, size, CACHE "/%s", getprogname());
	mkdir(cache, 0777);
	if (access(cache, W_OK) != 0) {
		char dir[256];
		if (confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir) - 16)) {
			snprintf(cache, size, "%ssweetmtl", dir);
			mkdir(cache, 0700);
		}
	}
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTLDevice : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	/* this object is dropped for the frontend's device: initialised, so its -[_MTLDevice dealloc] has queues to release */
	if (!(self = [super init]))
		return nil;
	static id device;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		char why[300];
		if (!listed(why, sizeof(why))) {
			sm_log("%s: %s, so no Metal device", getprogname(), why);
			return;
		}
		read_env();
		/* Metal validation failures abort by default; frameworks with Apple-GPU-only kernels (MPS) report one on any other device. */
		void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
		for (unsigned t = 0; set && t < 5; t++)
			set(t, 5); /* 5 = NSLog */
		if (!gpu_up())
			return;
		char cache[256];
		cache_dir(cache, sizeof(cache));
		/* plain rows everywhere: no Apple framebuffer compression, no 10-bit deep framebuffer */
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
		@try {
			device = native_bridge_metal_open(NULL, cache, registryID, missing);
		} @catch (NSException *e) {
			sm_log("device: exception %s", e.reason.UTF8String);
			device = nil;
		}
		sm_log("device %s in %.0f ms (registryID 0x%llx, cache %s)%s%s", device ? [[device name] UTF8String] : "refused",
		       (CFAbsoluteTimeGetCurrent() - t0) * 1000, (unsigned long long)registryID, cache, device ? "" : ": ",
		       device ? "" : nmb_last_error());
		/*
		 * Extended-range (XR10) formats: the frontend withholds them so that a backboardd on Metal keeps an 8-bit framebuffer;
		 * other processes may render into them (AegirPoster's CAMetalLayer throws "unsupported extended range format" without).
		 */
		if (device && strcmp(getprogname(), "backboardd") != 0)
			class_replaceMethod(object_getClass(device), sel_registerName("supportsExtendedXR10Formats"),
					    imp_implementationWithBlock(^BOOL(id me) {
						    (void)me;
						    return YES;
					    }),
					    "B16@0:8");
		/* the image base, for symbolicating samples (atos -o SweetMetal.unstripped -l BASE ADDR) */
		Dl_info self_info = { 0 };
		dladdr((void *)sweetmtl_logs, &self_info);
		sm_log("image: %s at %p", self_info.dli_fname ? self_info.dli_fname : "?", self_info.dli_fbase);
	});
	return device;
}
@end
