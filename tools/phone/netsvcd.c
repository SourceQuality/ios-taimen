/*
 * netsvcd: make the phone's USB Ethernet (ue0, 192.168.7.2, xnu bsd/dev/sweet_usb.c) a network
 * service iOS uses. It publishes ue0 in configd's dynamic store as IPConfiguration would for a
 * configured interface, so IPMonitor makes it primary (default route via the PC, 192.168.7.1),
 * mDNSResponder gets DNS servers and reachability says yes. After the Pixel 4 port's netsvc
 * (NWylynko/ios-flame, phone/tools/netsvc.m), in C on CoreFoundation.
 *
 * Internet without admin rights on the PC: the PC can only be a router with them (NAT:
 * tools/windows/usb-share.ps1), but it can always connect out to the phone. So the service also
 * carries an HTTP/HTTPS proxy, 127.0.0.1:8080, and netsvcd splices every connection to it onto
 * one of the idle connections the PC's agent (tools/taimen/usbproxy.py) keeps open to
 * 192.168.7.2:5142; the agent is an ordinary HTTP proxy on its end.
 *
 * Status: connect to 192.168.7.2:5143 (the PC: tools/taimen/usbproxy.py --status) for the store keys and
 * counters. (stdout goes to /dev/console, which does not reach the kernel log on taimen.)
 *   netsvcd            daemon (launchd job com.sweet.netsvcd): wait for configd and ue0, publish,
 *                      republish whenever the keys are gone (configd restarted), serve the proxy
 *   netsvcd once|show|down
 * Signed with netsvcd-ents.plist (SCDynamicStore write access).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* SystemConfiguration has these on iOS; the SDK only marks them unavailable */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);
extern Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);
extern Boolean SCDynamicStoreRemoveValue(SCDynamicStoreRef, CFStringRef);
extern int SCError(void);
extern const char *SCErrorString(int);

#define IFNAME          "ue0"
#define ADDR            "192.168.7.2"
#define MASK            "255.255.255.0"
#define ROUTER          "192.168.7.1"
#define SVC             "5EE7C0DE-0000-4000-8000-000000000001"  /* the Pixel 4 port's id for ue0 */
#define PROXY_PORT      8080
#define AGENT_PORT      5142
#define STATUS_PORT     5143
#define MAXIDLE         32

static const char *keys[] = {
	"State:/Network/Service/" SVC "/IPv4",
	"State:/Network/Service/" SVC "/DNS",
	"State:/Network/Interface/" IFNAME "/IPv4",
	"State:/Network/Service/" SVC "/Proxies",
	"Setup:/Network/Service/" SVC "/Proxies",
};
#define NKEYS   (sizeof(keys) / sizeof(keys[0]))

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int idle[MAXIDLE], nidle;
static unsigned long n_agents, n_clients, n_spliced, n_noagent, n_publish;
static char events[4096];       /* the last messages, for the status port */

static void
say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("netsvcd: %s\n", line);
	fflush(stdout);
	pthread_mutex_lock(&mu);
	time_t t = time(NULL);
	char stamp[16];
	strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&t));
	size_t used = strlen(events), add = strlen(stamp) + strlen(line) + 2;
	if (used + add >= sizeof(events)) {
		memmove(events, events + sizeof(events) / 2, strlen(events + sizeof(events) / 2) + 1);
	}
	snprintf(events + strlen(events), sizeof(events) - strlen(events), "%s %s\n", stamp, line);
	pthread_mutex_unlock(&mu);
}

static CFStringRef
str(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CFNumberRef
num(int v)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &v);
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

