#include <stdio.h>
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
 * Call getdirentries64 (SYS_getdirentries64 = 344) directly, bypassing
 * opendir/readdir and their use of getdirentriesattr (VNOP_READDIRATTR).
 * This exercises VNOP_READDIR directly.
 */
int main(void) {
    int fd = open(PROC_PATH, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        printf("open failed: %d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("open ok\n");

    char buf[8192];
    long base = 0;
    int count = 0;

    for (;;) {
        ssize_t n = syscall(SYS_getdirentries64, fd, buf, sizeof(buf), &base);
        if (n < 0) {
            printf("getdirentries64 error: %d (%s)\n", errno, strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) break; /* EOF */

        char *p = buf;
        char *end = buf + n;
        while (p < end) {
            struct dirent *de = (struct dirent *)p;
            if (de->d_reclen == 0) break;
            printf("  [%3d] ino=%llu type=%d reclen=%d name=%s\n",
                   count++, (unsigned long long)de->d_ino,
                   de->d_type, de->d_reclen, de->d_name);
            p += de->d_reclen;
        }
    }

    printf("done, %d entries\n", count);
    close(fd);
    return 0;
}
