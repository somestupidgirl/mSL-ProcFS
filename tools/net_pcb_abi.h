/*
 * net_pcb_abi.h
 *
 * Vendored subset of the XNU "named" PCB-list ABI (the structures returned by
 * the net.inet.{tcp,udp}.pcblist_n sysctls), used by procfsd to build
 * /proc/net/{tcp,tcp6,udp,udp6}. These structures are private to the kernel and
 * are not present in the userspace SDK, so - exactly as procfsd already does for
 * the System V IPC command struct - the definitions are copied verbatim here
 * from the pinned XNU source (lib/xnu):
 *   bsd/netinet/in_pcb.h   struct xinpgen, xinpcb_n, in_addr_4in6 (#pragma pack(4))
 *   bsd/sys/socketvar.h    struct xsocket_n, xsockbuf_n, XSO_* kinds
 *   bsd/netinet/tcp_var.h  struct xtcpcb_n (only the prefix through t_state)
 *
 * The stream produced by get_pcblist_n() is: one struct xinpgen, then per socket
 * a run of length-tagged blocks {xinpcb_n, xsocket_n, xsockbuf_n(rcv),
 * xsockbuf_n(snd), xsockstat_n, [xtcpcb_n]} each ROUNDUP64-aligned, then a
 * trailing xinpgen. The reader walks block-by-block using each block's on-wire
 * length field, so it is resilient to minor size drift; only the leading fields
 * read here need to match the running kernel.
 */
#ifndef PROCFS_NET_PCB_ABI_H
#define PROCFS_NET_PCB_ABI_H

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/un.h>

/* Block kinds (bsd/sys/socketvar.h). */
#define XSO_SOCKET      0x001
#define XSO_RCVBUF      0x002
#define XSO_SNDBUF      0x004
#define XSO_STATS       0x008
#define XSO_INPCB       0x010
#define XSO_TCPCB       0x020
#define XSO_UNPCB       0x200

/* so_state bit (bsd/sys/socketvar.h). */
#define PROCFS_SS_ISCONNECTED   0x0002

/* inp_vflag (bsd/netinet/in_pcb.h). */
#define INP_IPV4        0x1
#define INP_IPV6        0x2

typedef u_int64_t procfs_inp_gen_t;
typedef u_int64_t procfs_so_gen_t;

/* --- #pragma pack(4) group (bsd/netinet/in_pcb.h) --- */
#pragma pack(4)

struct procfs_in_addr_4in6 {
	u_int32_t       ia46_pad32[3];
	struct in_addr  ia46_addr4;
};

struct procfs_xinpgen {
	u_int32_t         xig_len;       /* length of this structure */
	u_int             xig_count;     /* number of PCBs at this time */
	procfs_inp_gen_t  xig_gen;
	procfs_so_gen_t   xig_sogen;
};

struct procfs_xinpcb_n {
	u_int32_t       xi_len;          /* length of this structure */
	u_int32_t       xi_kind;         /* XSO_INPCB */
	u_int64_t       xi_inpp;
	u_short         inp_fport;       /* foreign port (network order) */
	u_short         inp_lport;       /* local port (network order) */
	u_int64_t       inp_ppcb;
	procfs_inp_gen_t inp_gencnt;
	int             inp_flags;
	u_int32_t       inp_flow;
	u_char          inp_vflag;       /* INP_IPV4 / INP_IPV6 */
	u_char          inp_ip_ttl;
	u_char          inp_ip_p;
	union {
		struct procfs_in_addr_4in6 inp46_foreign;
		struct in6_addr            inp6_foreign;
	} inp_dependfaddr;
	union {
		struct procfs_in_addr_4in6 inp46_local;
		struct in6_addr            inp6_local;
	} inp_dependladdr;
	struct { u_char inp4_ip_tos; } inp_depend4;
	struct {
		u_int8_t inp6_hlim;
		int      inp6_cksum;
		u_short  inp6_ifindex;
		short    inp6_hops;
	} inp_depend6;
	u_int32_t       inp_flowhash;
	u_int32_t       inp_flags2;
};

#pragma pack()

/* --- #pragma pack(4) group (bsd/sys/socketvar.h, lines 425-575) --- */
#pragma pack(4)

struct procfs_xsockbuf_n {
	u_int32_t       xsb_len;
	u_int32_t       xsb_kind;        /* XSO_RCVBUF or XSO_SNDBUF */
	u_int32_t       sb_cc;           /* bytes queued */
	u_int32_t       sb_hiwat;
	u_int32_t       sb_mbcnt;
	u_int32_t       sb_mbmax;
	int32_t         sb_lowat;
	short           sb_flags;
	short           sb_timeo;
};

/* xsocket_n through the fields we read (so_uid, so_usecount). */
struct procfs_xsocket_n {
	u_int32_t       xso_len;
	u_int32_t       xso_kind;        /* XSO_SOCKET */
	u_int64_t       xso_so;
	short           so_type;
	u_int32_t       so_options;
	short           so_linger;
	short           so_state;
	u_int64_t       so_pcb;
	int             xso_protocol;
	int             xso_family;
	short           so_qlen;
	short           so_incqlen;
	short           so_qlimit;
	short           so_timeo;
	u_short         so_error;
	pid_t           so_pgid;
	u_int32_t       so_oobmark;
	uid_t           so_uid;
	pid_t           so_last_pid;
	pid_t           so_e_pid;
	procfs_so_gen_t so_gencnt;
	u_int32_t       so_flags;
	u_int32_t       so_flags1;
	int32_t         so_usecount;
	int32_t         so_retaincnt;
	u_int32_t       xso_filter_flags;
};

/* xtcpcb_n prefix through t_state (bsd/netinet/tcp_var.h, also #pragma pack(4);
 * the prefix has no alignment gaps so packed and natural offsets coincide, but
 * matching the kernel's packing keeps it correct if extended).
 * TCPT_NTIMERS_EXT == 4 in the pinned XNU. */
#define PROCFS_TCPT_NTIMERS_EXT 4
struct procfs_xtcpcb_n_pre {
	u_int32_t       xt_len;
	u_int32_t       xt_kind;         /* XSO_TCPCB */
	u_int64_t       t_segq;
	int             t_dupacks;
	int             t_timer[PROCFS_TCPT_NTIMERS_EXT];
	int             t_state;         /* macOS TCPS_* */
};

#pragma pack()

/* --- unix-domain PCB list (bsd/sys/unpcb.h, #pragma pack(4)) --- */
#pragma pack(4)

struct procfs_xunpgen {
	u_int32_t       xug_len;
	u_int           xug_count;
	u_int64_t       xug_gen;
	u_int64_t       xug_sogen;
};

struct procfs_xunpcb_n {
	u_int32_t       xunp_len;        /* length of this structure */
	u_int32_t       xunp_kind;       /* XSO_UNPCB */
	u_int64_t       xunp_unpp;       /* hashed pcb address (Num) */
	u_int64_t       xunp_vnode;
	u_int64_t       xunp_ino;        /* fake inode number */
	u_int64_t       xunp_conn;
	u_int64_t       xunp_refs;
	u_int64_t       xunp_reflink;
	int             xunp_cc;
	int             xunp_mbcnt;
	int             xunp_flags;
	u_int64_t       xunp_gencnt;
	union {
		struct sockaddr_un xuu_addr; /* our bound address */
		char               xu_dummy1[256];
	} xu_au;
	union {
		struct sockaddr_un xuu_caddr;
		char               xu_dummy2[256];
	} xu_cau;
};

#pragma pack()

#endif /* PROCFS_NET_PCB_ABI_H */
