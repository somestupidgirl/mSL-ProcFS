/*
 * libbsdmalloc - BSD-style _MALLOC()/_FREE() for third-party kexts.
 *
 * The kernel's <sys/malloc.h> only prototypes _MALLOC()/_FREE() in its
 * !XNU_KERNEL_PRIVATE branch (they are deprecated in favour of kalloc_type()),
 * so they are invisible in a kernel-private build. The symbols are still
 * exported BSD KPI, however, so this wrapper pulls in the real <sys/malloc.h>
 * behind it (via #include_next) and re-declares them, along with the classic
 * malloc()/free() spelling.
 *
 * Because this header is itself named <sys/malloc.h>, it must be reached first
 * on the include path; #include_next then continues on to the kernel header.
 */
#ifndef LIBBSDMALLOC_SYS_MALLOC_H
#define LIBBSDMALLOC_SYS_MALLOC_H

#include_next <sys/malloc.h>

extern void *_MALLOC(size_t size, int type, int flags);
extern void  _FREE(void *addr, int type);

#define malloc(size, type, flags)               _MALLOC(size, type, flags)
#define free(addr, type)                        _FREE(addr, type)

#endif /* LIBBSDMALLOC_SYS_MALLOC_H */
