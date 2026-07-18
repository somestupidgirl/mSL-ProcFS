/* Compare /proc/<pid>/taskinfo (the kext) to proc_pidinfo() ground truth.
 *   cc -o /tmp/tti tests/test_taskinfo.c ; /tmp/tti $$
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <libproc.h>
#include <sys/proc_info.h>

int main(int argc, char **argv) {
    pid_t pid = (argc > 1) ? atoi(argv[1]) : getpid();
    char path[256];
    snprintf(path, sizeof(path), "%s/proc/%d/taskinfo", getenv("HOME"), pid);

    struct proc_taskinfo kext = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    ssize_t n = pread(fd, &kext, sizeof(kext), 0);
    close(fd);
    if (n < (ssize_t)sizeof(kext)) { fprintf(stderr, "short read %zd\n", n); return 1; }

    struct proc_taskinfo truth = {0};
    proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &truth, sizeof(truth));

    printf("field              kext            proc_pidinfo\n");
    #define ROW(f) printf("%-18s %-15llu %llu\n", #f, (unsigned long long)kext.f, (unsigned long long)truth.f)
    ROW(pti_virtual_size);
    ROW(pti_resident_size);
    ROW(pti_threadnum);
    ROW(pti_policy);
    ROW(pti_total_user);
    ROW(pti_total_system);
    ROW(pti_faults);
    ROW(pti_syscalls_unix);
    ROW(pti_syscalls_mach);
    ROW(pti_csw);
    ROW(pti_priority);
    return 0;
}
