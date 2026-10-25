/*
 * netsvcd: make the phone's USB Ethernet (ue0, 192.168.7.2, xnu bsd/dev/sweet_usb.c) a network
 * service iOS uses. It publishes ue0 in configd's dynamic store as IPConfiguration would for a
 * configured interface, so IPMonitor makes it primary (default route via the PC, 192.168.7.1),
 * mDNSResponder gets DNS servers and reachability says yes. The PC NATs for it
 * (tools/windows/usb-share.ps1). After the Pixel 4 port's netsvc (NWylynko/ios-flame,
 * phone/tools/netsvc.m), in C on CoreFoundation.
 *   netsvcd            daemon (launchd job com.sweet.netsvcd): wait for configd and ue0, publish,
 *                      then republish whenever the keys are gone (configd restarted)
 *   netsvcd once|show|down
 * Messages go to stdout (launchd points it at /dev/console, so they reach the kernel log).
 * Signed with netsvcd-ents.plist (SCDynamicStore write access).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* SystemConfiguration has these on iOS; the SDK only marks them unavailable */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);
extern Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);
extern Boolean SCDynamicStoreRemoveValue(SCDynamicStoreRef, CFStringRef);
extern int SCError(void);
extern const char *SCErrorString(int);

#define IFNAME  "ue0"
#define ADDR    "192.168.7.2"
#define MASK    "255.255.255.0"
#define ROUTER  "192.168.7.1"
#define SVC     "5EE7C0DE-0000-4000-8000-000000000001"     /* the Pixel 4 port's id for ue0 */

static const char *keys[] = {
	"State:/Network/Service/" SVC "/IPv4",
	"State:/Network/Service/" SVC "/DNS",
	"State:/Network/Interface/" IFNAME "/IPv4",
};

static void
say(const char *fmt, const char *a)
{
	printf("netsvcd: ");
	printf(fmt, a);
	printf("\n");
	fflush(stdout);
}

static CFStringRef
str(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CFArrayRef
arr(int n, const char **v)
{
	CFStringRef s[4];
	for (int i = 0; i < n; i++) {
		s[i] = str(v[i]);
	}
	CFArrayRef a = CFArrayCreate(NULL, (const void **)s, n, &kCFTypeArrayCallBacks);
	for (int i = 0; i < n; i++) {
		CFRelease(s[i]);
	}
	return a;
}

static CFDictionaryRef
dict(int n, const char **k, CFTypeRef *v)
{
	CFStringRef ks[8];
	for (int i = 0; i < n; i++) {
		ks[i] = str(k[i]);
	}
	CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)ks, (const void **)v, n,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	for (int i = 0; i < n; i++) {
		CFRelease(ks[i]);
		CFRelease(v[i]);
	}
	return d;
}

/* ue0 has its address (the kernel sets it when the gadget starts) */
static int
ue0_up(void)
{
	struct ifaddrs *ifa, *i;
	int up = 0;
	if (getifaddrs(&ifa) != 0) {
		return 0;
	}
	for (i = ifa; i; i = i->ifa_next) {
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, IFNAME) == 0 &&
		    ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == inet_addr(ADDR)) {
			up = 1;
		}
	}
	freeifaddrs(ifa);
	return up;
}

static int
publish(SCDynamicStoreRef st)
{
	const char *addr[] = { ADDR }, *mask[] = { MASK }, *dns[] = { "1.1.1.1", "8.8.8.8" };
	const char *k4[] = { "Addresses", "SubnetMasks", "Router", "InterfaceName", "ConfigMethod" };
	CFTypeRef v4[] = { arr(1, addr), arr(1, mask), str(ROUTER), str(IFNAME), str("Manual") };
	const char *kd[] = { "ServerAddresses" };
	CFTypeRef vd[] = { arr(2, dns) };
	const char *ki[] = { "Addresses", "SubnetMasks" };
	CFTypeRef vi[] = { arr(1, addr), arr(1, mask) };
	const char *top[] = { keys[0], keys[1], keys[2] };
	CFTypeRef vals[] = { dict(5, k4, v4), dict(1, kd, vd), dict(2, ki, vi) };
	CFDictionaryRef all = dict(3, top, vals);
	Boolean ok = SCDynamicStoreSetMultiple(st, all, NULL, NULL);
	CFRelease(all);
	say("published " IFNAME " (router " ROUTER ", DNS 1.1.1.1 8.8.8.8): %s", ok ? "ok" : SCErrorString(SCError()));
	return ok;
}

static int
present(SCDynamicStoreRef st)
{
	CFStringRef k = str(keys[0]);
	CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
	CFRelease(k);
	if (v) {
		CFRelease(v);
	}
	return v != NULL;
}

static void
show(SCDynamicStoreRef st)
{
	const char *more[] = { "State:/Network/Global/IPv4", "State:/Network/Global/DNS", keys[0], keys[1], keys[2] };
	for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
		CFStringRef k = str(more[i]);
		CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
		printf("%s = ", more[i]);
		if (v) {
			CFStringRef d = CFCopyDescription(v);
			char buf[2048];
			CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
			printf("%s\n", buf);
			CFRelease(d);
			CFRelease(v);
		} else {
			printf("(none)\n");
		}
		CFRelease(k);
	}
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "daemon";
	SCDynamicStoreRef st = NULL;
	int waited = 0;
	while ((st = SCDynamicStoreCreate(NULL, CFSTR("netsvcd"), NULL, NULL)) == NULL) {
		if (strcmp(cmd, "daemon") != 0) {
			say("no dynamic store: %s", SCErrorString(SCError()));
			return 1;
		}
		if (waited++ == 0) {
			say("waiting for configd%s", "");
		}
		sleep(2);
	}
	if (strcmp(cmd, "show") == 0) {
		show(st);
		return 0;
	}
	if (strcmp(cmd, "down") == 0) {
		for (unsigned i = 0; i < 3; i++) {
			CFStringRef k = str(keys[i]);
			SCDynamicStoreRemoveValue(st, k);
			CFRelease(k);
		}
		say("%s removed", IFNAME);
		return 0;
	}
	if (strcmp(cmd, "once") == 0) {
		int ok = publish(st);
		sleep(1);               /* let IPMonitor recompute the primary service */
		show(st);
		return ok ? 0 : 1;
	}
	waited = 0;
	while (!ue0_up()) {
		if (waited++ == 0) {
			say("waiting for " IFNAME " (%s)", ADDR);
		}
		sleep(2);
	}
	publish(st);
	sleep(2);
	show(st);
	for (;;) {
		sleep(10);
		if (!present(st)) {
			CFRelease(st);
			while ((st = SCDynamicStoreCreate(NULL, CFSTR("netsvcd"), NULL, NULL)) == NULL) {
				sleep(2);
			}
			say("keys gone (configd restarted?)%s", "");
			publish(st);
		}
	}
}
