#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#ifndef PROC_PATH
#define PROC_PATH "/Users/sunneva/proc"
#endif

int main(void) {
    DIR *d = opendir(PROC_PATH);
    if (!d) {
        printf("opendir(%s) failed: %d (%s)\n", PROC_PATH, errno, strerror(errno));
        return 1;
    }
    printf("opendir ok\n");

    struct dirent *de;
    int count = 0;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        printf("  [%3d] ino=%llu type=%d name=%s\n",
               count++, (unsigned long long)de->d_ino, de->d_type, de->d_name);
        errno = 0;
    }
    if (errno) {
        printf("readdir error: %d (%s)\n", errno, strerror(errno));
    } else {
        printf("readdir done, %d entries\n", count);
    }
    closedir(d);
    return 0;
}
