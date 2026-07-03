/* TEMP diagnostic: reads /proc/<pid>/taskdump + proc_pidinfo ground truth and
 * locates the offset of each proc_taskinfo field within the raw struct task by
 * value-matching (and pair-summing the recount P/E tracks for CPU time).
 *
 *   cc -o /tmp/fto test/find_task_offsets.c
 *   /tmp/fto $$            # your shell
 *   /tmp/fto <busy-pid>    # a process that has burned CPU
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <libproc.h>
#include <sys/proc_info.h>

static uint8_t raw[8192]; static size_t rawlen;
static uint64_t task_ptr, proc_ptr;

static void find_u32(const char *name, uint32_t v) {
    printf("  %-22s = %-12u u32@:", name, v);
    if (v == 0) { printf(" (skip 0)\n"); return; }
    for (size_t o = 0; o + 4 <= rawlen; o++) if (*(uint32_t*)(raw+o)==v) printf(" 0x%zx", o);
    printf("\n");
}
static void find_u16(const char *name, uint16_t v) {
    printf("  %-22s = %-12d i16@:", name, (int16_t)v);
    for (size_t o = 0; o + 2 <= rawlen; o++) if (*(uint16_t*)(raw+o)==v) printf(" 0x%zx", o);
    printf("\n");
}
static void find_u64(const char *name, uint64_t v) {
    printf("  %-22s = %-20llu u64@:", name, (unsigned long long)v);
    if (v == 0) { printf(" (skip 0)\n"); return; }
    for (size_t o = 0; o + 8 <= rawlen; o++) if (*(uint64_t*)(raw+o)==v) printf(" 0x%zx", o);
    printf("\n");
}
/* recount sum: find pairs of u64 (P/E tracks) summing to target */
static void find_pair(const char *name, uint64_t target) {
    if (target == 0) { printf("  %-22s = 0 (skip)\n", name); return; }
    printf("  %-22s = %llu  pair@(a,b):", name, (unsigned long long)target);
    int hits=0;
    for (size_t i=0;i+8<=rawlen && hits<8;i+=8){uint64_t a=*(uint64_t*)(raw+i); if(a==0||a>target)continue;
      for(size_t j=i+8;j+8<=rawlen;j+=8){if(a+*(uint64_t*)(raw+j)==target){printf(" (0x%zx,0x%zx)",i,j);hits++;break;}}}
    printf("%s\n", hits?"":" none");
}

int main(int argc, char **argv) {
    pid_t pid = (argc>1)?atoi(argv[1]):getpid();
    char path[256]; snprintf(path,sizeof(path),"%s/proc/%d/taskdump",getenv("HOME"),pid);
    int fd=open(path,O_RDONLY); if(fd<0){perror("open");return 1;}
    uint8_t hdr[32]; if(pread(fd,hdr,32,0)!=32){perror("hdr");return 1;}
    uint64_t magic=*(uint64_t*)hdr; task_ptr=*(uint64_t*)(hdr+8); proc_ptr=*(uint64_t*)(hdr+16);
    uint64_t n=*(uint64_t*)(hdr+24);
    if(magic!=0x504d554b53415401ULL){fprintf(stderr,"bad magic 0x%llx\n",(unsigned long long)magic);return 1;}
    rawlen = n>sizeof(raw)?sizeof(raw):n;
    pread(fd, raw, rawlen, 32); close(fd);

    struct proc_taskinfo ti={0};
    proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti));

    printf("task=0x%llx proc=0x%llx rawlen=%zu pid=%d\n",
           (unsigned long long)task_ptr,(unsigned long long)proc_ptr,rawlen,pid);
    printf("--- clean anchors (should be unique-ish) ---\n");
    find_u32("thread_count", (uint32_t)ti.pti_threadnum);
    find_u16("priority", (uint16_t)(int16_t)ti.pti_priority);
    printf("--- plain counters (may have per-thread addend -> approx) ---\n");
    find_u32("syscalls_mach", (uint32_t)ti.pti_syscalls_mach);
    find_u32("syscalls_unix", (uint32_t)ti.pti_syscalls_unix);
    find_u32("csw", (uint32_t)ti.pti_csw);
    printf("--- recount times (sum of P/E tracks) ---\n");
    find_pair("total_user", ti.pti_total_user);
    find_pair("total_system", ti.pti_total_system);
    find_u64("total_user(single)", ti.pti_total_user);
    find_u64("total_system(single)", ti.pti_total_system);
    /* Structural map: live counters can't be value-matched, but the counter_t
     * cluster is 5 consecutive kernel pointers; the syscall and ctx-switch
     * counters follow it. */
    #define KPTR(x) ((x) >= 0xfffffe0000000000ULL)
    printf("--- structural dump 0x78..0x200 (K=kernel ptr) ---\n");
    for (size_t o = 0x78; o + 8 <= rawlen && o < 0x200; o += 8) {
        uint64_t v = *(uint64_t*)(raw+o);
        printf("  0x%03zx: %016llx %s  [u32 %u, %u]\n", o, (unsigned long long)v,
               KPTR(v) ? "K" : " ", *(uint32_t*)(raw+o), *(uint32_t*)(raw+o+4));
    }
    /* find first run of >=5 consecutive kernel pointers after priority -> counter cluster */
    printf("--- counter_t cluster candidates (run of >=5 kptrs) ---\n");
    for (size_t o = 0x90; o + 40 <= rawlen; o += 8) {
        int run = 0;
        while (o + (size_t)(run+1)*8 <= rawlen && KPTR(*(uint64_t*)(raw+o+(size_t)run*8))) run++;
        if (run >= 5) {
            size_t after = o + 40;  /* faults,pageins,cow,msgsent,msgrcv = 5 ptrs */
            printf("  cluster@0x%zx (run=%d); then decompressions@0x%zx sysc_mach@0x%zx "
                   "sysc_unix@0x%zx c_switch@0x%zx\n  -> values: decomp=%u sysc_mach=%u "
                   "sysc_unix=%u c_switch=%u (proc_pidinfo sysc_mach=%d sysc_unix=%d csw=%d)\n",
                   o, run, after, after+4, after+8, after+12,
                   *(uint32_t*)(raw+after), *(uint32_t*)(raw+after+4),
                   *(uint32_t*)(raw+after+8), *(uint32_t*)(raw+after+12),
                   ti.pti_syscalls_mach, ti.pti_syscalls_unix, ti.pti_csw);
            o += (size_t)run*8;
        }
    }
    printf("(proc_pidinfo: vsize=%llu rss=%llu tuser=%llu tsys=%llu thr=%d pri=%d faults=%d)\n",
        (unsigned long long)ti.pti_virtual_size,(unsigned long long)ti.pti_resident_size,
        (unsigned long long)ti.pti_total_user,(unsigned long long)ti.pti_total_system,
        ti.pti_threadnum, ti.pti_priority, ti.pti_faults);
    return 0;
}
