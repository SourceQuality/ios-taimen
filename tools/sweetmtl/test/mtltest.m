/*
 * mtltest: taimen's Metal device as an app sees it, through Metal.framework (docs/taimen-metal-plan.md M1, M2).
 *
 *   mtltest [--daemon]
 *
 * Unlike mtlbridgetest and mtlabitest, which call the bridge directly, this tool links only Apple's frameworks. Metal
 * finds the SweetMetal plug-in through the IOAcceleratorES the kernel names it on (boot-arg taimenmtl=SweetMetal-<hash8>,
 * xnu 0329), loads /System/Library/Extensions/SweetMetal-<hash8>.bundle into this process, and the plug-in gives this
 * process a device because /private/var/sweetmtl/clients lists "mtltest" (tools/sweetmtl/frontend/SweetMetal.mm).
 *
 * M1: the plug-in's setup (clients file, the accelerator's properties, the bundle), MTLCopyAllDevices and
 *     MTLCreateSystemDefaultDevice, the device's name, class and registryID, supportsFamily: answers, and buffers
 *     (newBuffer, contents, gpuAddress).
 * M2: a command queue; blits (fill, buffer copy, texture to buffer); a render pass that clears a BGRA8 texture (load
 *     Clear, store); and the same clear into BGRA8 textures over IOSurfaces (newTextureWithDescriptor:iosurface:plane:),
 *     read back through the surface's base address. The surface checks run with the plug-in's default import (by the
 *     surface's ID, SweetGPU sel 12) and again with SWEETMTL_IOSURFACE=host (the surface's mapping as host pages,
 *     shadowed); the plug-in's log lines say which import each surface got.
 *
 * Each check prints one line: "ok", "info" (a value, not judged) or "FAIL"; the exit status (with --daemon: the
 * "mtltest: done, status N; idling" line) counts the FAIL lines. --daemon: as a launchd job on the phone, run once
 * per boot after the GPU service is up, then idle (test/oneshot.h). The plug-in's own lines ("SweetMetal[pid]: ...")
 * go to stderr; with boot-arg sweetlogp=mtltest both reach the XNU log as |mtltest| lines.
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "oneshot.h"

#define CLIENTS "/private/var/sweetmtl/clients"

/*
 * oneshot.h's wait for the GPU service: SweetGPU answers its parameter query (sel 15), as tools/sweetgl/sweet_drm.c's
 * connect_dev asks. This tool closes that connection again; the plug-in opens its own in this process.
 */
int sweet_drm_open(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (!svc)
		return -1;
	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS)
		return -1;
	uint64_t out[5];
	uint32_t n = 5;
	kr = IOConnectCallScalarMethod(conn, 15, NULL, 0, out, &n);
	IOServiceClose(conn);
	return kr == KERN_SUCCESS && n == 5 ? open("/dev/null", O_RDONLY) : -1;
}

static int fails;

static void line(const char *verdict, const char *what, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(const char *verdict, const char *what, const char *fmt, ...)
{
	char detail[600] = "";
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
	}
	printf("%-8s %s%s%s\n", verdict, what, detail[0] ? ": " : "", detail);
	if (!strcmp(verdict, "FAIL"))
		fails++;
	fflush(stdout);
}

static double now_ms(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
}

/* ---- M1: the plug-in's setup ---- */

/* The clients file: this tool must be listed, and backboardd and SpringBoard must not be (they stay on GLES until M5). */
static void check_clients(void)
{
	FILE *f = fopen(CLIENTS, "r");
	if (!f) {
		line("FAIL", "clients", "no %s (%s): the plug-in gives no process a device", CLIENTS, strerror(errno));
		return;
	}
	char text[1024] = "", l[256];
	bool me = false, gl = false;
	while (fgets(l, sizeof(l), f)) {
		char *hash = strchr(l, '#');
		if (hash)
			*hash = 0;
		char *s = l;
		while (*s == ' ' || *s == '\t')
			s++;
		s[strcspn(s, " \t\r\n")] = 0;
		if (!*s)
			continue;
		if (strlen(text) + strlen(s) + 2 < sizeof(text)) {
			if (text[0])
				strcat(text, " ");
			strcat(text, s);
		}
		me |= !strcmp(s, getprogname());
		gl |= !strcmp(s, "backboardd") || !strcmp(s, "SpringBoard");
	}
	fclose(f);
	if (!me)
		line("FAIL", "clients", "%s lists \"%s\", not %s: no device for this process", CLIENTS, text, getprogname());
	else if (gl)
		line("FAIL", "clients", "%s lists \"%s\": backboardd and SpringBoard must stay on GLES (sweetgl) until M5", CLIENTS, text);
	else
		line("ok", "clients", "%s lists \"%s\"", CLIENTS, text);
}

