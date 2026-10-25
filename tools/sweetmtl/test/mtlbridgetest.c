/*
 * mtlbridgetest: the bridge's self-test (tools/sweetmtl/bridge, nmb_selftest) on sweetmtl-runtime: device, the
 * translator's fixture library, a compute kernel (1000 outputs), a render pass (8x8), a render into imported host
 * pages (64x64), then, given QuartzCore's default.metallib, every kernel and vertex/fragment pair of it built into
 * pipelines. After flame's phone/metal2/m2vtest.c.
 *
 *   mtlbridgetest [CACHE_DIR] [METALLIB]
 *
 * On the PC under Mesa's freedreno drm-shim (tools/sweetmtl/host-check.sh) nothing executes: the steps run, record
 * and submit, but the result counts read memory no GPU wrote (SWEETMTL_DRM_SHIM=1 says so instead of judging them).
 *
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "NativeMetalBridge.h"

/*
 * The bridge cleans and invalidates host pages around the GPU's use (libSystem's sys_dcache_flush on iOS, which assumes
 * Apple's coherent GPU, so the test defines its own as flame's m2vtest does). On the PC the shim's memory is the CPU's.
 */
void sys_dcache_flush(void *start, size_t len)
{
#if defined(__aarch64__) && defined(__APPLE__)
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)start;
	(void)len;
#endif
}

static double t0;
static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void mark(uint32_t m)
{
	static const char *const names[] = {"", "device", "fixture library", "compute", "render", "import", "QuartzCore"};
	unsigned i = m & 0xff;
	printf("[%7.3f s] step %x %s\n", now() - t0, m, i < 7 ? names[i] : (i == 0xf ? "done" : "?"));
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *cache = argc > 1 ? argv[1] : "/tmp/sweetmtl-cache";
	const char *qc = argc > 2 ? argv[2] : NULL;
	int shim = getenv("SWEETMTL_DRM_SHIM") != NULL;
	mkdir(cache, 0777);
	uint8_t *bytes = NULL;
	size_t n = 0;
	if (qc) {
		int fd = open(qc, O_RDONLY);
		struct stat st;
		if (fd >= 0 && fstat(fd, &st) == 0) {
			n = (size_t)st.st_size;
			bytes = malloc(n);
			if (read(fd, bytes, n) != (ssize_t)n)
				n = 0;
			close(fd);
		}
		printf("QuartzCore metallib: %zu bytes\n", n);
	}
	struct NmbSelftest out;
	memset(&out, 0, sizeof(out));
	t0 = now();
	uint32_t r = nmb_selftest("", cache, bytes, n, mark, &out);
	printf("result %x stage %x (%.2f s)\n", r, out.stage, now() - t0);
	if (out.stage == 0 || out.stage > 0x6503) {
		if (shim)
			printf("compute: ran; outputs not checkable on drm-shim (no execution): %u/1000 differ from 3i\n",
			       out.compute_bad);
		else
			printf("compute bad %u/1000\n", out.compute_bad);
	}
	if (out.stage == 0 || out.stage > 0x6504) {
		if (shim)
			printf("render: ran; texels not checkable on drm-shim: %u/64 differ\n", out.render_bad);
		else
			printf("render bad %u/64\n", out.render_bad);
	}
	if (out.stage == 0 || out.stage > 0x6505) {
		if (shim)
			printf("import: ran; texels not checkable on drm-shim: %u/4096 differ\n", out.import_bad);
		else
			printf("import bad %u/4096\n", out.import_bad);
	}
	if (n)
		printf("QuartzCore: %u functions, kernels %u ok %u failed, vertex/fragment pairs %u ok %u failed\n",
		       out.qc_functions, out.qc_kernels_ok, out.qc_kernels_failed, out.qc_pairs_ok, out.qc_pairs_failed);
	printf("message: %.96s\n", out.message);
	free(bytes);
	/* let the runtime's threads wind down (their trace lines) before the process exits */
	usleep(200000);
	return out.stage != 0;
}
