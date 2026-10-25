/*
 * sweettcc.dylib: TCCAccessPreflight that agrees with TCCAccessCheckAuditToken. On taimen tccd
 * answers every preflight "denied" (boot 144, tcctest: Photos, Contacts, Camera, Microphone all 1)
 * while its audit token check honours com.apple.private.tcc.allow (Photos and Contacts granted to
 * tcctest, which holds that entitlement for them). assetsd preflights kTCCServicePhotos through
 * -[PLPrivacy photosAccessAllowedWithScope:] and throws "assetsd requires TCCPhotosService
 * access", which leaves Photos without a library. Interposed here: a service the process's own
 * audit token is allowed is "granted"; anything else goes to tccd as before. xnu 0312 inserts it
 * (DYLD_INSERT_LIBRARIES) into the processes named in sweettcc (default: the Photos stack).
 * Linked against tools/phone/TCC.tbd (TCC is a private framework, not in the SDK).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

extern int TCCAccessPreflight(CFStringRef service, CFDictionaryRef options);
extern Boolean TCCAccessCheckAuditToken(CFStringRef service, audit_token_t token, CFDictionaryRef options);

static int
sweet_TCCAccessPreflight(CFStringRef service, CFDictionaryRef options)
{
	audit_token_t token;
	mach_msg_type_number_t n = TASK_AUDIT_TOKEN_COUNT;
	if (task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &n) == KERN_SUCCESS &&
	    TCCAccessCheckAuditToken(service, token, NULL)) {
		return 0;               /* kTCCAccessPreflightGranted */
	}
	return TCCAccessPreflight(service, options);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)sweet_TCCAccessPreflight, (const void *)TCCAccessPreflight },
};
