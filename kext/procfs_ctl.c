/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_ctl.c
 *
 * Kernel-control (PF_SYSTEM) bridge to the userspace procfsd daemon. Fields that
 * the kext cannot obtain on arm64 (the full proc_taskinfo, per-thread info -
 * fill_taskprocinfo/task_info and friends are stripped) are fetched on demand
 * from the daemon, which has them trivially via libproc's proc_pidinfo().
 *
 * A node read calls procfs_ctl_request(), which enqueues a request to the
 * connected daemon and sleeps (with a timeout) until the daemon's reply arrives
 * in the ctl_send callback. If no daemon is connected, or it does not answer in
 * time, the caller falls back to whatever the kext can compute itself.
 */
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/sbuf.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <string.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

#define PROCFS_CTL_SLOTS   16
#define PROCFS_CTL_TIMEO_S  2       /* daemon reply timeout (seconds) */

/* One in-flight request. */
struct procfs_ctl_slot {
    boolean_t in_use;
    boolean_t done;
    uint32_t  seq;
    int       error;
    uint32_t  len;
    uint8_t   buf[PROCFS_CTL_MAXPAYLOAD];
};

static kern_ctl_ref            g_ctl_ref;
static u_int32_t               g_ctl_unit;
static boolean_t               g_ctl_connected;
static lck_grp_t              *g_ctl_grp;
static lck_mtx_t              *g_ctl_lock;
static uint32_t                g_ctl_seq;
static struct procfs_ctl_slot  g_ctl_slots[PROCFS_CTL_SLOTS];

static errno_t
procfs_ctl_connect(__unused kern_ctl_ref kctlref, struct sockaddr_ctl *sac, void **unitinfo)
{
    lck_mtx_lock(g_ctl_lock);
    g_ctl_unit      = sac->sc_unit;
    g_ctl_connected = TRUE;
    lck_mtx_unlock(g_ctl_lock);
    *unitinfo = NULL;
    printf("procfs: ctl daemon connected (unit %u)\n", sac->sc_unit);
    return 0;
}

static errno_t
procfs_ctl_disconnect(__unused kern_ctl_ref kctlref, __unused u_int32_t unit,
    __unused void *unitinfo)
{
    lck_mtx_lock(g_ctl_lock);
    g_ctl_connected = FALSE;
    /* Wake any waiters so they fall back instead of blocking for the timeout. */
    for (int i = 0; i < PROCFS_CTL_SLOTS; i++) {
        if (g_ctl_slots[i].in_use && !g_ctl_slots[i].done) {
            g_ctl_slots[i].error = ENOTCONN;
            g_ctl_slots[i].done  = TRUE;
            wakeup(&g_ctl_slots[i]);
        }
    }
    lck_mtx_unlock(g_ctl_lock);
    printf("procfs: ctl daemon disconnected\n");
    return 0;
}

/* Reply from the daemon: [struct procfs_ctl_resp][payload]. */
static errno_t
procfs_ctl_send(__unused kern_ctl_ref kctlref, __unused u_int32_t unit,
    __unused void *unitinfo, mbuf_t m, __unused int flags)
{
    struct procfs_ctl_resp resp;
    size_t total = mbuf_pkthdr_len(m);

    if (total >= sizeof(resp) &&
        mbuf_copydata(m, 0, sizeof(resp), &resp) == 0 &&
        resp.magic == PROCFS_CTL_MAGIC) {
        uint32_t plen = resp.len;
        if (plen > PROCFS_CTL_MAXPAYLOAD) {
            plen = PROCFS_CTL_MAXPAYLOAD;
        }
        lck_mtx_lock(g_ctl_lock);
        for (int i = 0; i < PROCFS_CTL_SLOTS; i++) {
            if (g_ctl_slots[i].in_use && !g_ctl_slots[i].done &&
                g_ctl_slots[i].seq == resp.seq) {
                if (plen > 0 && total >= sizeof(resp) + plen) {
                    mbuf_copydata(m, sizeof(resp), plen, g_ctl_slots[i].buf);
                }
                g_ctl_slots[i].len   = plen;
                g_ctl_slots[i].error = resp.error;
                g_ctl_slots[i].done  = TRUE;
                wakeup(&g_ctl_slots[i]);
                break;
            }
        }
        lck_mtx_unlock(g_ctl_lock);
    }
    mbuf_freem(m);
    return 0;
}

/*
 * Request `type` for `pid` (and `arg`, e.g. a tid; `name`, e.g. a sysctl MIB
 * name, or NULL) from the daemon. On success copies up to `outcap` payload bytes
 * into `out` and sets *outlen. Returns 0, or an errno (ENOTCONN if no daemon,
 * ETIMEDOUT if it didn't answer in time) so the caller can fall back.
 */
