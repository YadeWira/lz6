/* LD_PRELOAD shim: fread() on any FILE whose fd is > 2 fails like a failed stdio read (0 items, the error flag set, errno EIO) once
 * FAILREAD_AFTER bytes have been read through it. (glibc's stdio calls its own read(), which preload cannot intercept: fread is the seam.) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
static long total, limit = -2;
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
    static size_t (*real)(void*, size_t, size_t, FILE*);
    if (!real) real = dlsym(RTLD_NEXT, "fread");
    if (limit == -2) { const char* e = getenv("FAILREAD_AFTER"); limit = e ? atol(e) : -1; }
    if (limit >= 0 && fileno(f) > 2) {
        if (total >= limit) { f->_flags |= 0x20; errno = EIO; return 0; }     /* _IO_ERR_SEEN */
        if (total + (long)(size * nmemb) > limit) nmemb = (size_t)(limit - total) / size;
    }
    size_t r = real(ptr, size, nmemb, f);
    if (limit >= 0 && fileno(f) > 2) total += (long)(r * size);
    return r;
}
