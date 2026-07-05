/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * procfs_ksyms.c
 *
 * Userspace symbol-staging helper for the procfs kext.
 *
 * On Apple Silicon the running kernel jettisons its __LINKEDIT after boot, and
 * the only on-disk symbol table for the running build lives in the booted
 * kernel collection, which is IMG4-wrapped and LZFSE-compressed. A kext cannot
 * decompress that (lzfse is not a linkable KPI), so this tool does it in
 * userspace - where libcompression is available - and writes the private symbol
 * addresses the kext needs to a small staged file. The kext reads that file,
 * applies the running KASLR slide (derived from `version`), validates it
 * against `kernel_pmap`, and uses the resolved addresses.
 *
 * Re-run after every kernel change (OS update); the kext's kernel_pmap check
 * safely ignores a stale file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <compression.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#ifndef STAGED_PATH
#define STAGED_PATH   "/var/db/procfs.ksyms"
#endif
#define KC_GLOB       "/System/Volumes/Preboot/*/boot/*/System/Library/Caches/com.apple.kernelcaches/kernelcache"

/* Private kernel symbols the kext wants. version + kernel_pmap are the slide
 * anchor and validation anchor; the rest unlock walled features. */
static const char *const WANTED[] = {
    "_version",
    "_kernel_pmap",
    "_proc_task",           /* task for a proc (mem/cmdline/taskinfo fallback) */
};
#define NWANTED (sizeof(WANTED) / sizeof(WANTED[0]))

