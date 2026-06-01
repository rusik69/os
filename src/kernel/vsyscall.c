/* vsyscall.c — VDSO-like vsyscall page for fast user-space syscalls */

#include "vsyscall.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"

/* Fixed vsyscall page virtual address (user-accessible, high address) */
#define VSYSCALL_PAGE_VADDR  0xFFFFFFFFFF600000ULL

/* Static page for vsyscall — contains trampoline code */
static uint8_t vsyscall_page[4096] __attribute__((aligned(4096)));

/* Marker for initialization */
static int vsyscall_initialized = 0;

/* Simple vsyscall trampoline code.
 * In a real system this would contain gettimeofday(), time(), getcpu()
 * implementations that don't need a ring transition.
 * Here we provide a minimal stub that executes "syscall" instruction
 * with the appropriate syscall number. */

/* Assembly stub for vsyscall#0 (gettimeofday) */
static const uint8_t vsyscall_gettimeofday_stub[] = {
    0xB8, 0x60, 0x00, 0x00, 0x00,  /* mov eax, 0x60  (SYS_gettimeofday) */
    0x0F, 0x05,                     /* syscall */
    0xC3,                           /* ret */
};

/* vsyscall#1 (time) */
static const uint8_t vsyscall_time_stub[] = {
    0xB8, 0xC9, 0x00, 0x00, 0x00,  /* mov eax, 0xC9 (SYS_time) */
    0x0F, 0x05,                     /* syscall */
    0xC3,                           /* ret */
};

/* vsyscall#2 (getcpu) */
static const uint8_t vsyscall_getcpu_stub[] = {
    0xB8, 0x35, 0x01, 0x00, 0x00,  /* mov eax, 0x135 (SYS_getcpu) */
    0x0F, 0x05,                     /* syscall */
    0xC3,                           /* ret */
};

#define VSYSCALL_ENTRY_SIZE 64
#define VSYSCALL_NR_ENTRIES 3

int vsyscall_init(void) {
    if (vsyscall_initialized) return 0;

    /* Clear the vsyscall page */
    memset(vsyscall_page, 0, sizeof(vsyscall_page));

    /* Place stubs at fixed offsets within the page */
    memcpy(&vsyscall_page[0 * VSYSCALL_ENTRY_SIZE],
           vsyscall_gettimeofday_stub, sizeof(vsyscall_gettimeofday_stub));

    memcpy(&vsyscall_page[1 * VSYSCALL_ENTRY_SIZE],
           vsyscall_time_stub, sizeof(vsyscall_time_stub));

    memcpy(&vsyscall_page[2 * VSYSCALL_ENTRY_SIZE],
           vsyscall_getcpu_stub, sizeof(vsyscall_getcpu_stub));

    /* Map the vsyscall page in the kernel's page tables as user-accessible,
     * read-only, with NX cleared (execute allowed) */
    vmm_map_page(VSYSCALL_PAGE_VADDR,
                 VIRT_TO_PHYS((uint64_t)vsyscall_page),
                 VMM_FLAG_PRESENT | VMM_FLAG_USER); /* No VMM_FLAG_NOEXEC — pages are executable */

    vsyscall_initialized = 1;
    kprintf("[cpu] vsyscall page at 0x%llx\n", (uint64_t)VSYSCALL_PAGE_VADDR);
    return 0;
}

void *vsyscall_get_page(void) {
    return (void *)VSYSCALL_PAGE_VADDR;
}
