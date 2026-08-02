; kpti_trampoline.asm — KPTI syscall entry/exit trampoline (FLAT BINARY)
;
; Assembled with: nasm -f bin -o kpti_trampoline.bin kpti_trampoline.asm
; Embedded into kernel via xxx -i and copied to trampoline page at boot.
;
; This code runs at KPTI_TRAMPOLINE_VADDR (0x7FFFFFFE0000).
; The `org` directive makes NASM generate correct absolute addresses.
; The physical page is mapped supervisor-only in both kernel and user PML4s.
;
; On SYSCALL entry (from user mode):
;   RCX = user RIP, R11 = user RFLAGS, RSP = user stack
;   RAX = syscall number, RDI..R8 = args (Linux ABI)
;
; CRITICAL: All data accesses use RIP-relative addressing via a base
; register (r15). This is required because the org address (0x7FFFFFFE0000)
; is too large for 32-bit sign-extended displacements — NASM would encode
; wrong addresses using the SIB+disp32 encoding.

%define KPTI_TRAMP_VADDR   0x00007FFFFFFE0000
%define KPTI_OFF_ENTRY     0x000
%define KPTI_OFF_EXIT      0x080
%define KPTI_OFF_CR3_KERN  0x100
%define KPTI_OFF_CR3_USER  0x108
%define KPTI_OFF_SAVE_RSP  0x110
%define KPTI_OFF_SAVE_RIP  0x118
%define KPTI_OFF_SAVE_RFL  0x120
%define KPTI_OFF_EXIT_RIP  0x128
%define KPTI_OFF_SAVE_RAX  0x130

%define CPU_INFO_KERNEL_RSP_OFF  0x18

org KPTI_TRAMP_VADDR
bits 64

; ============================================================================
; syscall_entry_trampoline — at KPTI_OFF_ENTRY (0x000)
; ============================================================================
; Saves user state, switches to kernel stack, switches to kernel page table,
; jumps to real handler.
;
; On entry (CPU has done):
;   RCX = saved user RIP  (instruction after `syscall`)
;   R11 = saved user RFLAGS
;   RSP = user stack
;   RAX = syscall number
;   RDI = a1, RSI = a2, RDX = a3, R10 = a4, R8 = a5, R9 = a6
;
; We use RIP-relative addressing via r15 for all data accesses because the
; org address (0x7FFFFFFExxxx) is too large for NASM's32-bit displacement
; encoding. LEA with [rip+disp32] computes correct absolute addresses.

global syscall_entry_trampoline
syscall_entry_trampoline:

    ; CRITICAL: syscall does NOT clear IF.  Disable interrupts immediately —
    ; a timer interrupt here would push the interrupt frame onto the USER
    ; stack (RSP still points there until we switch stacks below), corrupting
    ; it so a later userspace `ret` jumps to kernel addresses.  IF is restored
    ; by sysret (from user R11) at exit.
    cli

    ; Load r15 with RIP-relative pointer to the data area.
    ; lea r15, [rip + disp32] always works correctly regardless of org.
    lea     r15, [rel kpti_cr3_kernel_dword]

    ; Save user RAX (syscall number) before clobbering it
    mov     [r15 + KPTI_OFF_SAVE_RAX - KPTI_OFF_CR3_KERN], rax
    ; Save user RSP, RIP (RCX), RFLAGS (R11)
    mov     [r15 + KPTI_OFF_SAVE_RSP - KPTI_OFF_CR3_KERN], rsp
    mov     [r15 + KPTI_OFF_SAVE_RIP - KPTI_OFF_CR3_KERN], rcx
    mov     [r15 + KPTI_OFF_SAVE_RFL - KPTI_OFF_CR3_KERN], r11

    ; Switch to kernel CR3 FIRST — the trampoline page is mapped in both
    ; the user PML4 (kpti_setup_process) and the kernel PML4 (kpti_init),
    ; so instruction fetch and data access remain valid after the switch.
    ; The KPTI user PML4 has NO kernel entries (256-511 are zero), so the
    ; GS-relative access below would page-fault if we stayed on user CR3.
    mov     rax, [r15 + KPTI_OFF_CR3_KERN - KPTI_OFF_CR3_KERN]
    mov     cr3, rax

    ; Now on kernel PML4 — kernel memory is accessible.
    ; Load per-CPU kernel stack pointer via GS.
    mov     rsp, [gs:CPU_INFO_KERNEL_RSP_OFF]

    ; We're now in kernel page table mode with kernel stack — the
    ; user-stack corruption window is closed.  Re-enable interrupts so
    ; the scheduler can preempt (e.g. a blocking waitpid must be
    ; interruptible, and the timer softirq must keep running).
    sti

    ; We're now in kernel page table mode with kernel stack.
    ; Jump to the real syscall handler at kernel VMA.
    ; The handler address was patched in by kpti_init().
    mov     rax, [r15 + KPTI_OFF_EXIT_RIP - KPTI_OFF_CR3_KERN]
    jmp     rax

