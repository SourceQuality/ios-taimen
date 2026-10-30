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
 * list, as "|oslog| NAME[pid] subsystem:category message". --daemon: as a KeepAlive launchd job,
 * idle forever once the stream fails instead of exiting (no restart loop).
 */
#include <dlfcn.h>
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
	const char *list = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--debug") == 0) {
			debug = true;
		} else if (strcmp(argv[i], "--daemon") == 0) {
			daemon = true;
		} else {
			list = argv[i];
		}
	}
	if (list == NULL) {
		fprintf(stderr, "usage: oslogd [--debug] [--daemon] PROC[,PROC...]\n");
		return 2;
	}
	char *copy = strdup(list);
	names = calloc(strlen(list) + 1, sizeof(char *));
	for (char *s = copy, *t; (t = strsep(&s, ",")) != NULL;) {
		if (*t) {
			names[nnames++] = t;
		}
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

	stream_t stream = stream_for_pid(-1, STREAM_INFO | (debug ? STREAM_DEBUG : 0),
	    ^bool (struct stream_entry *e, int error) {
		if (error != 0 || e == NULL || e->type != TYPE_LOG_MESSAGE || !wanted(e->proc_imagepath)) {
			return true;
		}
		char *text = copy_formatted_message(&e->log_message);
		const char *base = strrchr(e->proc_imagepath, '/');
		printf("|oslog| %s[%d] %s:%s %s\n", base != NULL ? base + 1 : e->proc_imagepath, e->pid,
		    e->log_message.subsystem != NULL ? e->log_message.subsystem : "",
		    e->log_message.category != NULL ? e->log_message.category : "", text != NULL ? text : "(null)");
		free(text);
		shown++;
		return true;
	});
	if (stream == NULL) {
		printf("oslogd: os_activity_stream_for_pid refused (entitlement com.apple.diagnosticd.diagnostic?)\n");
		goto idle;
	}
	stream_set_event_handler(stream, ^(stream_t s, uint32_t event) {
		(void)s;
		printf("oslogd: stream %s (%lu messages shown)\n", event == EVENT_STARTED ? "started" :
		    event == EVENT_STOPPED ? "stopped" : event == EVENT_FAILED ? "FAILED" : "event", shown);
	});
	stream_resume(stream);
	printf("oslogd: streaming %s messages of %s\n", debug ? "default, info and debug" : "default and info", list);
	dispatch_main();

idle:
	if (daemon) {
		for (;;) {
			pause();
		}
	}
	return 1;
}
