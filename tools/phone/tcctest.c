/*
 * tcctest: what TCC (tccd) answers a platform process that holds com.apple.private.tcc.allow,
 * as assetsd does (boot 143: "assetsd requires TCCPhotosService access"; its
 * -[PLPrivacy photosAccessAllowedWithScope:] preflights kTCCServicePhotos). Prints the process's
 * code signing flags (csops) and TCCAccessPreflight / TCCAccessCheckAuditToken for a few
 * services, then sleeps (launchd job com.sweet.tcctest; stdout is teed into the kernel log).
 * Signed with tcctest-ents.plist.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

typedef int (*preflight_fn)(CFStringRef service, CFDictionaryRef options);
typedef Boolean (*check_token_fn)(CFStringRef service, audit_token_t token, CFDictionaryRef options);

int
main(void)
{
	uint32_t flags = 0;
	int r = csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof(flags));
	printf("tcctest: uid %d, csops status %d flags 0x%08x (VALID %d, SIGNED %d, PLATFORM %d)\n", getuid(), r, flags,
	    !!(flags & 0x1), !!(flags & 0x20000000), !!(flags & 0x4000000));
	void *tcc = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_NOW);
	preflight_fn preflight = tcc ? (preflight_fn)dlsym(tcc, "TCCAccessPreflight") : NULL;
	check_token_fn check = tcc ? (check_token_fn)dlsym(tcc, "TCCAccessCheckAuditToken") : NULL;
	printf("tcctest: TCC %p, TCCAccessPreflight %p, TCCAccessCheckAuditToken %p\n", tcc, (void *)preflight, (void *)check);
	audit_token_t token;
	mach_msg_type_number_t n = TASK_AUDIT_TOKEN_COUNT;
	kern_return_t kr = task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &n);
	const char *services[] = { "kTCCServicePhotos", "kTCCServiceAddressBook", "kTCCServiceCamera", "kTCCServiceMicrophone" };
	for (unsigned i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		CFStringRef s = CFStringCreateWithCString(NULL, services[i], kCFStringEncodingUTF8);
		int p = preflight ? preflight(s, NULL) : -1;
		int c = (check && kr == KERN_SUCCESS) ? check(s, token, NULL) : -1;
		printf("tcctest: %s: preflight %d (0 granted, 1 denied, 2 unknown), check by audit token %d\n", services[i], p, c);
		CFRelease(s);
	}
	fflush(stdout);
	sleep(3600);
	return 0;
}
