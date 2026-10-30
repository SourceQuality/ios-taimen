/*
 * oslogd: iOS's unified log (os_log) for named processes, printed on stdout, so that a launchd job
 * with sweetlogp=oslogd puts it into the XNU log. sweetlogp's OS_ACTIVITY_DT_MODE mirrors nothing
 * of os_log to stderr on this port (boot 176: locationd's orientation detector never showed), and
 * iOS has no `log stream`.
 *
 *   oslogd [--debug] [--daemon] PROC[,PROC...]
 *
 * Reads the live stream LoggingSupport's activity-stream SPI gives diagnosticd's clients
 * (os_activity_stream_for_pid; entitlement com.apple.diagnosticd.diagnostic), the interface lldb's
 * debugserver used (DarwinLog). The declarations below are only the fields read here. Messages at
 * default and info level (--debug: debug too) of the processes whose executable name is in the
 * list, as "|oslog| NAME[pid] message". --daemon: as a KeepAlive launchd job,
 * idle forever once the stream fails instead of exiting (no restart loop).
 */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

/* the activity-stream SPI: flags, entry types, events */
enum {
	STREAM_PROCESS_ONLY = 0x1,
	STREAM_INFO = 0x100,
	STREAM_DEBUG = 0x20,
	TYPE_LOG_MESSAGE = 0x0400,
	EVENT_STARTED = 1, EVENT_STOPPED = 2, EVENT_FAILED = 3,
};

/* the fields every entry starts its payload with, then a log message's own */
struct log_message {
	uint64_t trace_id, timestamp, thread;
	const uint8_t *image_uuid;
	const char *image_path;
	struct timeval tv_gmt;
	struct timezone tz;
	uint32_t offset;
	const char *format;
	const uint8_t *buffer;
	size_t buffer_sz;
	const uint8_t *privdata;
	size_t privdata_sz;
	const char *subsystem;
	const char *category;
	uint32_t oversize_id;
	uint8_t ttl;
	bool persisted;
};

struct stream_entry {
	uint32_t type;
	pid_t pid;
	uint64_t proc_id;
	const uint8_t *proc_imageuuid;
	const char *proc_imagepath;
	uint64_t activity_id, parent_id;
	struct log_message log_message;         /* the first member of the payload union */
};

typedef struct stream *stream_t;
typedef bool (^entry_block_t)(struct stream_entry *entry, int error);
typedef void (^event_block_t)(stream_t stream, uint32_t event);

static stream_t (*stream_for_pid)(pid_t pid, uint32_t flags, entry_block_t block);
static void (*stream_resume)(stream_t stream);
static void (*stream_set_event_handler)(stream_t stream, event_block_t block);
static char *(*copy_formatted_message)(struct log_message *message);

static char **names;
static int nnames;

/* libproc (not in the iOS SDK's headers) */
#define PROC_ALL_PIDS           1
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

#define MAX_WATCHED             64
static pid_t watched[MAX_WATCHED];
static int nwatched;

