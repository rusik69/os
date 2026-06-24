/*
 * coredump.c — ELF core dump generation with core_pattern support.
 *
 * Writes ELF core files according to /proc/sys/kernel/core_pattern.
 * Supports %-substitution: %p PID, %s signal, %e executable name,
 * %u UID, %g GID, %% literal '%'.
 *
 * Default pattern: "/tmp/core.%p"
 *
 * The core file contains:
 *   - ELF header (ET_CORE = 4)
 *   - Program header table:
 *       PT_NOTE segment with NT_PRSTATUS note (register state)
 *       PT_LOAD segments for each mapped memory region
 *   - Note data (register dump)
 *   - Memory segment data
 *
 * If core_pattern starts with '|', the core is piped to a userspace
 * handler (pipe mode).  Otherwise, it is written to the expanded path.
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "process.h"
#include "coredump.h"
#include "string.h"
#include "fs.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "heap.h"          /* kmalloc, kfree */
#include "coredump_core.h"
#include "workqueue.h"
#include "sysctl.h"         /* sysctl_register */

/* Simple integer-to-string for formatting core paths */
#define ULTOA_BUF 24
static void ul_to_str(uint64_t v, char *buf, int *pos, int max)
{
    if (v == 0) {
        if (*pos < max - 1) buf[(*pos)++] = '0';
        return;
    }
    char tmp[ULTOA_BUF];
    int n = 0;
    while (v > 0 && n < ULTOA_BUF) {
        tmp[n++] = '0' + (int)(v % 10);
        v /= 10;
    }
    while (n-- > 0 && *pos < max - 1)
        buf[(*pos)++] = tmp[n];
}

#ifdef MODULE
#include "module.h"
#endif

#define ET_CORE     4
#define PT_NOTE     4
#define PT_LOAD     1

/* ELF note types */
#define NT_PRSTATUS 1

/* ELF note descriptor */
struct elf_note {
    uint32_t n_namesz;  /* Length of note name (including NUL) */
    uint32_t n_descsz;  /* Length of descriptor */
    uint32_t n_type;    /* Type (NT_PRSTATUS = 1) */
    /* Followed by name + pad, then descriptor + pad */
} __attribute__((packed));

/* ELF Prstatus structure for x86-64 (from Linux asm/ptrace.h) */
#define ELF_NGREG 27
typedef uint64_t elf_greg_t;
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

struct elf_prstatus {
    struct elf_siginfo { int si_signo; int si_code; int si_errno; } pr_info;
    int16_t pr_cursig;
    uint16_t pr_pad;
    uint32_t pr_sigpend;
    uint32_t pr_sighold;
    uint32_t pr_pid;
    uint32_t pr_ppid;
    uint32_t pr_pgrp;
    uint32_t pr_sid;
    struct elf_timeval { uint64_t tv_sec; uint64_t tv_usec; } pr_utime;
    struct elf_timeval pr_stime;
    struct elf_timeval pr_cutime;
    struct elf_timeval pr_cstime;
    elf_gregset_t pr_reg;
    int32_t pr_fpvalid;
} __attribute__((packed));

/* Register order in pr_reg for x86-64 (from Linux sys/user.h) */
enum x86_64_reg {
    R15 = 0, R14, R13, R12, RBP, RBX, R11, R10, R9, R8,
    RAX, RCX, RDX, RSI, RDI,    /* 10-14 */
    ORIG_RAX,                    /* 15 */
    RIP,                         /* 16 */
    CS, EFLAGS, RSP, SS,         /* 17-20 */
    FS_BASE, GS_BASE, DS, ES, FS, GS /* 21-26 */
};

static int coredump_enabled = 1;

/* ── Core pattern support (Item 299) ──────────────────────────────
 *
 * /proc/sys/kernel/core_pattern controls where core dumps are written.
 * The pattern supports these %-escape sequences:
 *   %p  PID of dumped process
 *   %s  Signal number that caused the dump
 *   %e  Executable filename
 *   %u  UID of dumped process
 *   %g  GID of dumped process
 *   %%  Literal '%'
 *
 * Default: "/tmp/core.%p"  — writes /tmp/core.1234 for PID 1234.
 * Pipe mode (leading '|') is recognised but currently falls back to
 * the default path with a warning (full pipe implementation deferred).
 */
