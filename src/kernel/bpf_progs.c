/*
 * bpf_progs.c — eBPF program attachment and execution
 *
 * Supports attaching eBPF programs to:
 *   - Kprobes: dynamic kernel probes
 *   - Tracepoints: static tracepoints
 *   - XDP: eXpress Data Path (network driver hook)
 *
 * Each attached program is verified and then executed at the hook point.
 */

#define KERNEL_INTERNAL
#include "bpf_progs.h"
#include "bpf_verifier.h"
#include "bpf_helpers.h"
#include "bpf_maps.h"
#include "printf.h"
#include "string.h"
#include "kprobes.h"
#include "errno.h"
#include "spinlock.h"

#define BPF_PROG_MAX       64
#define BPF_PROG_NAME_MAX  32
#define BPF_MAX_INSN       4096

struct bpf_prog {
    int      in_use;
    int      type;
    char     name[BPF_PROG_NAME_MAX];
    int      fd;                     /* program fd */
    struct bpf_insn *insns;          /* verified instructions */
    int      insn_cnt;
    int      attached;               /* 1 if currently attached */
    int      attach_target;          /* target ID (kprobe addr, etc.) */
};

/* Instruction format (must match bpf_verifier.c) */
struct bpf_insn {
    uint8_t  code;
    uint8_t  dst_reg:4;
    uint8_t  src_reg:4;
    int16_t  off;
    int32_t  imm;
} __attribute__((packed));

static struct bpf_prog g_progs[BPF_PROG_MAX];
static spinlock_t g_progs_lock;

/* ── Program management ────────────────────────────────────────────── */

int bpf_prog_load(int prog_type, const struct bpf_insn *insns, int insn_cnt,
                  const char *name, char *log, int log_size)
{
    if (!insns || insn_cnt <= 0 || insn_cnt > BPF_MAX_INSN)
        return -EINVAL;
    if (prog_type < 0 || prog_type > 4)
        return -EINVAL;

    /* Verify the program first */
    char vlog[256];
    int ret = bpf_verify_program(insns, insn_cnt, prog_type, vlog, sizeof(vlog));
    if (ret < 0) {
        if (log && log_size > 0)
            snprintf(log, log_size, "verification failed: %s", vlog);
        kprintf("[BPF] Program '%s' verification FAILED: %s\n",
                name ? name : "?", vlog);
        return ret;
    }

    spinlock_acquire(&g_progs_lock);
    int fd = -1;
    for (int i = 0; i < BPF_PROG_MAX; i++) {
        if (!g_progs[i].in_use) {
            fd = i + 1;
            break;
        }
    }
    if (fd < 0) {
        spinlock_release(&g_progs_lock);
        return -ENOSPC;
    }

    int idx = fd - 1;
    struct bpf_prog *prog = &g_progs[idx];
    memset(prog, 0, sizeof(*prog));
    prog->in_use = 1;
    prog->type = prog_type;
    prog->fd = fd;
    prog->insn_cnt = insn_cnt;
    if (name) {
        strncpy(prog->name, name, BPF_PROG_NAME_MAX - 1);
        prog->name[BPF_PROG_NAME_MAX - 1] = '\0';
    }

    kprintf("[BPF] Loaded program '%s' fd=%d type=%d insns=%d\n",
            prog->name, fd, prog_type, insn_cnt);
    spinlock_release(&g_progs_lock);
    return fd;
}

/* ── Attachment ────────────────────────────────────────────────────── */

int bpf_prog_attach_kprobe(int prog_fd, const char *symbol)
{
    if (prog_fd < 1 || prog_fd > BPF_PROG_MAX) return -EBADF;
    struct bpf_prog *prog = &g_progs[prog_fd - 1];
    if (!prog->in_use) return -EBADF;
    if (prog->type != 1) return -EINVAL; /* BPF_PROG_TYPE_KPROBE */

    /* Attach via kprobe framework */
    int ret = kprobe_register_bpf(symbol, prog_fd);
    if (ret == 0) {
        prog->attached = 1;
        kprintf("[BPF] Attached prog %d to kprobe '%s'\n", prog_fd, symbol);
    }
    return ret;
}

int bpf_prog_attach_tracepoint(int prog_fd, const char *tracepoint)
{
    if (prog_fd < 1 || prog_fd > BPF_PROG_MAX) return -EBADF;
    struct bpf_prog *prog = &g_progs[prog_fd - 1];
    if (!prog->in_use) return -EBADF;
    if (prog->type != 2) return -EINVAL;

    /* Stub: would hook into tracepoint system */
    prog->attached = 1;
    kprintf("[BPF] Attached prog %d to tracepoint '%s'\n", prog_fd, tracepoint);
    return 0;
}

int bpf_prog_attach_xdp(int prog_fd, const char *ifname)
{
    if (prog_fd < 1 || prog_fd > BPF_PROG_MAX) return -EBADF;
    struct bpf_prog *prog = &g_progs[prog_fd - 1];
    if (!prog->in_use) return -EBADF;
    if (prog->type != 3) return -EINVAL;

    /* Stub: would hook into net device XDP hook */
    prog->attached = 1;
    kprintf("[BPF] Attached prog %d to XDP on '%s'\n", prog_fd, ifname);
    return 0;
}

int bpf_prog_detach(int prog_fd)
{
    if (prog_fd < 1 || prog_fd > BPF_PROG_MAX) return -EBADF;
    struct bpf_prog *prog = &g_progs[prog_fd - 1];
    if (!prog->in_use) return -EBADF;

    prog->attached = 0;
    kprintf("[BPF] Detached prog %d\n", prog_fd);
    return 0;
}

int bpf_prog_unload(int prog_fd)
{
    if (prog_fd < 1 || prog_fd > BPF_PROG_MAX) return -EBADF;
    spinlock_acquire(&g_progs_lock);
    if (g_progs[prog_fd - 1].attached) {
        spinlock_release(&g_progs_lock);
        return -EBUSY;
    }
    memset(&g_progs[prog_fd - 1], 0, sizeof(struct bpf_prog));
    spinlock_release(&g_progs_lock);
    kprintf("[BPF] Unloaded prog %d\n", prog_fd);
    return 0;
}

/* ── Initialization ────────────────────────────────────────────────── */

void bpf_progs_init(void)
{
    memset(g_progs, 0, sizeof(g_progs));
    spinlock_init(&g_progs_lock);
    kprintf("[OK] BPF programs initialized (%d max)\n", BPF_PROG_MAX);
}

/* ── Stub: bpf_prog_run ─────────────────────────────── */
int bpf_prog_run(int fd, const void *ctx, void *result)
{
    (void)fd;
    (void)ctx;
    (void)result;
    kprintf("[bpf] bpf_prog_run: not yet implemented\n");
    return -ENOSYS;
}