static int
procfs_ctl_request_impl(uint32_t type, int pid, uint64_t arg, const char *name,
    void *out, uint32_t outcap, uint32_t *outlen)
{
    if (!g_ctl_connected || g_ctl_ref == NULL) {
        return ENOTCONN;
    }

    lck_mtx_lock(g_ctl_lock);
    int slot = -1;
    for (int i = 0; i < PROCFS_CTL_SLOTS; i++) {
        if (!g_ctl_slots[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        lck_mtx_unlock(g_ctl_lock);
        return EBUSY;
    }
    uint32_t seq = ++g_ctl_seq;
    if (seq == 0) {
        seq = ++g_ctl_seq;
    }
    g_ctl_slots[slot].in_use = TRUE;
    g_ctl_slots[slot].done   = FALSE;
    g_ctl_slots[slot].seq    = seq;
    g_ctl_slots[slot].error  = 0;
    g_ctl_slots[slot].len    = 0;
    u_int32_t    unit = g_ctl_unit;
    kern_ctl_ref ref  = g_ctl_ref;
    lck_mtx_unlock(g_ctl_lock);

    struct procfs_ctl_req req = {
        .magic = PROCFS_CTL_MAGIC,
        .seq   = seq,
        .type  = type,
        .pid   = pid,
        .arg   = arg,
    };
    if (name != NULL) {
        strlcpy(req.name, name, sizeof(req.name));
    }

    int error;
    errno_t e = ctl_enqueuedata(ref, unit, &req, sizeof(req), 0);
    if (e != 0) {
        error = e;
    } else {
        struct timespec ts = { .tv_sec = PROCFS_CTL_TIMEO_S, .tv_nsec = 0 };
        lck_mtx_lock(g_ctl_lock);
        while (!g_ctl_slots[slot].done) {
            int r = msleep(&g_ctl_slots[slot], g_ctl_lock, PCATCH, "procfsctl", &ts);
            if (r != 0) {
                break;      /* timeout (EWOULDBLOCK) or signal */
            }
        }
        if (g_ctl_slots[slot].done) {
            error = g_ctl_slots[slot].error;
            if (error == 0) {
                uint32_t n = g_ctl_slots[slot].len;
                if (n > outcap) {
                    n = outcap;
                }
                memcpy(out, g_ctl_slots[slot].buf, n);
                if (outlen != NULL) {
                    *outlen = n;
                }
            }
        } else {
            error = ETIMEDOUT;
        }
        lck_mtx_unlock(g_ctl_lock);
    }

    lck_mtx_lock(g_ctl_lock);
    g_ctl_slots[slot].in_use = FALSE;
    lck_mtx_unlock(g_ctl_lock);
    return error;
}

int
procfs_ctl_request(uint32_t type, int pid, uint64_t arg, void *out,
    uint32_t outcap, uint32_t *outlen)
{
    return procfs_ctl_request_impl(type, pid, arg, NULL, out, outcap, outlen);
}

int
procfs_ctl_request_named(uint32_t type, int pid, uint64_t arg, const char *name,
    void *out, uint32_t outcap, uint32_t *outlen)
{
    return procfs_ctl_request_impl(type, pid, arg, name, out, outcap, outlen);
}

/*
 * Assemble a large text reply from the daemon into `sb` by requesting it in
 * PROCFS_CTL_MAXPAYLOAD-sized chunks (arg = byte offset), stopping at the first
 * short (or empty) chunk. Used for listings that exceed one payload (e.g. the
 * loaded-kext list). Returns 0 on success, or an errno (ENOTCONN with no daemon)
 * so the caller can fall back.
 */
int
procfs_ctl_request_blob(uint32_t type, struct sbuf *sb)
{
    uint8_t   chunk[PROCFS_CTL_MAXPAYLOAD];
    uint64_t  offset = 0;

    for (;;) {
        uint32_t got = 0;
        int e = procfs_ctl_request(type, 0, offset, chunk, sizeof(chunk), &got);
        if (e != 0) {
            return e;
        }
        if (got > 0) {
            sbuf_bcat(sb, chunk, got);
        }
        offset += got;
        if (got < PROCFS_CTL_MAXPAYLOAD) {
            break;      /* short chunk: end of the listing */
        }
    }
    return 0;
}

kern_return_t
procfs_ctl_register(void)
{
    g_ctl_grp = lck_grp_alloc_init("procfs.ctl", LCK_GRP_ATTR_NULL);
    if (g_ctl_grp == NULL) {
        return KERN_FAILURE;
    }
    g_ctl_lock = lck_mtx_alloc_init(g_ctl_grp, LCK_ATTR_NULL);
    if (g_ctl_lock == NULL) {
        lck_grp_free(g_ctl_grp);
        g_ctl_grp = NULL;
        return KERN_FAILURE;
    }

    struct kern_ctl_reg reg;
    bzero(&reg, sizeof(reg));
    strlcpy(reg.ctl_name, PROCFS_CTL_NAME, sizeof(reg.ctl_name));
    reg.ctl_flags      = CTL_FLAG_PRIVILEGED;   /* only root may connect */
    reg.ctl_sendsize   = 8192;
    reg.ctl_recvsize   = 8192;
    reg.ctl_connect    = procfs_ctl_connect;
    reg.ctl_disconnect = procfs_ctl_disconnect;
    reg.ctl_send       = procfs_ctl_send;

    errno_t e = ctl_register(&reg, &g_ctl_ref);
    if (e != 0) {
        printf("procfs: ctl_register failed (%d)\n", e);
        lck_mtx_free(g_ctl_lock, g_ctl_grp);
        lck_grp_free(g_ctl_grp);
        g_ctl_lock = NULL;
        g_ctl_grp  = NULL;
        return KERN_FAILURE;
    }
    printf("procfs: kernel control '%s' registered\n", PROCFS_CTL_NAME);
    return KERN_SUCCESS;
}

void
procfs_ctl_deregister(void)
{
    if (g_ctl_ref != NULL) {
        ctl_deregister(g_ctl_ref);
        g_ctl_ref = NULL;
    }
    g_ctl_connected = FALSE;
    if (g_ctl_lock != NULL) {
        lck_mtx_free(g_ctl_lock, g_ctl_grp);
        g_ctl_lock = NULL;
    }
    if (g_ctl_grp != NULL) {
        lck_grp_free(g_ctl_grp);
        g_ctl_grp = NULL;
    }
}
