#ifndef BPF_HELPERS_H
#define BPF_HELPERS_H

#include "types.h"

/* BPF helper function prototypes */
uint64_t bpf_get_current_pid_tgid(void);
uint64_t bpf_get_current_uid_gid(void);
uint64_t bpf_get_current_comm(char *buf, uint32_t size);
uint64_t __printf(1, 3) bpf_trace_printk(const char *fmt, uint32_t fmt_size, ...);
uint64_t bpf_ktime_get_ns(void);
uint32_t bpf_get_smp_processor_id(void);

/* Main dispatcher — called by BPF program when it does CALL <helper_id> */
uint64_t bpf_dispatch_helper(int helper_id, uint64_t r1, uint64_t r2,
                              uint64_t r3, uint64_t r4, uint64_t r5);

/* Context for bpf_perf_event_output helper */
struct bpf_perf_event_output_ctx {
    uint64_t data;
    uint32_t size;
    uint32_t flags;
};

/* Initialize BPF helpers subsystem */
void bpf_helpers_init(void);

#endif /* BPF_HELPERS_H */