static void cf_string(CFTypeRef v, char *out, size_t size)
{
	out[0] = 0;
	if (v && CFGetTypeID(v) == CFStringGetTypeID())
		CFStringGetCString((CFStringRef)v, out, (CFIndex)size, kCFStringEncodingUTF8);
	else if (v && CFGetTypeID(v) == CFDataGetTypeID())
		snprintf(out, size, "%.*s", (int)CFDataGetLength((CFDataRef)v), (const char *)CFDataGetBytePtr((CFDataRef)v));
}

/* The IOAcceleratorES Metal finds the plug-in on: its registry entry ID (the device's registryID) and plug-in names. */
static uint64_t check_accelerator(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
	if (!svc) {
		line("FAIL", "accelerator", "no IOAcceleratorES (boot-arg sweetaccel=0? xnu 0166)");
		return 0;
	}
	uint64_t id = 0;
	IORegistryEntryGetRegistryEntryID(svc, &id);
	char name[128], cls[128], gl[128];
	CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0);
	cf_string(v, name, sizeof(name));
	if (v)
		CFRelease(v);
	v = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);
	cf_string(v, cls, sizeof(cls));
	if (v)
		CFRelease(v);
	v = IORegistryEntryCreateCFProperty(svc, CFSTR("IOGLBundleName"), kCFAllocatorDefault, 0);
	cf_string(v, gl, sizeof(gl));
	if (v)
		CFRelease(v);
	IOObjectRelease(svc);
	if (!name[0]) {
		line("FAIL", "accelerator", "IOAcceleratorES 0x%llx has no MetalPluginName: boot-arg taimenmtl=SweetMetal-<hash8> missing "
		     "(or a kernel before xnu 0329)", (unsigned long long)id);
		return id;
	}
	line("ok", "accelerator", "IOAcceleratorES 0x%llx: MetalPluginName %s, MetalPluginClassName %s%s%s", (unsigned long long)id, name,
	     cls[0] ? cls : "(none)", gl[0] ? ", IOGLBundleName " : "", gl);
	char path[300];
	struct stat st;
	snprintf(path, sizeof(path), "/System/Library/Extensions/%s.bundle/SweetMetal", name);
	if (stat(path, &st) != 0)
		line("FAIL", "bundle", "%s: %s (not installed? mkrootfix --zfile, tools/taimen/sweetmtl-install.sh)", path, strerror(errno));
	else
		line("ok", "bundle", "%s, %lld bytes", path, (long long)st.st_size);
	return id;
}

/* ---- M1: the device ---- */

static void check_devices(void)
{
	/* not declared for iOS in the SDK's headers, so looked up: Metal.framework has it. It returns a copy (+1). */
	typedef CFArrayRef (*copy_all_fn)(void);
	copy_all_fn copy_all = (copy_all_fn)dlsym(RTLD_DEFAULT, "MTLCopyAllDevices");
	if (!copy_all) {
		line("info", "MTLCopyAllDevices", "not in this Metal.framework");
		return;
	}
	NSArray *all = CFBridgingRelease(copy_all());
	char names[400] = "";
	for (id<MTLDevice> d in all) {
		size_t n = strlen(names);
		snprintf(names + n, sizeof(names) - n, "%s%s (0x%llx)", n ? ", " : "", d.name.UTF8String ?: "?", (unsigned long long)d.registryID);
	}
	line(all.count ? "ok" : "FAIL", "MTLCopyAllDevices", "%lu device%s%s%s", (unsigned long)all.count, all.count == 1 ? "" : "s",
	     all.count ? ": " : "", names);
}