/* a dictionary of n keys; takes over the values */
static CFDictionaryRef
dict(int n, const char **k, CFTypeRef *v)
{
	CFStringRef ks[16];
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

static CFDictionaryRef
proxies(void)
{
	const char *exc[] = { "*.local", "169.254/16", "192.168.7.0/24" };
	const char *k[] = { "HTTPEnable", "HTTPProxy", "HTTPPort", "HTTPSEnable", "HTTPSProxy", "HTTPSPort",
		            "ExceptionsList", "ExcludeSimpleHostnames" };
	CFTypeRef v[] = { num(1), str("127.0.0.1"), num(PROXY_PORT), num(1), str("127.0.0.1"), num(PROXY_PORT),
		          arr(3, exc), num(1) };
	return dict(8, k, v);
}

/*
 * The proxy keys go with the service only while the PC's agent is there (idle connections open, or
 * one taken in the last 30 s): with the PC routing for us instead (NAT), a proxy with no agent
 * behind it would fail every request.
 */
static time_t last_take;

static int
agent_alive(void)
{
	pthread_mutex_lock(&mu);
	for (int i = 0; i < nidle;) {           /* drop the ones the PC closed */
		struct pollfd p = { idle[i], POLLIN, 0 };
		if (poll(&p, 1, 0) != 0) {
			close(idle[i]);
			idle[i] = idle[--nidle];
		} else {
			i++;
		}
	}
	int alive = nidle > 0 || time(NULL) - last_take < 30;
	pthread_mutex_unlock(&mu);
	return alive;
}

static int
publish(SCDynamicStoreRef st, int proxied)
{
	const char *addr[] = { ADDR }, *mask[] = { MASK }, *dns[] = { "1.1.1.1", "8.8.8.8" };
	const char *k4[] = { "Addresses", "SubnetMasks", "Router", "InterfaceName", "ConfigMethod" };
	CFTypeRef v4[] = { arr(1, addr), arr(1, mask), str(ROUTER), str(IFNAME), str("Manual") };
	const char *kd[] = { "ServerAddresses" };
	CFTypeRef vd[] = { arr(2, dns) };
	const char *ki[] = { "Addresses", "SubnetMasks" };
	CFTypeRef vi[] = { arr(1, addr), arr(1, mask) };
	CFTypeRef vals[] = { dict(5, k4, v4), dict(1, kd, vd), dict(2, ki, vi), NULL, NULL };
	if (proxied) {
		vals[3] = proxies();
		vals[4] = proxies();
	}
	CFDictionaryRef set = dict(proxied ? 5 : 3, keys, vals);
	CFArrayRef gone = proxied ? NULL : arr(2, keys + 3);
	Boolean ok = SCDynamicStoreSetMultiple(st, set, gone, NULL);
	CFRelease(set);
	if (gone) {
		CFRelease(gone);
	}
	n_publish++;
	say("published " IFNAME " (router " ROUTER ", DNS 1.1.1.1 8.8.8.8, %s): %s",
	    proxied ? "proxy 127.0.0.1:8080 to the PC agent" : "no proxy", ok ? "ok" : SCErrorString(SCError()));
	return ok;
}

static int
present(SCDynamicStoreRef st, int proxied)
{
	int all = 1;
	for (unsigned i = 0; i < (proxied ? NKEYS : 3); i++) {
		CFStringRef k = str(keys[i]);
		CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
		CFRelease(k);
		if (v) {
			CFRelease(v);
		} else {
			all = 0;
		}
	}
	return all;
}

/* the store keys that matter, as text */
static void
show_to(SCDynamicStoreRef st, FILE *f)
{
	const char *more[] = { "State:/Network/Global/IPv4", "State:/Network/Global/DNS", "State:/Network/Global/Proxies",
		               keys[0], keys[1], keys[2], keys[3], keys[4] };
	for (unsigned i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
		CFStringRef k = str(more[i]);
		CFPropertyListRef v = SCDynamicStoreCopyValue(st, k);
		fprintf(f, "%s = ", more[i]);
		if (v) {
			CFStringRef d = CFCopyDescription(v);
			char buf[4096];
			if (!CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) {
				strlcpy(buf, "(too long)", sizeof(buf));
			}
			fprintf(f, "%s\n", buf);
			CFRelease(d);
			CFRelease(v);
		} else {
			fprintf(f, "(none)\n");
		}
		CFRelease(k);
	}
	fflush(f);
}

static int
listen_on(const char *ip, int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
	struct sockaddr_in a;
	if (s < 0) {
		return -1;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(ip);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 64) != 0) {
		say("listen %s:%d: %s", ip, port, strerror(errno));
		close(s);
		return -1;
	}
	return s;
}

static void
nodelay(int s)
{
	int on = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}

static int
write_all(int s, const char *b, ssize_t n)
{
	while (n > 0) {
		ssize_t w = write(s, b, n);
		if (w <= 0) {
			return -1;
		}
		b += w;
		n -= w;
	}
	return 0;
}

/* the PC's agent connections, kept until an app needs one */
static void *
agent_thread(void *arg)
{
	(void)arg;
	int ls;
	while ((ls = listen_on(ADDR, AGENT_PORT)) < 0) {
		sleep(5);
	}
	for (;;) {
		int s = accept(ls, NULL, NULL);
		if (s < 0) {
			sleep(1);
			continue;
		}
		nodelay(s);
		pthread_mutex_lock(&mu);
		if (n_agents++ == 0) {
			pthread_mutex_unlock(&mu);
			say("first agent connection from the PC");
			pthread_mutex_lock(&mu);
		}
		if (nidle < MAXIDLE) {
			idle[nidle++] = s;
			s = -1;
		}
		pthread_cond_broadcast(&cv);
		pthread_mutex_unlock(&mu);
		if (s >= 0) {
			close(s);
		}
	}
	return NULL;
}

/* an idle agent connection that is still open, waiting up to 5 s for one */
static int
take_agent(void)
{
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 5;
	pthread_mutex_lock(&mu);
	for (;;) {
		while (nidle > 0) {
			int s = idle[--nidle];
			struct pollfd p = { s, POLLIN, 0 };
			if (poll(&p, 1, 0) == 0) {       /* nothing to read and not closed: alive */
				last_take = time(NULL);
				pthread_mutex_unlock(&mu);
				return s;
			}
			close(s);
		}
		if (pthread_cond_timedwait(&cv, &mu, &until) == ETIMEDOUT) {
			pthread_mutex_unlock(&mu);
			return -1;
		}
	}
}