static bool
wanted(const char *path)
{
	const char *base = path != NULL ? strrchr(path, '/') : NULL;
	base = base != NULL ? base + 1 : path;
	if (base == NULL) {
		return false;
	}
	for (int i = 0; i < nnames; i++) {
		if (strcmp(base, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

int
main(int argc, char **argv)
{
	__block unsigned long shown = 0;        /* before any goto: a jump may not skip a __block variable */
	bool debug = false, daemon = false;
	char list[512] = "";
	/* every other argument names processes, alone or comma-separated: mkrootfix --daemon splits its
	 * arguments on commas, so "--daemon,locationd,backboardd" arrives as two (boot 181 kept only the last) */
	names = calloc((size_t)argc * 64, sizeof(char *));
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			debug = true;
		} else if (strcmp(argv[i], "--daemon") == 0) {
			daemon = true;
		} else {
			char *copy = strdup(argv[i]);
			for (char *s = copy, *t; (t = strsep(&s, ",")) != NULL;) {
				if (*t && nnames < argc * 64) {
					names[nnames++] = t;
					strlcat(list, list[0] ? "," : "", sizeof(list));
					strlcat(list, t, sizeof(list));
				}
			}
		}
	}
	if (nnames == 0) {
		fprintf(stderr, "usage: oslogd [--debug] [--daemon] PROC[,PROC...] [PROC...]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	void *ls = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	stream_for_pid = dlsym(RTLD_DEFAULT, "os_activity_stream_for_pid");
	stream_resume = dlsym(RTLD_DEFAULT, "os_activity_stream_resume");
	stream_set_event_handler = dlsym(RTLD_DEFAULT, "os_activity_stream_set_event_handler");
	copy_formatted_message = dlsym(RTLD_DEFAULT, "os_log_copy_formatted_message");
	if (ls == NULL || stream_for_pid == NULL || stream_resume == NULL || stream_set_event_handler == NULL ||
	    copy_formatted_message == NULL) {
		printf("oslogd: no activity-stream SPI (LoggingSupport %p)\n", ls);
		goto idle;
	}

	/*
	 * One process-only stream per running process of the list: boot 181 streamed every process and kept the
	 * wanted ones here, which cost diagnosticd ~150% and oslogd ~75% of a CPU. The process list is scanned
	 * every 2 s for new ones (a daemon restarted, an app launched).
	 */
	uint32_t flags = STREAM_INFO | STREAM_PROCESS_ONLY | (debug ? STREAM_DEBUG : 0);
	entry_block_t on_entry = ^bool (struct stream_entry *e, int error) {
		if (error != 0 || e == NULL || e->type != TYPE_LOG_MESSAGE) {
			return true;
		}
		/*
		 * No subsystem:category: in iOS 16's entries those two fields are not where DarwinLog's
		 * declaration puts them (always NULL here, and 0x10 in AegirPoster's messages, which crashed
		 * printf: boot 185, four oslogd crashes). The formatted text and the process fields are right.
		 */
		char *text = copy_formatted_message(&e->log_message);
		const char *base = e->proc_imagepath != NULL ? strrchr(e->proc_imagepath, '/') : NULL;
		printf("|oslog| %s[%d] %s\n", base != NULL ? base + 1 : (e->proc_imagepath ? e->proc_imagepath : "?"),
		    e->pid, text != NULL ? text : "(null)");
		free(text);
		shown++;
		return true;
	};
	printf("oslogd: streaming %s messages of %s\n", debug ? "default, info and debug" : "default and info", list);
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{
		static int pids[4096];
		int n = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)) / (int)sizeof(int);
		for (int j = 0; j < nwatched;) {        /* forget the ones that are gone */
			if (kill(watched[j], 0) != 0 && errno == ESRCH) {
				watched[j] = watched[--nwatched];
			} else {
				j++;
			}
		}
		for (int i = 0; i < n; i++) {
			char path[4096];
			if (pids[i] <= 0 || proc_pidpath(pids[i], path, sizeof(path)) <= 0 || !wanted(path)) {
				continue;
			}
			bool known = false;
			for (int j = 0; j < nwatched; j++) {
				known = known || watched[j] == pids[i];
			}
			if (known || nwatched >= MAX_WATCHED) {
				continue;
			}
			stream_t s = stream_for_pid(pids[i], flags, on_entry);
			if (s == NULL) {
				printf("oslogd: no stream for %s[%d] (entitlement com.apple.diagnosticd.diagnostic?)\n", path, pids[i]);
				continue;
			}
			stream_set_event_handler(s, ^(stream_t st, uint32_t event) {
				(void)st;
				if (event == EVENT_FAILED) {
					printf("oslogd: a stream FAILED (%lu messages shown)\n", shown);
				}
			});
			stream_resume(s);
			watched[nwatched++] = pids[i];
			printf("oslogd: watching %s[%d]\n", strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path, pids[i]);
		}
	});
	dispatch_resume(timer);
	dispatch_main();

idle:
	if (daemon) {
		for (;;) {
			pause();
		}
	}
	return 1;
}
