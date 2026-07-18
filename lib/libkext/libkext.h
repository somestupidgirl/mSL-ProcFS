/**
 * Minimalist's kernel extension library
 *
 * Created 180826  lynnl
 */

#ifndef LIBKEXT_H
#define LIBKEXT_H

#include <libkern/libkern.h>    /* printf() */
#include <sys/types.h>
#include <uuid/uuid.h>

/*
 * Make STATIC do nothing in debug mode, so that all static
 * functions and variables are available in the debugger.
 */
#if DEBUG
#define STATIC
#else /* DEBUG */
#define STATIC static
#endif /* DEBUG */

#define readonly_extern     extern  /* Cheap annotation */

/*
 * Used to indicate unused function parameters
 * see: <sys/cdefs.h>#__unused
 */
#define UNUSED(e, ...)      (void) ((void) (e), ##__VA_ARGS__)

/*
 * G for GCC-specific
 * see:
 *  <sys/param.h>#MIN, MAX
 *  <libkern/libkern.h>#min, max
 */
#define GMIN(a, b) ({       \
    __typeof(a) _a = (a);   \
    __typeof(b) _b = (b);   \
    _a < _b ? _a : _b;      \
})

#define GMAX(a, b) ({       \
    __typeof(a) _a = (a);   \
    __typeof(b) _b = (b);   \
    _a > _b ? _a : _b;      \
})

#define ARRAY_COUNT(a)      (sizeof((a)) / sizeof((a)[0]))
#define ARRAY_SIZE(a)       (sizeof(a) / sizeof(*a))
#define ARRAY_LAST(a)       (ARRAY_SIZE(a) - 1u)

/**
 * Prefix for every message logged through the macros below.
 *
 * A consumer that builds libkext as part of a larger kernel extension can
 * define this to its own kext name (-DKEXTNAME_S=\"foo\") so that its
 * translation units log under that name; a standalone build of the library
 * falls back to the library's own name.
 *
 * Note this is a compile-time string literal, so it is baked into whichever
 * translation unit expands it. libkext's own sources reach it through
 * LOG_BUG() and, in non-DEBUG builds, kassert().
 */
#ifndef KEXTNAME_S
#define KEXTNAME_S "libkext"
#endif

/**
 * os_log() is only available on macOS 10.12 or newer
 *  thus os_log do have compatibility issue  use printf instead
 *
 * XNU kernel version of printf() don't recognize some rarely used specifiers
 *  like h, i, j, t  use unrecognized spcifier may raise kernel panic
 *
 * Feel free to print NULL as %s  it checked explicitly by kernel-printf
 *
 * see: xnu/osfmk/kern/printf.c#printf
 */
#define LOG(fmt, ...)        printf(KEXTNAME_S ": " fmt "\n", ##__VA_ARGS__)

#define LOG_INF(fmt, ...)    LOG("INF " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   LOG("[WARN] " fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)    LOG("ERR " fmt, ##__VA_ARGS__)
#define LOG_BUG(fmt, ...)    LOG("BUG " fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...)  LOG("[TRACE] " fmt, ##__VA_ARGS__)
#define LOG_OFF(fmt, ...)    (void) (0, ##__VA_ARGS__)
#ifdef DEBUG
#define LOG_DBG(fmt, ...)    LOG("DBG " fmt, ##__VA_ARGS__)
#else
#define LOG_DBG(fmt, ...)    LOG_OFF(fmt, ##__VA_ARGS__)
#endif

#define panicf(fmt, ...)                \
    panic("\n" fmt "\n%s@%s#L%d\n\n",   \
            ##__VA_ARGS__, __BASE_FILE__, __FUNCTION__, __LINE__)

#ifdef DEBUG
/*
 * NOTE: Do NOT use any multi-nary conditional/logical operator inside assertion
 *       like operators && || ?:  it's extremely EVIL
 *       Separate them  each statement per line
 */
#define kassert(ex) (ex) ? (void) 0 : panicf("Assert `%s' failed", #ex)

/**
 * @ex      the expression
 * @fmt     panic message format
 *
 * Example: kassertf(sz > 0, "Why size %zd nonpositive?", sz);
 */
#define kassertf(ex, fmt, ...) \
    (ex) ? (void) 0 : panicf("Assert `%s' failed: " fmt, #ex, ##__VA_ARGS__)
#else
#define kassert(ex) (ex) ? (void) 0 : LOG_BUG("Assert `%s' failed", #ex)

#define kassertf(ex, fmt, ...) \
    (ex) ? (void) 0 : LOG_BUG("Assert `%s' failed: " fmt, #ex, ##__VA_ARGS__)
#endif

#define kassert_nonnull(ptr) kassert(((void *) ptr) != NULL)
#define kassert_null(ptr)       kassert(ptr == NULL)

#define __kassert_cmp(v1, v2, f1, f2, op)   \
    kassertf((v1) op (v2), "left: " f1 " right: " f2, (v1), (v2))

#define kassert_eq(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, ==)
#define kassert_ne(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, !=)
#define kassert_le(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, <=)
#define kassert_ge(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, >=)
#define kassert_lt(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, <)
#define kassert_gt(v1, v2, f1, f2)  __kassert_cmp(v1, v2, f1, f2, >)

/**
 * Branch predictions
 * see: linux/include/linux/compiler.h
 */
#define likely(x)       __builtin_expect(!(x), 0)
#define unlikely(x)     __builtin_expect(!(x), 1)

/**
 * Compile-time assurance  see: linux/arch/x86/boot/boot.h
 * Will fail build if condition yield true
 */
#ifdef DEBUG
#define BUILD_BUG_ON(cond)      UNUSED(sizeof(char[-!!(cond)]))
#else
#define BUILD_BUG_ON(cond)      UNUSED(cond)
#endif

/* XXX: ONLY quote those deprecated functions */
#define SUPPRESS_WARN_DEPRECATED_DECL_BEGIN     \
    _Pragma("clang diagnostic push")            \
    _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")

#define SUPPRESS_WARN_DEPRECATED_DECL_END       \
    _Pragma("clang diagnostic pop")

/**
 * Capital Q stands for quick
 * XXX: should only used for `char[]'  NOT `char *'
 */
#define QSTRLEN(s)          (sizeof(s) - 1)

void *libkext_malloc(size_t, int);
void *libkext_realloc(void *, size_t, size_t, int);
void libkext_mfree(void *);
void libkext_massert(void);

int libkext_get_kcb(void);
int libkext_put_kcb(void);
int libkext_read_kcb(void);
void libkext_invalidate_kcb(void);

int libkext_vma_uuid(vm_address_t, uuid_string_t);
void libkext_format_uuid_string(const uuid_t, uuid_string_t);

int libkext_file_read(const char *, unsigned char *, size_t, off_t, size_t *);

/**
 * kern_os_* family provides zero-out memory allocation
 * see: xnu/libkern/c++/OSRuntime.cpp
 */
extern void *kern_os_malloc(size_t);
extern void kern_os_free(void *);
extern void *kern_os_realloc(void *, size_t);

#endif /* LIBKEXT_H */