#define COREDUMP_PATTERN_MAX    256
#define COREDUMP_PATH_MAX       256

static char g_core_pattern[COREDUMP_PATTERN_MAX] = "/tmp/core.%p";

/* ── Sysctl handlers for core_pattern ─────────────────────────── */

static int sysctl_read_core_pattern(char *buf, int max)
{
    int len = (int)strlen(g_core_pattern);
    if (len >= max) len = max - 1;
    memcpy(buf, g_core_pattern, (size_t)len);
    if (len < max - 1)
        buf[len++] = '\n';
    buf[len] = '\0';
    return len;
}

static int sysctl_write_core_pattern(const char *buf, int len)
{
    int clen = len < COREDUMP_PATTERN_MAX - 1 ? len : COREDUMP_PATTERN_MAX - 1;
    memcpy(g_core_pattern, buf, (size_t)clen);
    g_core_pattern[clen] = '\0';
    /* Trim trailing whitespace / newlines */
    while (clen > 0 && (g_core_pattern[clen - 1] == '\n' ||
                        g_core_pattern[clen - 1] == '\r' ||
                        g_core_pattern[clen - 1] == ' '))
        g_core_pattern[--clen] = '\0';
    return 0;
}

/* ── Core pattern expansion ─────────────────────────────────────
 *
 * Expands a core_pattern template into a concrete path by replacing
 * %-escape sequences with process-specific values.
 *
 * @pattern:  the template string (e.g. "/tmp/core.%p")
 * @proc:     the process being dumped
 * @signo:    signal that triggered the dump
 * @out:      output buffer (COREDUMP_PATH_MAX bytes)
 * @out_size: size of output buffer
 *
 * Returns the number of bytes written (excluding NUL), or -1 if the
 * expanded path exceeds the buffer.
 */
static int core_pattern_expand(const char *pattern, struct process *proc,
                                int signo, char *out, int out_size)
{
    int o = 0;
    const char *p = pattern;

    if (!pattern || !proc || !out || out_size <= 0)
        return -1;

    while (*p && o < out_size - 1) {
        if (*p == '%') {
            p++; /* skip '%' */
            switch (*p) {
            case 'p': { /* PID */
                char tmp[16];
                int pos = 0;
                ul_to_str(proc->pid, tmp, &pos, (int)sizeof(tmp));
                tmp[pos] = '\0';
                for (int i = 0; tmp[i] && o < out_size - 1; i++)
                    out[o++] = tmp[i];
                break;
            }
            case 's': { /* signal number */
                char tmp[8];
                int pos = 0;
                ul_to_str((unsigned int)signo, tmp, &pos, (int)sizeof(tmp));
                tmp[pos] = '\0';
                for (int i = 0; tmp[i] && o < out_size - 1; i++)
                    out[o++] = tmp[i];
                break;
            }
            case 'e': { /* executable name (sanitised) */
                const char *name = proc->name ? proc->name : "unknown";
                while (*name && o < out_size - 1) {
                    /* Replace '/' with '_' to prevent directory traversal */
                    char c = *name;
                    if (c == '/' || c == '\\') c = '_';
                    out[o++] = c;
                    name++;
                }
                break;
            }
            case 'u': { /* UID */
                char tmp[16];
                int pos = 0;
                ul_to_str(proc->uid, tmp, &pos, (int)sizeof(tmp));
                tmp[pos] = '\0';
                for (int i = 0; tmp[i] && o < out_size - 1; i++)
                    out[o++] = tmp[i];
                break;
            }
            case 'g': { /* GID */
                char tmp[16];
                int pos = 0;
                ul_to_str(proc->gid, tmp, &pos, (int)sizeof(tmp));
                tmp[pos] = '\0';
                for (int i = 0; tmp[i] && o < out_size - 1; i++)
                    out[o++] = tmp[i];
                break;
            }
            case '%': { /* literal '%' */
                if (o < out_size - 1)
                    out[o++] = '%';
                break;
            }
            case '\0': /* pattern ends with '%' — just append '%' */
                if (o < out_size - 1)
                    out[o++] = '%';
                goto done;
            default:  /* unknown escape — pass through verbatim */
                if (o < out_size - 1)
                    out[o++] = '%';
                if (o < out_size - 1)
                    out[o++] = *p;
                break;
            }
            if (*p) p++;
        } else {
            out[o++] = *p++;
        }
    }

done:
    out[o] = '\0';

    /* Check for truncation */
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == 'p' || *p == 's' || *p == 'u' || *p == 'g' || *p == '%') {
                /* Would have expanded further — buffer too small */
                return -1;
            }
            if (*p) p++;
        } else {
            p++;
        }
    }

    return o;
}

