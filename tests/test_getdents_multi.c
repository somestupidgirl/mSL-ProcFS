#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/dirent.h>
#include <sys/syscall.h>

#ifndef PROC_PATH
#define PROC_PATH "/Users/sunneva/proc"
#endif

/*
 * Like test_getdents but with a small buffer (mimics opendir's f_iosize-sized
 * reads) so the directory requires MULTIPLE getdirentries64 calls. Logs each
 * call's offset, byte count, and entry count to pinpoint where resumption fails.
 */
int main(int argc, char **argv) {
    size_t bufsize = (argc > 1) ? (size_t)atoi(argv[1]) : 4096;
    const char *path = (argc > 2) ? argv[2] : PROC_PATH;

    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { printf("open %s failed: %s\n", path, strerror(errno)); return 1; }

    char buf[131072];
    if (bufsize > sizeof(buf)) bufsize = sizeof(buf);
    long base = 0;
    int call = 0, total = 0;

    for (;;) {
        off_t off_before = lseek(fd, 0, SEEK_CUR);
        ssize_t n = syscall(SYS_getdirentries64, fd, buf, bufsize, &base);
        off_t off_after = lseek(fd, 0, SEEK_CUR);
        if (n < 0) {
            printf("call %d: offset_before=%lld ERROR %d (%s)\n",
                   call, (long long)off_before, errno, strerror(errno));
            close(fd); return 1;
        }
        int this_entries = 0;
        char *p = buf, *end = buf + n;
        while (p < end) {
            struct dirent *de = (struct dirent *)p;
            if (de->d_reclen == 0) break;
            this_entries++; total++;
            p += de->d_reclen;
        }
        printf("call %d: off_before=%lld off_after=%lld bytes=%zd entries=%d (total=%d)\n",
               call, (long long)off_before, (long long)off_after, n, this_entries, total);
        if (n == 0) break;
        call++;
        if (call > 50) { printf("too many calls, aborting\n"); break; }
    }
    printf("done, %d entries over %d calls\n", total, call);
    close(fd);
    return 0;
}