static void check_device(id<MTLDevice> dev, uint64_t accelerator)
{
	Class c = object_getClass(dev);
	char chain[300] = "";
	for (Class k = c; k; k = class_getSuperclass(k)) {
		size_t n = strlen(chain);
		snprintf(chain + n, sizeof(chain) - n, "%s%s", n ? " < " : "", class_getName(k));
	}
	line("info", "device class", "%s", chain);
	line("info", "device name", "%s", dev.name.UTF8String ?: "(nil)");
	if (!accelerator)
		line("info", "registryID", "0x%llx", (unsigned long long)dev.registryID);
	else if (dev.registryID == accelerator)
		line("ok", "registryID", "0x%llx, the IOAcceleratorES's", (unsigned long long)dev.registryID);
	else
		line("FAIL", "registryID", "0x%llx, the IOAcceleratorES is 0x%llx", (unsigned long long)dev.registryID, (unsigned long long)accelerator);
	MTLSize t = dev.maxThreadsPerThreadgroup;
	line("info", "limits", "maxBufferLength %llu, maxThreadsPerThreadgroup %lux%lux%lu, hasUnifiedMemory %d",
	     (unsigned long long)dev.maxBufferLength, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
	     (int)dev.hasUnifiedMemory);
	static const struct {
		const char *name;
		MTLGPUFamily family;
	} families[] = {
		{ "apple1", (MTLGPUFamily)1001 }, { "apple2", (MTLGPUFamily)1002 }, { "apple3", (MTLGPUFamily)1003 },
		{ "apple4", (MTLGPUFamily)1004 }, { "apple5", (MTLGPUFamily)1005 }, { "apple6", (MTLGPUFamily)1006 },
		{ "apple7", (MTLGPUFamily)1007 }, { "apple8", (MTLGPUFamily)1008 }, { "mac2", (MTLGPUFamily)2002 },
		{ "common1", (MTLGPUFamily)3001 }, { "common2", (MTLGPUFamily)3002 }, { "common3", (MTLGPUFamily)3003 },
		{ "metal3", (MTLGPUFamily)5001 },
	};
	char text[300] = "";
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
		size_t n = strlen(text);
		snprintf(text + n, sizeof(text) - n, "%s%s %d", n ? ", " : "", families[i].name, (int)[dev supportsFamily:families[i].family]);
	}
	line("info", "supportsFamily", "%s", text);
}

/* newBuffer, contents, gpuAddress */
static id<MTLBuffer> check_buffers(id<MTLDevice> dev)
{
	const NSUInteger length = 65536;
	id<MTLBuffer> a = [dev newBufferWithLength:length options:MTLResourceStorageModeShared];
	if (!a || !a.contents) {
		line("FAIL", "newBufferWithLength", "%s", a ? "no contents" : "nil");
		return nil;
	}
	uint32_t *p = (uint32_t *)a.contents;
	unsigned zero = 0, bad = 0;
	for (NSUInteger i = 0; i < length / 4; i++)
		zero += p[i] != 0;
	for (NSUInteger i = 0; i < length / 4; i++)
		p[i] = 0x9e3779b9u * (uint32_t)(i + 1);
	for (NSUInteger i = 0; i < length / 4; i++)
		bad += p[i] != 0x9e3779b9u * (uint32_t)(i + 1);
	uint64_t ga = a.gpuAddress;
	line(!zero && !bad && ga && a.length == length ? "ok" : "FAIL", "newBufferWithLength",
	     "%lu bytes at %p, gpuAddress 0x%llx; %u words not zeroed, %u words read back wrong", (unsigned long)a.length, a.contents,
	     (unsigned long long)ga, zero, bad);
	uint8_t bytes[256];
	for (int i = 0; i < 256; i++)
		bytes[i] = (uint8_t)(i * 7 + 3);
	id<MTLBuffer> b = [dev newBufferWithBytes:bytes length:sizeof(bytes) options:MTLResourceStorageModeShared];
	if (!b || !b.contents) {
		line("FAIL", "newBufferWithBytes", "%s", b ? "no contents" : "nil");
		return a;
	}
	uint64_t gb = b.gpuAddress;
	bool apart = gb && (gb >= ga + length || gb + sizeof(bytes) <= ga);
	line(!memcmp(b.contents, bytes, sizeof(bytes)) && apart ? "ok" : "FAIL", "newBufferWithBytes",
	     "256 bytes %s, gpuAddress 0x%llx (%s the first buffer)", memcmp(b.contents, bytes, sizeof(bytes)) ? "differ" : "match",
	     (unsigned long long)gb, apart ? "apart from" : "OVERLAPS");
	return a;
}