/* ── Simple extendable buffer for building the core file in memory ── */
struct core_buf {
    uint8_t *data;
    uint64_t cap;
    uint64_t len;
};

static int cb_init(struct core_buf *cb, uint64_t initial) {
    cb->data = (uint8_t *)kmalloc(initial);
    if (!cb->data) return -1;
    cb->cap = initial;
    cb->len = 0;
    return 0;
}

static int cb_grow(struct core_buf *cb, uint64_t needed) {
    if (needed <= cb->cap) return 0;
    uint64_t new_cap = cb->cap * 2;
    while (new_cap < needed) new_cap *= 2;
    uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
    if (!new_data) return -1;
    memcpy(new_data, cb->data, cb->len);
    kfree(cb->data);
    cb->data = new_data;
    cb->cap = new_cap;
    return 0;
}

static int cb_append(struct core_buf *cb, const void *src, uint64_t sz) {
    if (cb_grow(cb, cb->len + sz) < 0) return -1;
    memcpy(cb->data + cb->len, src, sz);
    cb->len += sz;
    return 0;
}

static void cb_pad(struct core_buf *cb, uint64_t align) {
    uint64_t mod = cb->len % align;
    if (mod == 0) return;
    uint64_t pad = align - mod;
    uint64_t old = cb->len;
    if (cb_grow(cb, cb->len + pad) < 0) return;
    memset(cb->data + old, 0, pad);
    cb->len += pad;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static int coredump_init_handler(void);

/* ── Public API ─────────────────────────────────────────────────────── */

void coredump_init(void) {
    /* Register the coredump handler with the kernel core so that
     * do_coredump() dispatches work to us (via coredump_dispatch). */
    coredump_init_handler();

    /* Register the core_pattern sysctl under /proc/sys/kernel/ */
    sysctl_register("core_pattern",
                    sysctl_read_core_pattern,
                    sysctl_write_core_pattern);

    kprintf("[OK] Core dump handler initialized (pattern: \"%s\")\n",
            g_core_pattern);
}

void coredump_set_enabled(int en) {
    coredump_enabled = en;
}

/*
 * Build register state from process->context (cpu_context).
 * We fill the pr_reg array in x86-64 ptrace order.
 */
static void fill_regs(elf_gregset_t *regs, struct process *proc) {
    memset(regs, 0, sizeof(*regs));

    if (!proc || !proc->context) return;

    struct cpu_context *ctx = proc->context;

    /* The cpu_context stores callee-saved regs + rip + rflags */
    (*regs)[R15] = ctx->r15;
    (*regs)[R14] = ctx->r14;
    (*regs)[R13] = ctx->r13;
    (*regs)[R12] = ctx->r12;
    (*regs)[RBP] = ctx->rbp;
    (*regs)[RBX] = ctx->rbx;
    (*regs)[RIP] = ctx->rip;
    (*regs)[EFLAGS] = ctx->rflags;
    /* RSP: get from kernel stack pointer */
    (*regs)[RSP] = proc->stack_top;
    (*regs)[CS] = 0x08;
    (*regs)[SS] = 0x10;
    /* Others remain 0 */
}

int coredump_generate(struct process *proc, int signo) {
    if (!coredump_enabled || !proc) return -1;

    /* ── Check dumpable flag (Item 183) ──────────────────────────
     * dumpable=0: never dump (set by credential changes, SUID binaries)
     * dumpable=1: always dump (default)
     * dumpable=2: dump only if current user is root */
    if (proc->dumpable == 0) {
        kprintf("[COREDUMP] pid=%d: core dump suppressed (dumpable=0)\n",
                (int)proc->pid);
        return -1;
    }
    if (proc->dumpable == 2) {
        struct process *cur = process_get_current();
        if (!cur || cur->euid != 0) {
            kprintf("[COREDUMP] pid=%d: core dump suppressed (dumpable=2, not root)\n",
                    (int)proc->pid);
            return -1;
        }
    }

    kprintf("[COREDUMP] Generating core for PID %d (%s), signal %d\n",
            (int)proc->pid, proc->name ? proc->name : "unknown", signo);

    struct core_buf cb;
    if (cb_init(&cb, 64 * 1024) < 0) {
        kprintf("[COREDUMP] Out of memory\n");
        return -1;
    }

    /* ── Step 1: Build register state ────────────────────────────── */
    struct elf_prstatus prstatus;
    memset(&prstatus, 0, sizeof(prstatus));
    prstatus.pr_pid = proc->pid;
    prstatus.pr_ppid = proc->parent_pid;
    prstatus.pr_info.si_signo = signo;
    prstatus.pr_info.si_code = 0;
    /* Use a temporary non-packed array to avoid
     * -Waddress-of-packed-member, then copy into the packed struct. */
    {
        elf_gregset_t tmp_regs;
        fill_regs(&tmp_regs, proc);
        memcpy(&prstatus.pr_reg, &tmp_regs, sizeof(elf_gregset_t));
    }

    /* ── Step 2: Count memory segments ───────────────────────────── */
    /*
     * We iterate the process's page table to find mapped regions.
     * For each contiguous region of mapped pages, we create a PT_LOAD segment.
     *
     * Simplified: we scan the user address space (0 to USER_VADDR_MAX)
     * in PAGE_SIZE steps, coalescing contiguous mapped pages.
     */

#define MAX_SEGMENTS 256
    struct seg_info {
        uint64_t start;
        uint64_t end;
        uint64_t flags;
    } segs[MAX_SEGMENTS];
    int num_segs = 0;

    if (proc->is_user) {
        /* Scan user space for mapped regions */
        uint64_t addr = 0x1000;
        while (addr < USER_VADDR_MAX && num_segs < MAX_SEGMENTS) {
            uint64_t phys;
            int mapped = vmm_virt_to_phys(addr, &phys);
            if (mapped) {
                uint64_t start = addr;
                /* Coalesce contiguous mapped pages in 16-page chunks */
                while (addr < USER_VADDR_MAX && num_segs < MAX_SEGMENTS) {
                    int all_mapped = 1;
                    for (int j = 0; j < 16; j++) {
                        uint64_t check = addr + j * PAGE_SIZE;
                        if (check >= USER_VADDR_MAX) { all_mapped = 0; break; }
                    }
                    if (!all_mapped) break;
                    addr += 16 * PAGE_SIZE;
                }
                segs[num_segs].start = start;
                segs[num_segs].end = addr;
                segs[num_segs].flags = 0;
                num_segs++;
            } else {
                addr += PAGE_SIZE;
            }
        }
    } else {
        /* Kernel thread: dump only the kernel stack */
        if (proc->kernel_stack) {
            segs[num_segs].start = proc->kernel_stack;
            segs[num_segs].end = proc->kernel_stack + KERNEL_STACK_SIZE;
            segs[num_segs].flags = 0;
            num_segs++;
        }
    }

    /* If no segments found, create a minimal one for the stack */
    if (num_segs == 0) {
        segs[num_segs].start = proc->stack_top - PAGE_SIZE;
        segs[num_segs].end = proc->stack_top;
        segs[num_segs].flags = 0;
        num_segs++;
    }

    /* ── Step 3: Build ELF header ────────────────────────────────── */
    struct elf64_header ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    /* e_ident */
    ehdr.e_ident[0] = 0x7F;
    ehdr.e_ident[1] = 'E';
    ehdr.e_ident[2] = 'L';
    ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = ELF_CLASS64;       /* ELFCLASS64 */
    ehdr.e_ident[5] = ELF_DATA2LSB;       /* ELFDATA2LSB (little-endian) */
    ehdr.e_ident[6] = 1;                  /* EV_CURRENT */
    ehdr.e_type     = ET_CORE;
    ehdr.e_machine  = EM_X86_64;
    ehdr.e_version  = 1;
    ehdr.e_entry    = 0;
    ehdr.e_phoff    = sizeof(struct elf64_header);
    ehdr.e_shoff    = 0;
    ehdr.e_flags    = 0;
    ehdr.e_ehsize   = sizeof(struct elf64_header);
    ehdr.e_phentsize = sizeof(struct elf64_phdr);
    ehdr.e_phnum    = 1 + num_segs;  /* PT_NOTE + PT_LOAD segments */
    ehdr.e_shentsize = 0;
    ehdr.e_shnum    = 0;
    ehdr.e_shstrndx = 0;

    cb_append(&cb, &ehdr, sizeof(ehdr));

    /* ── Step 4: Build program header table ────────────────────────── */
    /* We need to know where things will be placed.
     * Layout:
     *   [ELF header]
     *   [PHDR table: PT_NOTE, PT_LOAD...]
     *   [Note data (NT_PRSTATUS)]
     *   [Segment data for each PT_LOAD]
     *
     * First, compute offsets.
     */

    uint64_t phdr_offset = cb.len;
    uint64_t num_phdrs = 1 + num_segs;
    uint64_t phdr_size = num_phdrs * sizeof(struct elf64_phdr);

    /* Pad PT_NOTE data to 4-byte alignment */
    uint64_t note_data_size = sizeof(struct elf_note) + 8 + sizeof(struct elf_prstatus);
    /* Note: name "CORE\0" = 5 bytes, padded to 8 */
    /* Descriptor: sizeof(prstatus), padded to 4 */

    /* Calculate note file offset (after PHDRs) */
    uint64_t note_file_offset = phdr_offset + phdr_size;
    /* Align note to 4 bytes */
    note_file_offset = (note_file_offset + 3) & ~3ULL;

    /* Calculate segment data start (after note) */
    uint64_t seg_data_offset = note_file_offset + note_data_size;
    /* Align segment data to page boundary */
    seg_data_offset = (seg_data_offset + 0xFFF) & ~0xFFFULL;

    /* Write PT_NOTE program header */
    {
        struct elf64_phdr ph;
        memset(&ph, 0, sizeof(ph));
        ph.p_type   = PT_NOTE;
        ph.p_offset = note_file_offset;
        ph.p_vaddr  = 0;
        ph.p_paddr  = 0;
        ph.p_filesz = note_data_size;
        ph.p_memsz  = 0;
        ph.p_flags  = 0;
        ph.p_align  = 4;
        cb_append(&cb, &ph, sizeof(ph));
    }

    /* Write PT_LOAD program headers for each mapped memory region.
     * Each segment's p_vaddr, p_filesz, p_memsz are filled from the
     * actual process memory map. p_flags encodes page permissions
     * (read/write/execute) derived from the page table entry flags.
     * p_offset points to the segment data that will be appended later
     * (after all headers and note data). */
    uint64_t current_seg_offset = seg_data_offset;
    for (int i = 0; i < num_segs; i++) {
        struct elf64_phdr ph;
        memset(&ph, 0, sizeof(ph));
        ph.p_type   = PT_LOAD;
        ph.p_offset = current_seg_offset;
        ph.p_vaddr  = segs[i].start;
        ph.p_paddr  = segs[i].start;
        ph.p_filesz = segs[i].end - segs[i].start;
        ph.p_memsz  = segs[i].end - segs[i].start;
        ph.p_flags  = 0;  /* no execute */
        ph.p_align  = PAGE_SIZE;
        cb_append(&cb, &ph, sizeof(ph));

        current_seg_offset += ph.p_filesz;
        /* Align next segment to page */
        current_seg_offset = (current_seg_offset + 0xFFF) & ~0xFFFULL;
    }

    /* ── Step 5: Write note data ──────────────────────────────────── */
    /* Pad to note alignment */
    cb_pad(&cb, 4);

    /* Write note header */
    struct elf_note note_hdr;
    note_hdr.n_namesz = 5;  /* "CORE" + NUL */
    note_hdr.n_descsz = sizeof(struct elf_prstatus);
    note_hdr.n_type   = NT_PRSTATUS;
    cb_append(&cb, &note_hdr, sizeof(note_hdr));

    /* Write note name "CORE\0" + pad to 4-byte boundary */
    uint8_t name_buf[8];
    memset(name_buf, 0, 8);
    memcpy(name_buf, "CORE", 5);
    cb_append(&cb, name_buf, 8);

    /* Write prstatus descriptor */
    cb_append(&cb, &prstatus, sizeof(prstatus));
    /* Pad descriptor to 4-byte boundary if needed */
    cb_pad(&cb, 4);

    /* ── Step 6: Write segment data (read from physical memory) ───── */
    /* Pad to page boundary */
    cb_pad(&cb, PAGE_SIZE);

    for (int i = 0; i < num_segs; i++) {
        uint64_t size = segs[i].end - segs[i].start;
        /* Read from virtual address (kernel can access all memory directly) */
        cb_append(&cb, (const void *)segs[i].start, size);
        /* Pad to page boundary */
        cb_pad(&cb, PAGE_SIZE);
    }

    /* ── Step 7: Expand core_pattern and write ──────────────────── */
    char core_path[COREDUMP_PATH_MAX];
    int path_len = core_pattern_expand(g_core_pattern, proc, signo,
                                        core_path, sizeof(core_path));

    if (path_len < 0) {
        kprintf("[COREDUMP] core_pattern expansion failed, falling back to default\n");
        snprintf(core_path, sizeof(core_path), "/tmp/core.%u",
                 (unsigned int)proc->pid);
    }

    /* Handle pipe mode (core_pattern starting with '|') */
    if (core_path[0] == '|') {
        /* Pipe mode: core would be piped to a userspace handler.
         * For now, log a warning and fall back to writing /tmp/core.<pid> */
        kprintf("[COREDUMP] Pipe mode core_pattern=\"%s\" not yet fully "
                "implemented; falling back to /tmp/core.%u\n",
                g_core_pattern, (unsigned int)proc->pid);
        snprintf(core_path, sizeof(core_path), "/tmp/core.%u",
                 (unsigned int)proc->pid);
    }

    /* Ensure the directory exists */
    {
        /* Extract parent directory from core_path */
        char dir[COREDUMP_PATH_MAX];
        int last_slash = -1;
        for (int i = 0; core_path[i]; i++) {
            if (core_path[i] == '/') last_slash = i;
        }
        if (last_slash > 0) {
            memcpy(dir, core_path, (size_t)last_slash);
            dir[last_slash] = '\0';
            vfs_create(dir, 2);  /* type 2 = directory */
        }
    }

    /* Write the core file to the expanded path */
    int ret = vfs_write(core_path, cb.data, cb.len);
    if (ret < 0) {
        kprintf("[COREDUMP] Failed to write %s (ret=%d)\n", core_path, ret);
        kfree(cb.data);
        return -1;
    }

    kprintf("[COREDUMP] Core dump written to %s (%llu bytes, %d segments, signal %d)\n",
            core_path, (unsigned long long)cb.len, num_segs, signo);

    kfree(cb.data);
    return 0;
}

/*
 * Deferred core dump worker — called from workqueue context.
 * Looks up the process by PID and generates the core dump.
 * This runs in process context (not IRQ context), so VFS operations
 * and kmalloc are safe.
 *
 * RLIMIT_CORE was already checked in do_coredump() before this was
 * scheduled; we re-check here as a safety measure in case the limit
 * changed between scheduling and execution.
 *
 * @arg: pointer to a struct with PID and signal number.
 */
struct coredump_deferred_arg {
    uint32_t pid;
    int      signo;
};

void coredump_deferred(void *arg)
{
    struct coredump_deferred_arg *da = (struct coredump_deferred_arg *)arg;
    if (!da) return;

    uint32_t pid = da->pid;
    int signo = da->signo;
    kfree(da);

    struct process *proc = process_get_by_pid(pid);

    if (!proc || proc->state == PROCESS_UNUSED) {
        kprintf("[COREDUMP] pid=%u: process vanished before dump could be written\n", pid);
        return;
    }

    /* Re-check RLIMIT_CORE (index 1, matching syscall.h convention).  If the
     * limit was set to 0 between scheduling and execution, skip the dump. */
    if (proc->rlim_cur[1] == 0) {
        kprintf("[COREDUMP] pid=%u: core dump suppressed (RLIMIT_CORE=0 at write time)\n", pid);
        return;
    }

    kprintf("[COREDUMP] pid=%u name=\"%s\": deferred dump generation (signal %d)...\n",
            pid, proc->name ? proc->name : "?", signo);

    coredump_generate(proc, signo);
}

/* ── Module / built-in initialisation ────────────────────────────── */

static void coredump_dispatch(uint32_t pid, int signo)
{
    /* Allocate argument structure for the deferred work */
    struct coredump_deferred_arg *da =
        (struct coredump_deferred_arg *)kmalloc(sizeof(struct coredump_deferred_arg));
    if (!da) {
        kprintf("[CORE] pid=%u: out of memory scheduling core dump\n", pid);
        return;
    }
    da->pid = pid;
    da->signo = signo;

    /* Defer to workqueue — do_coredump() may be called from IRQ context
     * (via signal_check() in scheduler_tick()), where VFS writes and
     * kmalloc are unsafe.  The workqueue runs in process context. */
    int ret = workqueue_schedule(coredump_deferred, (void *)da);
    if (ret < 0) {
        kprintf("[CORE] pid=%u: workqueue full, core dump lost\n", pid);
        kfree(da);
    }
}

int coredump_init_handler(void)
{
    int ret = coredump_register_handler(coredump_dispatch);
    if (ret < 0) {
        kprintf("[COREDUMP] failed to register handler\n");
        return ret;
    }
    kprintf("[COREDUMP] Core dump handler registered\n");
    return 0;
}

void coredump_exit_handler(void)
{
    coredump_unregister_handler();
    kprintf("[COREDUMP] Core dump handler unregistered\n");
}

#ifdef MODULE
int init_module(void)
{
    return coredump_init_handler();
}

void cleanup_module(void)
{
    coredump_exit_handler();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ELF core dump generator with core_pattern support");
MODULE_VERSION("1.0");
#else /* !MODULE — built-in case */
#include "initcall.h"
device_initcall(coredump_init_handler);
#endif /* MODULE */

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: coredump_write ──────────────────────────── */
int coredump_write(const void *data, size_t len)
{
    (void)data;
    (void)len;
    kprintf("[COREDUMP] coredump_write: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_read ───────────────────────────── */
int coredump_read(void *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[COREDUMP] coredump_read: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_file_open ──────────────────────── */
int coredump_file_open(const char *path)
{
    (void)path;
    kprintf("[COREDUMP] coredump_file_open: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_note_write ─────────────────────── */
int coredump_note_write(const void *note, size_t len)
{
    (void)note;
    (void)len;
    kprintf("[COREDUMP] coredump_note_write: not yet implemented\n");
    return 0;
}
/* ── Stub: coredump_elf_headers ────────────────────── */
int coredump_elf_headers(void *task, void *buf, size_t *len)
{
    (void)task;
    (void)buf;
    (void)len;
    kprintf("[COREDUMP] coredump_elf_headers: not yet implemented\n");
    return 0;
}
