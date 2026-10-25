/*
 * --daemon for the test tools: how they run as launchd jobs on the phone. tools/taimen/mkrootfix.py --daemon makes
 * RunAtLoad + KeepAlive jobs (ThrottleInterval 10 s) with stdout and stderr on /dev/null, which taimen's kernel tees
 * into its log for the processes boot-arg sweetlogp (xnu 0308) or sweetnull (xnu 0048) names. A tool that exits would
 * be started again every 10 s, so with --daemon as its first argument a tool
 *   - line-buffers stdout: each line is one write, and the log keeps what ran before a crash;
 *   - turns the runtime's trace on (SWEETMTL_TRACE=1, unless the environment sets it);
 *   - on iOS, waits for the GPU service (SweetGPU comes up 10-20 s into boot), 180 s at most;
 *   - runs once per boot: /private/var/tmp/NAME.ran (/tmp/NAME.ran off iOS) holds the boot's identity, written just
 *     before the run; when it matches this boot (launchd restarted the tool after a crash), it does not run again;
 *   - then idles in pause() for good, so KeepAlive has nothing to restart.
 * Remove the marker (or reboot) to run it again. Without --daemon a tool runs once and exits with its status.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SWEETMTL_ONESHOT_H
#define SWEETMTL_ONESHOT_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/time.h>
/* tools/sweetgl/sweet_drm.c (in libsweetgallium.a on iOS): a dup of the device "fd", or -1 while SweetGPU is not up */
int sweet_drm_open(void);
#endif

static int oneshot_daemon;

static void oneshot_boot(char *out, size_t n)
{
#ifdef __APPLE__
	struct timeval tv;
	size_t len = sizeof(tv);
	if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == 0)
		snprintf(out, n, "boot %ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
	else
		snprintf(out, n, "boot unknown");
#else
	char id[64] = "unknown";
	FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (f) {
		if (!fgets(id, sizeof(id), f))
			strcpy(id, "unknown");
		fclose(f);
	}
	id[strcspn(id, "\n")] = 0;
	snprintf(out, n, "boot %s", id);
#endif
}

static void oneshot_idle(const char *name, const char *why)
{
	printf("%s: %s; idling\n", name, why);
	fflush(stdout);
	for (;;)
		pause();
}

/*
 * First thing in main: with --daemon as argv[1], drops it from argv and returns 1 (or idles for good when this boot
 * ran the tool already, or the GPU never came up). Otherwise returns 0 and changes nothing.
 */
static int oneshot_begin(const char *name, int *argc, char ***argv)
{
	if (*argc < 2 || strcmp((*argv)[1], "--daemon") != 0)
		return 0;
	(*argv)[1] = (*argv)[0];
	(*argv)++;
	(*argc)--;
	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("SWEETMTL_TRACE", "1", 0);
	char path[256], boot[96], have[96] = "", why[400];
#ifdef __APPLE__
	snprintf(path, sizeof(path), "/private/var/tmp/%s.ran", name);
#else
	snprintf(path, sizeof(path), "/tmp/%s.ran", name);
#endif
	oneshot_boot(boot, sizeof(boot));
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(have, sizeof(have), f))
			have[0] = 0;
		fclose(f);
		have[strcspn(have, "\n")] = 0;
	}
	if (!strcmp(have, boot)) {
		snprintf(why, sizeof(why), "ran already this %s (remove %s to run again)", boot, path);
		oneshot_idle(name, why);
	}
#ifdef __APPLE__
	int waited = 0, fd;
	while ((fd = sweet_drm_open()) < 0) {
		if (++waited == 360)
			oneshot_idle(name, "no GPU service (SweetGPU) after 180 s");
		usleep(500000);
	}
	close(fd); /* sweet_drm.c keeps its connection; the shim opens its own fd */
	printf("%s: GPU service up after %.1f s\n", name, waited * 0.5);
#endif
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", boot);
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	} else {
		printf("%s: cannot write %s (%s): a crash would run it again\n", name, path, strerror(errno));
	}
	printf("%s: --daemon run, %s\n", name, boot);
	oneshot_daemon = 1;
	return 1;
}

/* Last thing in main: returns status, except under --daemon, where it reports it and idles. */
static int oneshot_end(const char *name, int status)
{
	if (oneshot_daemon) {
		char why[64];
		snprintf(why, sizeof(why), "done, status %d", status);
		oneshot_idle(name, why);
	}
	return status;
}

#endif