/* ---- M2: commands ---- */

/* Commits and waits (30 s at most: the runtime fails a command buffer the GPU does not finish in 10 s). */
static bool run(id<MTLCommandBuffer> cb, const char *what, double *ms)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	[cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
		(void)b;
		dispatch_semaphore_signal(done);
	}];
	double t0 = now_ms();
	[cb commit];
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC))) {
		line("FAIL", what, "not completed after 30 s (status %ld)", (long)cb.status);
		return false;
	}
	if (ms)
		*ms = now_ms() - t0;
	if (cb.status != MTLCommandBufferStatusCompleted) {
		line("FAIL", what, "status %ld: %s", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "(no error)");
		return false;
	}
	return true;
}

static void check_queue(id<MTLCommandQueue> q)
{
	id<MTLCommandBuffer> cb = [q commandBuffer];
	double ms = 0;
	if (!cb) {
		line("FAIL", "commandBuffer", "nil");
		return;
	}
	if (run(cb, "empty command buffer", &ms))
		line("ok", "empty command buffer", "completed in %.2f ms", ms);
}

/* fillBuffer and copyFromBuffer: on the GPU (copies) and the CPU after a wait (fills: a5xx has no clear_buffer) */
static void check_blits(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLBuffer> a)
{
	if (!a)
		return;
	id<MTLBuffer> c = [dev newBufferWithLength:8192 options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
	if (!c || !bl) {
		line("FAIL", "blits", "%s", c ? "no blit encoder" : "no buffer");
		return;
	}
	[bl fillBuffer:a range:NSMakeRange(256, 4096) value:0x5a];
	[bl copyFromBuffer:a sourceOffset:8192 toBuffer:c destinationOffset:1024 size:4096];
	[bl endEncoding];
	double ms = 0;
	if (!run(cb, "blits", &ms))
		return;
	const uint8_t *pa = (const uint8_t *)a.contents, *pc = (const uint8_t *)c.contents;
	unsigned bad_fill = 0, bad_copy = 0;
	for (int i = 256; i < 256 + 4096; i++)
		bad_fill += pa[i] != 0x5a;
	bad_fill += pa[255] == 0x5a && pa[4352] == 0x5a; /* the bytes around stay */
	for (int i = 0; i < 4096; i++)
		bad_copy += pc[1024 + i] != pa[8192 + i];
	for (int i = 0; i < 1024; i++)
		bad_copy += pc[i] != 0;
	line(!bad_fill ? "ok" : "FAIL", "fillBuffer", "4096 bytes of 0x5a, %u wrong (%.2f ms)", bad_fill, ms);
	line(!bad_copy ? "ok" : "FAIL", "copyFromBuffer", "4096 bytes, %u wrong", bad_copy);
}

/* BGRA8 bytes Metal stores for a clear colour of exact k/255 values */
struct bgra {
	uint8_t b, g, r, a;
};

static unsigned count_bad(const uint8_t *base, size_t row, unsigned w, unsigned h, struct bgra want, char *first, size_t size)
{
	unsigned bad = 0;
	first[0] = 0;
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *p = base + y * row;
		for (unsigned x = 0; x < w; x++, p += 4) {
			bool ok = abs(p[0] - want.b) <= 1 && abs(p[1] - want.g) <= 1 && abs(p[2] - want.r) <= 1 && abs(p[3] - want.a) <= 1;
			if (!ok && !bad++)
				snprintf(first, size, "first at (%u,%u): %02x %02x %02x %02x, want %02x %02x %02x %02x", x, y, p[0], p[1], p[2], p[3],
					 want.b, want.g, want.r, want.a);
		}
	}
	return bad;
}

