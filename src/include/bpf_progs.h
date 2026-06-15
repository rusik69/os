#ifndef BPF_PROGS_H
#define BPF_PROGS_H

#include "types.h"

/* eBPF program types */
#define BPF_PROG_TYPE_UNSPEC       0
#define BPF_PROG_TYPE_KPROBE       1
#define BPF_PROG_TYPE_TRACEPOINT   2
#define BPF_PROG_TYPE_XDP          3
#define BPF_PROG_TYPE_SOCKET_FILTER 4

/* Load an eBPF program after verification.
 * Returns fd (>= 1) on success, negative errno on failure. */
int bpf_prog_load(int prog_type, const struct bpf_insn *insns, int insn_cnt,
                  const char *name, char *log, int log_size);

/* Attach a loaded program to a kprobe by symbol name. */
int bpf_prog_attach_kprobe(int prog_fd, const char *symbol);

/* Attach a loaded program to a tracepoint. */
int bpf_prog_attach_tracepoint(int prog_fd, const char *tracepoint);

/* Attach a loaded program to an XDP hook on a network interface. */
int bpf_prog_attach_xdp(int prog_fd, const char *ifname);

/* Detach a program (stop running it at its hook). */
int bpf_prog_detach(int prog_fd);

/* Unload a program (must be detached first). */
int bpf_prog_unload(int prog_fd);

/* Initialize the BPF programs subsystem. */
void bpf_progs_init(void);

#endif /* BPF_PROGS_H */
