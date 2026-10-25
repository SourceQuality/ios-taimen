/*
 * tcctest: what TCC (tccd) answers a platform process that holds com.apple.private.tcc.allow,
 * as assetsd does (boot 143: "assetsd requires TCCPhotosService access"; its
 * -[PLPrivacy photosAccessAllowedWithScope:] preflights kTCCServicePhotos). Prints the process's
 * code signing flags (csops), TCCAccessPreflight / TCCAccessCheckAuditToken for a few services
 * (boot 144: every preflight "denied", the audit token check as entitled), whether
 * com.apple.tccd can be looked up, and what tccd itself replies to a preflight sent over XPC,
 * then sleeps (launchd job com.sweet.tcctest; stdout is teed into the kernel log).
 * Signed with tcctest-ents.plist.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

/* declared here: the iOS SDK marks these unavailable */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *name, mach_port_t *sp);
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, void *queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t c, xpc_handler_t h);
extern void xpc_connection_resume(xpc_connection_t c);
extern void xpc_connection_cancel(xpc_connection_t c);
extern xpc_object_t xpc_dictionary_create(const char *const *keys, xpc_object_t const *values, size_t count);
extern void xpc_dictionary_set_string(xpc_object_t d, const char *key, const char *value);
extern void xpc_dictionary_set_bool(xpc_object_t d, const char *key, bool value);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t c, xpc_object_t m);
extern char *xpc_copy_description(xpc_object_t o);

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

typedef int (*preflight_fn)(CFStringRef service, CFDictionaryRef options);
typedef Boolean (*check_token_fn)(CFStringRef service, audit_token_t token, CFDictionaryRef options);

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void
xpc_probe(const char *function)
{
	xpc_connection_t c = xpc_connection_create_mach_service("com.apple.tccd", NULL, 0);
	xpc_connection_set_event_handler(c, ^(xpc_object_t e) {
		(void)e;
	});
	xpc_connection_resume(c);
	xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_string(m, "function", function);
	xpc_dictionary_set_string(m, "service", "kTCCServicePhotos");
	xpc_dictionary_set_bool(m, "preflight", true);
	double t = now_ms();
	xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
	char *d = xpc_copy_description(r);
	printf("tcctest: xpc %s -> (%.0f ms) %.600s\n", function, now_ms() - t, d);
	free(d);
	xpc_connection_cancel(c);
}

int
main(void)
{
	uint32_t flags = 0;
	int r = csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof(flags));
	printf("tcctest: uid %d, csops status %d flags 0x%08x (VALID %d, SIGNED %d, PLATFORM %d)\n", getuid(), r, flags,
	    !!(flags & 0x1), !!(flags & 0x20000000), !!(flags & 0x4000000));
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t lk = bootstrap_look_up(bootstrap_port, "com.apple.tccd", &port);
	printf("tcctest: bootstrap_look_up(com.apple.tccd) = %d, port 0x%x\n", lk, port);
	void *tcc = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_NOW);
	preflight_fn preflight = tcc ? (preflight_fn)dlsym(tcc, "TCCAccessPreflight") : NULL;
	check_token_fn check = tcc ? (check_token_fn)dlsym(tcc, "TCCAccessCheckAuditToken") : NULL;
	audit_token_t token;
	mach_msg_type_number_t n = TASK_AUDIT_TOKEN_COUNT;
	kern_return_t kr = task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &n);
	const char *services[] = { "kTCCServicePhotos", "kTCCServiceAddressBook", "kTCCServiceCamera", "kTCCServiceMicrophone" };
	for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		CFStringRef s = CFStringCreateWithCString(NULL, services[i], kCFStringEncodingUTF8);
		double t = now_ms();
		int p = preflight ? preflight(s, NULL) : -1;
		double tp = now_ms() - t;
		t = now_ms();
		int c = (check && kr == KERN_SUCCESS) ? check(s, token, NULL) : -1;
		printf("tcctest: %s: preflight %d in %.0f ms (0 granted, 1 denied, 2 unknown), check by audit token %d in %.0f ms\n",
		    services[i], p, tp, c, now_ms() - t);
		CFRelease(s);
	}
	xpc_probe("TCCAccessRequest");
	xpc_probe("TCCAccessPreflight");
	fflush(stdout);
	sleep(3600);
	return 0;
}
