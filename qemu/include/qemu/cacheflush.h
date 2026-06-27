/*
 * Flush host instruction and data caches for generated code.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_CACHEFLUSH_H
#define QEMU_CACHEFLUSH_H

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm__))
#include <libkern/OSCacheControl.h>
#endif

static inline void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len)
{
#if defined(__i386__) || defined(__x86_64__) || defined(__s390__)
    /* Instruction and data caches are coherent. */
    (void)rx;
    (void)rw;
    (void)len;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm__))
    sys_dcache_flush((void *)rw, len);
    sys_icache_invalidate((void *)rx, len);
#else
    if (rw != rx) {
        __builtin___clear_cache((char *)rw, (char *)rw + len);
    }
    __builtin___clear_cache((char *)rx, (char *)rx + len);
#endif
}

#endif /* QEMU_CACHEFLUSH_H */
