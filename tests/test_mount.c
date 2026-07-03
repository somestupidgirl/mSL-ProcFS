#include <sys/mount.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

typedef struct pfsmount_args {
    int mnt_options;
} pfsmount_args_t;

int main(void) {
    pfsmount_args_t args = { .mnt_options = 0 };
    mkdir("/Users/sunneva/proc", 0755);
    int ret = mount("procfs", "/Users/sunneva/proc", 0, &args);
    printf("ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));
    return ret;
}