static MTLRenderPassDescriptor *clear_pass(id<MTLTexture> t, MTLClearColor color)
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = t;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = color;
	return pass;
}

/* A render pass that clears a plain BGRA8 texture, read back with getBytes and a texture-to-buffer blit */
static void check_clear(id<MTLDevice> dev, id<MTLCommandQueue> q)
{
	const unsigned w = 64, h = 64;
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h
										  mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [dev newTextureWithDescriptor:td];
	id<MTLBuffer> out = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
	if (!t || !out) {
		line("FAIL", "texture clear", "%s", t ? "no buffer" : "newTextureWithDescriptor: nil");
		return;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:clear_pass(t, MTLClearColorMake(0.2, 0.4, 0.6, 1.0))];
	if (!re) {
		line("FAIL", "texture clear", "renderCommandEncoderWithDescriptor: nil");
		return;
	}
	[re endEncoding];
	id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
	[bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out
	     destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
	[bl endEncoding];
	double ms = 0;
	if (!run(cb, "texture clear", &ms))
		return;
	const struct bgra want = { 153, 102, 51, 255 };
	uint8_t *bytes = malloc(w * h * 4);
	char first[160], first2[160];
	[t getBytes:bytes bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
	unsigned bad = count_bad(bytes, w * 4, w, h, want, first, sizeof(first));
	unsigned bad2 = count_bad(out.contents, w * 4, w, h, want, first2, sizeof(first2));
	free(bytes);
	line(!bad ? "ok" : "FAIL", "texture clear", "64x64 BGRA8, getBytes %u/%u wrong%s%s (%.2f ms)", bad, w * h, bad ? ", " : "", first, ms);
	line(!bad2 ? "ok" : "FAIL", "copyFromTexture:toBuffer", "%u/%u wrong%s%s", bad2, w * h, bad2 ? ", " : "", first2);
}

/* ---- M2: IOSurfaces ---- */

static IOSurfaceRef new_surface(unsigned w, unsigned h, size_t row)
{
	NSDictionary *props = @{
		(__bridge NSString *)kIOSurfaceWidth : @(w),
		(__bridge NSString *)kIOSurfaceHeight : @(h),
		(__bridge NSString *)kIOSurfaceBytesPerElement : @4,
		(__bridge NSString *)kIOSurfaceBytesPerRow : @(row),
		(__bridge NSString *)kIOSurfaceAllocSize : @(row * h),
		(__bridge NSString *)kIOSurfacePixelFormat : @(0x42475241u), /* 'BGRA' */
	};
	return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

/*
 * A BGRA8 texture over a new IOSurface, cleared by a render pass and read back by the CPU through the surface's base
 * address (what CA and CAMetalLayer drawables amount to: CA consumes IOSurfaces), and through getBytes.
 */
static void check_surface(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *how, unsigned w, unsigned h, MTLClearColor color,
			  struct bgra want)
{
	char what[96];
	snprintf(what, sizeof(what), "IOSurface %ux%u (%s)", w, h, how);
	size_t row = ((size_t)w * 4 + 63) & ~(size_t)63; /* a5xx linear rows: 64-byte pitch */
	IOSurfaceRef s = new_surface(w, h, row);
	if (!s) {
		line("FAIL", what, "IOSurfaceCreate: NULL");
		return;
	}
	row = IOSurfaceGetBytesPerRow(s);
	IOSurfaceLock(s, 0, NULL);
	memset(IOSurfaceGetBaseAddress(s), 0x11, IOSurfaceGetAllocSize(s));
	IOSurfaceUnlock(s, 0, NULL);
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h
										  mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	double t0 = now_ms();
	id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
	double make = now_ms() - t0;
	if (!t) {
		line("FAIL", what, "newTextureWithDescriptor:iosurface:plane: nil (surface %u, base %p, rows %zu bytes, %zu bytes)",
		     IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), row, IOSurfaceGetAllocSize(s));
		CFRelease(s);
		return;
	}
	if (t.iosurface != s || t.width != w || t.height != h || t.pixelFormat != MTLPixelFormatBGRA8Unorm)
		line("FAIL", what, "texture iosurface %p (want %p), %lux%lu, format %lu", t.iosurface, s, (unsigned long)t.width,
		     (unsigned long)t.height, (unsigned long)t.pixelFormat);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:clear_pass(t, color)];
	if (!re) {
		line("FAIL", what, "renderCommandEncoderWithDescriptor: nil");
		CFRelease(s);
		return;
	}
	[re endEncoding];
	double ms = 0;
	if (!run(cb, what, &ms)) {
		CFRelease(s);
		return;
	}
	char first[160], first2[160];
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	unsigned bad = count_bad(IOSurfaceGetBaseAddress(s), row, w, h, want, first, sizeof(first));
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	uint8_t *bytes = malloc((size_t)w * h * 4);
	[t getBytes:bytes bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
	unsigned bad2 = count_bad(bytes, w * 4, w, h, want, first2, sizeof(first2));
	free(bytes);
	line(!bad ? "ok" : "FAIL", what, "surface %u, rows %zu bytes: base address %u/%u wrong%s%s (texture %.2f ms, clear %.2f ms)",
	     IOSurfaceGetID(s), row, bad, w * h, bad ? ", " : "", first, make, ms);
	line(!bad2 ? "ok" : "FAIL", what, "getBytes %u/%u wrong%s%s", bad2, w * h, bad2 ? ", " : "", first2);
	t = nil;
	CFRelease(s);
}

static void m2_surfaces(id<MTLDevice> dev, id<MTLCommandQueue> q)
{
	/* the plug-in's default: by the surface's ID (SweetGPU sel 12), falling back to host pages if that is refused */
	unsetenv("SWEETMTL_IOSURFACE");
	check_surface(dev, q, "default import", 64, 64, MTLClearColorMake(0.2, 0.4, 0.6, 1.0), (struct bgra){ 153, 102, 51, 255 });
	/* backboardd's display rows (1125 px: 4544 bytes, eagl.c), a few of them */
	check_surface(dev, q, "default import", 1125, 48, MTLClearColorMake(1.0, 0.0, 0.8, 0.6), (struct bgra){ 204, 0, 255, 153 });
	/* the host-pointer import the plug-in falls back to (shadowed by the runtime) */
	setenv("SWEETMTL_IOSURFACE", "host", 1);
	check_surface(dev, q, "SWEETMTL_IOSURFACE=host", 64, 64, MTLClearColorMake(0.4, 0.2, 1.0, 1.0), (struct bgra){ 255, 51, 102, 255 });
	unsetenv("SWEETMTL_IOSURFACE");
}

static int test(void)
{
	double t0 = now_ms();
	printf("mtltest: pid %d, Metal.framework's device through the SweetMetal plug-in\n", getpid());
	check_clients();
	uint64_t accelerator = check_accelerator();
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	line(dev ? "ok" : "FAIL", "MTLCreateSystemDefaultDevice", "%s (%.0f ms)", dev ? "a device" : "nil: the plug-in's SweetMetal[...] lines say why",
	     now_ms() - t0);
	check_devices();
	if (!dev)
		return fails;
	check_device(dev, accelerator);
	id<MTLBuffer> a = check_buffers(dev);
	id<MTLCommandQueue> q = [dev newCommandQueue];
	if (!q) {
		line("FAIL", "newCommandQueue", "nil");
		return fails;
	}
	check_queue(q);
	check_blits(dev, q, a);
	check_clear(dev, q);
	m2_surfaces(dev, q);
	printf("mtltest: %d FAIL lines (%.0f ms)\n", fails, now_ms() - t0);
	return fails;
}

int main(int argc, char **argv)
{
	oneshot_begin("mtltest", &argc, &argv);
	(void)argv;
	int status;
	@autoreleasepool {
		@try {
			status = test();
		} @catch (NSException *e) {
			line("FAIL", "exception", "%s: %s", e.name.UTF8String ?: "?", e.reason.UTF8String ?: "?");
			status = fails;
		}
	}
	return oneshot_end("mtltest", status);
}
