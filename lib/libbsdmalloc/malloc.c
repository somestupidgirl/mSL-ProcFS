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
 */
#include <sys/types.h>
#include <sys/malloc.h>

/*
 * <sys/malloc.h> defines malloc() as a function-like macro (the M_ZERO
 * zero-in-place optimisation), so the name is parenthesised here to suppress
 * that expansion and define the underlying function - the same idiom FreeBSD
 * uses in sys/kern/kern_malloc.c.
 */
void *
(malloc)(size_t size, int type, int flags)
{
	return _MALLOC(size, type, flags);
}

void
free(void *addr, int type)
{
	_FREE(addr, type);
}
