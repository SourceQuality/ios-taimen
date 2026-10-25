/*
 * sweetexc.dylib: says why a process dies of an uncaught Objective-C exception. Without it an
 * NSException thrown on a background queue (libdispatch calls std::terminate) prints only
 * "libc++abi: terminating due to uncaught exception of type NSException" in processes that never
 * installed Foundation's logging handler (app extensions, daemons). The kernel injects it with
 * DYLD_INSERT_LIBRARIES into the processes named in boot-arg sweetlogp (xnu 0311), whose stderr is
 * teed into the kernel log; this prints the exception's name, reason and the throw site's
 * backtrace with each frame's image and offset (and the shared cache slide, for
 * tools/sweetgl/dsc-sym.py), then lets the process die as before.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <string.h>

extern intptr_t _dyld_get_image_slide(const struct mach_header *mh);
extern const void *_dyld_get_shared_cache_range(size_t *length);

static NSUncaughtExceptionHandler *previous;

static void
sweetexc_handler(NSException *e)
{
	@autoreleasepool {
		size_t len = 0;
		const void *cache = _dyld_get_shared_cache_range(&len);
		fprintf(stderr, "sweetexc: uncaught %s: %s (shared cache at %p)\n", e.name.UTF8String ?: "?",
		    e.reason.UTF8String ?: "(no reason)", cache);
		NSArray<NSNumber *> *pcs = e.callStackReturnAddresses;
		for (NSUInteger i = 0; i < pcs.count && i < 40; i++) {
			void *pc = (void *)pcs[i].unsignedLongValue;
			Dl_info info;
			if (dladdr(pc, &info) && info.dli_fname) {
				const char *img = strrchr(info.dli_fname, '/');
				fprintf(stderr, "sweetexc: #%lu %p %s`%s+0x%lx\n", (unsigned long)i, pc, img ? img + 1 : info.dli_fname,
				    info.dli_sname ?: "?", info.dli_saddr ? (unsigned long)((char *)pc - (char *)info.dli_saddr) : 0UL);
			} else {
				fprintf(stderr, "sweetexc: #%lu %p\n", (unsigned long)i, pc);
			}
		}
		if (e.userInfo.count) {
			fprintf(stderr, "sweetexc: userInfo %s\n", e.userInfo.description.UTF8String);
		}
		fflush(stderr);
	}
	if (previous) {
		previous(e);
	}
}

__attribute__((constructor)) static void
sweetexc_init(void)
{
	previous = NSGetUncaughtExceptionHandler();
	NSSetUncaughtExceptionHandler(sweetexc_handler);
}