struct pair {
	int a, b;
};

static void *
splice_thread(void *arg)
{
	struct pair p = *(struct pair *)arg;
	free(arg);
	char buf[32768];
	struct pollfd f[2] = { { p.a, POLLIN, 0 }, { p.b, POLLIN, 0 } };
	for (int done = 0; !done;) {
		if (poll(f, 2, -1) <= 0 && errno != EINTR) {
			break;
		}
		for (int i = 0; i < 2 && !done; i++) {
			if (f[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				ssize_t n = read(f[i].fd, buf, sizeof(buf));
				if (n <= 0 || write_all(f[1 - i].fd, buf, n) != 0) {
					done = 1;
				}
			}
		}
	}
	close(p.a);
	close(p.b);
	return NULL;
}

static void *
proxy_thread(void *arg)
{
	(void)arg;
	int ls;
	while ((ls = listen_on("127.0.0.1", PROXY_PORT)) < 0) {
		sleep(5);
	}
	for (;;) {
		int c = accept(ls, NULL, NULL);
		if (c < 0) {
			sleep(1);
			continue;
		}
		nodelay(c);
		n_clients++;
		int a = take_agent();
		if (a < 0) {
			static const char no[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			if (n_noagent++ % 50 == 0) {
				say("app connection with no agent from the PC (tools/taimen/usbproxy.py): %lu so far", n_noagent);
			}
			write_all(c, no, sizeof(no) - 1);
			close(c);
			continue;
		}
		struct pair *p = malloc(sizeof(*p));
		pthread_t t;
		p->a = c;
		p->b = a;
		if (pthread_create(&t, NULL, splice_thread, p) == 0) {
			pthread_detach(t);
			n_spliced++;
		} else {
			free(p);
			close(c);
			close(a);
		}
	}
	return NULL;
}

static SCDynamicStoreRef store(int wait);

static void *
status_thread(void *arg)
{
	(void)arg;
	SCDynamicStoreRef st = store(1);        /* its own: the main loop uses the other */
	int ls;
	while ((ls = listen_on(ADDR, STATUS_PORT)) < 0) {
		sleep(5);
	}
	for (;;) {
		int c = accept(ls, NULL, NULL);
		if (c < 0) {
			sleep(1);
			continue;
		}
		FILE *f = fdopen(c, "w");
		if (f == NULL) {
			close(c);
			continue;
		}
		pthread_mutex_lock(&mu);
		fprintf(f, "netsvcd: published %lu times; agents %lu (%d idle); app connections %lu, spliced %lu, "
		    "without an agent %lu\n%s", n_publish, n_agents, nidle, n_clients, n_spliced, n_noagent, events);
		pthread_mutex_unlock(&mu);
		show_to(st, f);
		fclose(f);
	}
	return NULL;
}

static SCDynamicStoreRef
store(int wait)
{
	SCDynamicStoreRef st;
	int waited = 0;
	while ((st = SCDynamicStoreCreate(NULL, CFSTR("netsvcd"), NULL, NULL)) == NULL) {
		if (!wait) {
			say("no dynamic store: %s", SCErrorString(SCError()));
			return NULL;
		}
		if (waited++ == 0) {
			say("waiting for configd");
		}
		sleep(2);
	}
	return st;
}

int
main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "daemon";
	signal(SIGPIPE, SIG_IGN);
	SCDynamicStoreRef st = store(strcmp(cmd, "daemon") == 0);
	if (st == NULL) {
		return 1;
	}
	if (strcmp(cmd, "show") == 0) {
		show_to(st, stdout);
		return 0;
	}
	if (strcmp(cmd, "down") == 0) {
		for (unsigned i = 0; i < NKEYS; i++) {
			CFStringRef k = str(keys[i]);
			SCDynamicStoreRemoveValue(st, k);
			CFRelease(k);
		}
		say("%s removed", IFNAME);
		return 0;
	}
	if (strcmp(cmd, "once") == 0) {
		int ok = publish(st, 0);
		sleep(1);               /* let IPMonitor recompute the primary service */
		show_to(st, stdout);
		return ok ? 0 : 1;
	}
	int waited = 0;
	while (!ue0_up()) {
		if (waited++ == 0) {
			say("waiting for " IFNAME " (%s)", ADDR);
		}
		sleep(2);
	}
	pthread_t t;
	pthread_create(&t, NULL, agent_thread, NULL);
	pthread_create(&t, NULL, proxy_thread, NULL);
	pthread_create(&t, NULL, status_thread, NULL);
	int proxied = -1;
	for (;;) {
		int want = agent_alive();
		if (want != proxied) {
			publish(st, want);
			proxied = want;
		} else if (!present(st, proxied)) {
			say("keys gone (configd restarted, or prefs reloaded): publishing again");
			publish(st, proxied);
		}
		sleep(2);
	}
}