; Pad to KPTI_OFF_EXIT (0x080).  Without this, the exit trampoline lands
; immediately after the entry code (~0x02C) and the kernel's jump to
; KPTI_OFF_EXIT hits zero-filled padding that executes as garbage.
times KPTI_OFF_EXIT - ($ - $$) db 0

; ============================================================================
; syscall_exit_trampoline — at KPTI_OFF_EXIT (0x080)
; ============================================================================
; Called from the kernel's syscall return path before SYSRET.
; On entry (from jump in syscall_entry_full):
;   RAX = syscall return value
;   RCX = user RIP
;   R11 = user RFLAGS
;   RSP = user stack (already set up by kernel return path)
;
; Saves the return state including RAX, switches to user CR3,
; restores RAX, then SYSRET.
;   - We must save/restore RAX because the exit trampoline clobbers
;     it with the CR3 load. Without this, the user would see CR3 as
;     the syscall return value instead of the actual return value.

global syscall_exit_trampoline
syscall_exit_trampoline:

    ; CRITICAL: RSP is the USER stack here (popped by the kernel return path)
    ; and IF must be 0 — an interrupt in this window would push the interrupt
    ; frame onto the user stack and corrupt it.  The entry trampoline cleared
    ; IF, but clear it again defensively (covers any future path that enters
    ; with IF set).  sysret restores IF from R11.
    cli

    ; Load r15 with RIP-relative pointer to the data area
    lea     r15, [rel kpti_cr3_kernel_dword]

    ; Save the return values including RAX (syscall result)
    mov     [r15 + KPTI_OFF_SAVE_RAX - KPTI_OFF_CR3_KERN], rax
    mov     [r15 + KPTI_OFF_SAVE_RIP - KPTI_OFF_CR3_KERN], rcx
    mov     [r15 + KPTI_OFF_SAVE_RFL - KPTI_OFF_CR3_KERN], r11
    mov     [r15 + KPTI_OFF_SAVE_RSP - KPTI_OFF_CR3_KERN], rsp

    ; Switch to user page table
    mov     rax, [r15 + KPTI_OFF_CR3_USER - KPTI_OFF_CR3_KERN]
    mov     cr3, rax

    ; Restore user state — read RAX back so user gets the real return value
    mov     rax, [r15 + KPTI_OFF_SAVE_RAX - KPTI_OFF_CR3_KERN]
    mov     rcx, [r15 + KPTI_OFF_SAVE_RIP - KPTI_OFF_CR3_KERN]
    mov     r11, [r15 + KPTI_OFF_SAVE_RFL - KPTI_OFF_CR3_KERN]
    mov     rsp, [r15 + KPTI_OFF_SAVE_RSP - KPTI_OFF_CR3_KERN]

    o64 sysret

; ============================================================================
; Data area — filled at runtime by kpti_init() / kpti_trampoline_patch_cr3()
; ============================================================================
times KPTI_OFF_CR3_KERN - ($ - $$) db 0

global kpti_cr3_kernel_dword
kpti_cr3_kernel_dword:
    dq 0  ; KPTI_OFF_CR3_KERN: kernel PML4 physical address
kpti_cr3_user_dword:
    dq 0  ; KPTI_OFF_CR3_USER: user PML4 physical address

; Saved register state (used by both entry and exit)
kpti_saved_rsp:
    dq 0  ; KPTI_OFF_SAVE_RSP
kpti_saved_rip:
    dq 0  ; KPTI_OFF_SAVE_RIP
kpti_saved_rfl:
    dq 0  ; KPTI_OFF_SAVE_RFL
kpti_real_handler_addr:
    dq 0  ; KPTI_OFF_EXIT_RIP: patched to kernel's syscall_entry_full address
kpti_saved_rax:
    dq 0  ; KPTI_OFF_SAVE_RAX: saved RAX (syscall number / return value)
