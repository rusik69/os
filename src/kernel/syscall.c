#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "fs.h"
#include "vga.h"
#include "timer.h"
#include "printf.h"
#include "io.h"
#include "vmm.h"

/* MSR numbers */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

/* EFER bits */
#define EFER_SCE (1 << 0)  /* Syscall Enable */

extern void syscall_entry(void);

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── Syscall handlers ─────────────────────────────────────────── */

static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    (void)fd; (void)buf_addr; (void)len;
    /* Placeholder: stdin not wired for user processes yet */
    return (uint64_t)-1;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (fd == 1 || fd == 2) {
        const char *s = (const char *)buf_addr;
        for (uint64_t i = 0; i < len; i++)
            vga_putchar(s[i]);
        return len;
    }
    return (uint64_t)-1;
}

static uint64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode) {
    (void)flags; (void)mode;
    const char *path = (const char *)path_addr;
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) return (uint64_t)-1;
    /* Return a dummy fd (full fd table not implemented yet) */
    return 3;
}

static uint64_t sys_close(uint64_t fd) {
    (void)fd;
    return 0;
}

static uint64_t sys_exit(uint64_t code) {
    (void)code;
    struct process *p = process_get_current();
    /* If this is a user-mode process, clean up page tables */
    if (p && p->is_user && p->pml4) {
        vmm_switch_pml4(vmm_get_pml4()); /* switch back to kernel pages */
        vmm_destroy_user_pml4(p->pml4);
        p->pml4 = NULL;
    }
    process_exit();
    return 0; /* unreachable */
}

static uint64_t sys_getpid(void) {
    struct process *p = process_get_current();
    return p ? (uint64_t)p->pid : 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t sig) {
    (void)sig;
    struct process *p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)-1;
    p->state = PROCESS_ZOMBIE;
    return 0;
}

static uint64_t sys_brk(uint64_t addr) {
    /* Minimal stub — user-space heap management not yet implemented */
    (void)addr;
    return addr;
}

static uint64_t sys_stat(uint64_t path_addr, uint64_t out_addr) {
    const char *path = (const char *)path_addr;
    uint32_t *out = (uint32_t *)out_addr;
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) return (uint64_t)-1;
    if (out) { out[0] = size; out[1] = type; }
    return 0;
}

static uint64_t sys_mkdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    return (uint64_t)fs_create(path, 2 /* FS_TYPE_DIR */);
}

static uint64_t sys_unlink(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    return (uint64_t)fs_delete(path);
}

static uint64_t sys_time(void) {
    return timer_get_ticks() / TIMER_FREQ;
}

static uint64_t sys_yield(void) {
    scheduler_yield();
    return 0;
}

static uint64_t sys_uptime(void) {
    return timer_get_ticks();
}

/* ── Dispatch table ───────────────────────────────────────────── */

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    switch (num) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2, a3);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_EXIT:   return sys_exit(a1);
        case SYS_GETPID: return sys_getpid();
        case SYS_KILL:   return sys_kill(a1, a2);
        case SYS_BRK:    return sys_brk(a1);
        case SYS_STAT:   return sys_stat(a1, a2);
        case SYS_MKDIR:  return sys_mkdir(a1);
        case SYS_UNLINK: return sys_unlink(a1);
        case SYS_TIME:   return sys_time();
        case SYS_YIELD:  return sys_yield();
        case SYS_UPTIME: return sys_uptime();
        default:         return (uint64_t)-1;
    }
}

/* ── Init ─────────────────────────────────────────────────────── */

void syscall_init(void) {
    /* Enable SCE bit in EFER */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* STAR: bits 47:32 = kernel CS (0x08), bits 63:48 = user CS-8 (0x18-8=0x10
     * The CPU loads CS=STAR[47:32] and SS=STAR[47:32]+8 on syscall
     * On sysret:   CS=STAR[63:48]+16, SS=STAR[63:48]+8
     * We want kernel CS=0x08, user CS=0x18 (ring-3 code at GDT index 3)
     * So STAR[63:48] = 0x10 → sysret CS = 0x10+16 = 0x1B (with RPL3 added by CPU)
     */
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK: mask IF (bit 9) during syscall execution */
    wrmsr(MSR_SFMASK, (1 << 9));
}