/* Extract the "xnu-<version>" token from a Darwin version string. */
static int
xnu_token(const char *s, char *out, size_t outsz)
{
    const char *p = strstr(s, "xnu-");
    if (p == NULL) {
        return -1;
    }
    size_t i = 0;
    while (p[i] != '\0' && p[i] != ' ' && i < outsz - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

int
main(void)
{
    /* Running kernel build token, to confirm we stage the right kernelcache. */
    char uname_v[512] = { 0 };
    size_t un = sizeof(uname_v);
    if (sysctlbyname("kern.version", uname_v, &un, NULL, 0) != 0) {
        fprintf(stderr, "procfs_ksyms: kern.version sysctl failed\n");
        return 1;
    }
    char want_xnu[64];
    if (xnu_token(uname_v, want_xnu, sizeof(want_xnu)) != 0) {
        fprintf(stderr, "procfs_ksyms: no xnu token in kern.version\n");
        return 1;
    }

    glob_t g;
    if (glob(KC_GLOB, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "procfs_ksyms: no booted kernelcache found\n");
        return 1;
    }

    uint8_t *out = NULL;
    const char *used = NULL;
    char kc_ver[256] = { 0 };

    for (size_t c = 0; c < g.gl_pathc && used == NULL; c++) {
        int fd = open(g.gl_pathv[c], O_RDONLY);
        if (fd < 0) {
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); continue; }
        uint8_t *raw = malloc(st.st_size);
        if (raw == NULL) { close(fd); continue; }
        ssize_t rd = read(fd, raw, st.st_size);
        close(fd);
        if (rd != st.st_size) { free(raw); continue; }

        /* IMG4 payload is LZFSE ("bvx2"); decode from there. */
        uint8_t *bvx = memmem(raw, st.st_size, "bvx2", 4);
        if (bvx == NULL) { free(raw); continue; }

        size_t cap = 512ull * 1024 * 1024;
        uint8_t *dec = malloc(cap);
        if (dec == NULL) { free(raw); continue; }
        size_t n = compression_decode_buffer(dec, cap, bvx,
            (size_t)((raw + st.st_size) - bvx), NULL, COMPRESSION_LZFSE);
        free(raw);
        if (n == 0) { free(dec); continue; }

        /* Confirm this kernelcache's kernel build matches the running one. */
        uint8_t *vs = memmem(dec, n, "Darwin Kernel Version", 21);
        char this_xnu[64] = { 0 };
        if (vs != NULL) {
            size_t i = 0;
            while (vs[i] != '\0' && i < sizeof(kc_ver) - 1) { kc_ver[i] = vs[i]; i++; }
            kc_ver[i] = '\0';
        }
        if (vs == NULL || xnu_token(kc_ver, this_xnu, sizeof(this_xnu)) != 0 ||
            strcmp(this_xnu, want_xnu) != 0) {
            free(dec);
            continue;       /* wrong build - keep looking */
        }

        out = dec;
        used = g.gl_pathv[c];
    }

    if (out == NULL) {
        fprintf(stderr, "procfs_ksyms: no matching kernelcache (running %s)\n", want_xnu);
        globfree(&g);
        return 1;
    }

    /* Parse fileset -> com.apple.kernel slice -> LC_SYMTAB. */
    struct mach_header_64 *mh = (struct mach_header_64 *)out;
    uint64_t kfo = 0;
    size_t off = sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct load_command *lc = (struct load_command *)(out + off);
        if (lc->cmd == LC_FILESET_ENTRY) {
            struct fileset_entry_command *fe = (struct fileset_entry_command *)lc;
            const char *nm = (const char *)lc + fe->entry_id.offset;
            if (strcmp(nm, "com.apple.kernel") == 0) { kfo = fe->fileoff; }
        }
        off += lc->cmdsize;
    }
    if (kfo == 0) {
        fprintf(stderr, "procfs_ksyms: no com.apple.kernel slice\n");
        return 1;
    }

    struct mach_header_64 *km = (struct mach_header_64 *)(out + kfo);
    struct symtab_command *sc = NULL;
    off = kfo + sizeof(*km);
    for (uint32_t i = 0; i < km->ncmds; i++) {
        struct load_command *lc = (struct load_command *)(out + off);
        if (lc->cmd == LC_SYMTAB) { sc = (struct symtab_command *)lc; break; }
        off += lc->cmdsize;
    }
    if (sc == NULL) {
        fprintf(stderr, "procfs_ksyms: no LC_SYMTAB\n");
        return 1;
    }

    struct nlist_64 *syms = (struct nlist_64 *)(out + sc->symoff);
    const char *strs = (const char *)(out + sc->stroff);
    uint64_t addr[NWANTED] = { 0 };
    for (uint32_t i = 0; i < sc->nsyms; i++) {
        uint32_t sx = syms[i].n_un.n_strx;
        if (sx == 0 || sx >= sc->strsize || syms[i].n_value == 0) {
            continue;
        }
        const char *nm = strs + sx;
        for (size_t w = 0; w < NWANTED; w++) {
            if (addr[w] == 0 && strcmp(nm, WANTED[w]) == 0) {
                addr[w] = syms[i].n_value;
            }
        }
    }

    if (addr[0] == 0 || addr[1] == 0) {   /* version + kernel_pmap are required */
        fprintf(stderr, "procfs_ksyms: missing required anchor symbols\n");
        return 1;
    }

    /* Write the staged file atomically. */
    char tmp[] = STAGED_PATH ".tmpXXXXXX";
    int tfd = mkstemp(tmp);
    if (tfd < 0) {
        fprintf(stderr, "procfs_ksyms: mkstemp %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    FILE *f = fdopen(tfd, "w");
    fprintf(f, "# procfs kernel symbols (auto-generated; do not edit)\n");
    fprintf(f, "# build %s\n", want_xnu);
    int staged = 0;
    for (size_t w = 0; w < NWANTED; w++) {
        if (addr[w] != 0) {
            fprintf(f, "%s 0x%llx\n", WANTED[w], (unsigned long long)addr[w]);
            staged++;
        }
    }
    fclose(f);
    chmod(tmp, 0644);
    if (rename(tmp, STAGED_PATH) != 0) {
        fprintf(stderr, "procfs_ksyms: rename to %s: %s\n", STAGED_PATH, strerror(errno));
        unlink(tmp);
        return 1;
    }

    printf("procfs_ksyms: staged %d/%zu symbols from %s to %s\n",
           staged, NWANTED, used, STAGED_PATH);
    return 0;
}
