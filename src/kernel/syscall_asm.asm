bits 64

; ============================================================================
; Kernel/userspace interface
; ============================================================================
;
; RING-3 USER PROCESSES (ELF binaries)
;   Issue the `syscall` instruction → CPU jumps to syscall_entry (LSTAR MSR).
;   They have no other way to reach kernel code: they run at CPL=3 in their
;   own page tables, so any attempt to call a kernel address directly causes
;   a general-protection fault.  The syscall gate is the ONLY path.
;
; RING-0 KERNEL CODE (shell, built-in commands, drivers)
;   Call libc_syscall() which tail-calls syscall_dispatch() directly.
;   This is safe because ring-0 code is already part of the kernel binary and
;   has full trust; routing it through the syscall gate buys nothing and
;   introduces brittleness (stack switching, interrupt masking, alignment
;   constraints).  The separation for ring-0 code is logical, not hardware-
;   enforced — it goes through the same dispatch table as ring-3 code.
;
; libc_syscall dispatches based on the caller's RSP:
;   bit 63 set → kernel (high-half) address → direct call to syscall_dispatch
;   bit 63 clear → user  (low-half)  address → syscall instruction → gate

global syscall_entry
global syscall_entry_full
global libc_syscall
extern syscall_dispatch
extern zero_kernel_stack_uapi

; KPTI trampoline constants (must match kpti.h)
%define KPTI_TRAMP_VADDR   0x00007FFFFFFE0000
%define KPTI_OFF_CR3_KERN  0x100
%define KPTI_OFF_CR3_USER  0x108
%define KPTI_OFF_SAVE_RSP  0x110
%define KPTI_OFF_SAVE_RIP  0x118
%define KPTI_OFF_SAVE_RFL  0x120
%define KPTI_OFF_EXIT_RIP  0x128
%define KPTI_OFF_EXIT      0x080

; Per-CPU kernel stack pointer offset within cpu_info struct (smp.h)
;   cpu_info.current_kernel_rsp is at offset 24 (0x18)
; Accessed via GS.base which points to the current CPU's cpu_info.
%define CPU_INFO_KERNEL_RSP_OFF  0x18

; Save user RIP/RFLAGS at syscall entry so clone() can read them
global syscall_user_rip
global syscall_user_rflags
syscall_user_rip: dq 0
syscall_user_rflags: dq 0

; 6th syscall argument (R9 from user) — saved here before the dispatch call.
global syscall_arg6
syscall_arg6: dq 0

; execve state: when execve_pending is non-zero, the syscall return path
; uses these values instead of the saved stack state.
global execve_pending
global execve_user_rip
global execve_user_rflags
global execve_user_rsp
execve_pending: dq 0
execve_user_rip: dq 0
execve_user_rflags: dq 0
execve_user_rsp: dq 0

; ── Kernel stack zeroing ────────────────────────────────────────────
global syscall_entry_rsp_saved
syscall_entry_rsp_saved: dq 0

; ── KPTI mode selector: 0 = disabled, 1 = active ────────────────────
; Set by kpti_init().  When active, the original syscall_entry is not used
; from userspace (LSTAR points to the trampoline), but syscall_entry_full
; is the handler called by the trampoline.
global kpti_active_flag
kpti_active_flag: dq 0

; ============================================================================
; syscall_entry — ring-3 path only (used when KPTI is DISABLED)
; ============================================================================
;
; On entry (CPU has done):
;   RCX = saved user RIP (instruction after `syscall`)
;   R11 = saved user RFLAGS
;   RSP = user stack pointer (NOT changed by the CPU)
;   RAX = syscall number
;   RDI = a1, RSI = a2, RDX = a3, R10 = a4, R8 = a5
;
; We switch to the per-process kernel stack (syscall_kernel_rsp), save all
; user-visible state, call syscall_dispatch, restore, and sysret.

section .text
syscall_entry:
    mov     [rel syscall_user_rsp], rsp        ; save user RSP
    mov     rsp, [gs:CPU_INFO_KERNEL_RSP_OFF]  ; switch to per-CPU process kernel stack

    push    qword [rel syscall_user_rsp]       ; saved user RSP   (frame 1)
    push    rcx                                ; saved user RIP   (frame 2)
    push    r11                                ; saved user RFLAGS (frame 3)
    push    rbp                                ; (4)
    push    rbx                                ; (5)
    push    r12                                ; (6)
    push    r13                                ; (7)
    push    r14                                ; (8)
    push    r15                                ; (9)

    ; Save the stack pointer after pushing all registers for stack zeroing.
    mov     [rel syscall_entry_rsp_saved], rsp

    ; Save user RIP and RFLAGS for clone()
    mov     [rel syscall_user_rip], rcx
    mov     [rel syscall_user_rflags], r11

    ; Save user R9 (6th syscall argument) before clobbering it.
    mov     [rel syscall_arg6], r9

    ; Arg shuffle: syscall_dispatch(num, a1, a2, a3, a4, a5) — SysV
    mov     r9,  r8         ; a5  (save before r8 is overwritten)
    mov     r8,  r10        ; a4  (r10 holds arg4 per Linux syscall ABI)
    mov     rcx, rdx        ; a3
    mov     rdx, rsi        ; a2
    mov     rsi, rdi        ; a1
    mov     rdi, rax        ; num

    call    syscall_dispatch ; result in rax

    ; Check if execve() was called
    cmp     qword [rel execve_pending], 0
    je      .normal_return

    ; Force execve return
    xor     eax, eax               ; execve returns 0
    mov     rcx, [rel execve_user_rip]
    mov     r11, [rel execve_user_rflags]
    mov     rsp, [rel execve_user_rsp]
    mov     qword [rel execve_pending], 0
    o64 sysret

.normal_return:
    ; ── Zero kernel stack ────────────────────────────────────────────
    mov     rdi, [rel syscall_entry_rsp_saved]  ; arg0 = entry RSP
    call    zero_kernel_stack_uapi

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    pop     r11             ; user RFLAGS → sysret reads R11
    pop     rcx             ; user RIP   → sysret reads RCX
    pop     rsp             ; user RSP

    o64 sysret              ; return to ring-3 user mode

; ============================================================================
; syscall_entry_full — KPTI handler (called from the trampoline)
; ============================================================================
;
; The KPTI trampoline has already:
;   1. Saved user RSP, RCX (RIP), R11 (RFLAGS) to the trampoline page
;   2. Switched CR3 to kernel page table
;   3. Jumped here (at kernel VMA)
;
; We need to read the saved user state from the trampoline page and
; proceed with normal syscall handling.  At the end, instead of sysret,
; we jump back to the exit trampoline (which switches CR3 to user PML4).

syscall_entry_full:
    ; Load KPTI base address once
    mov     r15, KPTI_TRAMP_VADDR
    ; Read saved user state from trampoline page
    mov     rcx, [r15 + KPTI_OFF_SAVE_RIP]   ; user RIP
    mov     r11, [r15 + KPTI_OFF_SAVE_RFL]    ; user RFLAGS
    mov     rax, [r15 + KPTI_OFF_SAVE_RSP]    ; user RSP
    mov     [rel syscall_user_rsp], rax

    ; Switch to per-CPU process kernel stack
    mov     rsp, [gs:CPU_INFO_KERNEL_RSP_OFF]

    ; Push saved state (same frame as syscall_entry)
    push    qword [rel syscall_user_rsp]       ; saved user RSP
    push    rcx                                ; saved user RIP
    push    r11                                ; saved user RFLAGS
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rel syscall_entry_rsp_saved], rsp

    ; Save RIP/RFLAGS for clone() — read back from trampoline
    mov     rcx, [r15 + KPTI_OFF_SAVE_RIP]
    mov     r11, [r15 + KPTI_OFF_SAVE_RFL]
    mov     [rel syscall_user_rip], rcx
    mov     [rel syscall_user_rflags], r11

    ; Save arg6
    mov     [rel syscall_arg6], r9

    ; Arg shuffle (same as syscall_entry)
    mov     r9,  r8
    mov     r8,  r10
    mov     rcx, rdx
    mov     rdx, rsi
    mov     rsi, rdi
    mov     rdi, rax

    call    syscall_dispatch

    ; Check execve
    cmp     qword [rel execve_pending], 0
    je      .full_normal_return

    ; Execve path: jump to exit trampoline
    xor     eax, eax
    mov     rcx, [rel execve_user_rip]
    mov     r11, [rel execve_user_rflags]
    mov     rsp, [rel execve_user_rsp]
    mov     qword [rel execve_pending], 0

    ; Jump to exit trampoline
    mov     r15, KPTI_TRAMP_VADDR + KPTI_OFF_EXIT
    jmp     r15

.full_normal_return:
    ; Zero kernel stack
    mov     rdi, [rel syscall_entry_rsp_saved]
    call    zero_kernel_stack_uapi

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    pop     r11             ; user RFLAGS
    pop     rcx             ; user RIP
    pop     rsp             ; user RSP

    ; Instead of sysret, jump to exit trampoline which switches CR3 and sysrets
    mov     r15, KPTI_TRAMP_VADDR + KPTI_OFF_EXIT
    jmp     r15

; ============================================================================
; libc_syscall — C library syscall gateway
; ============================================================================
;
; C prototype (libc.h):
;   uint64_t libc_syscall(uint64_t num, uint64_t a1, uint64_t a2,
;                         uint64_t a3, uint64_t a4, uint64_t a5);
;
; SysV calling convention on entry:
;   rdi=num  rsi=a1  rdx=a2  rcx=a3  r8=a4  r9=a5
;
; Dispatch:
;   RSP bit 63 set   → kernel-mode caller → tail-call syscall_dispatch directly
;   RSP bit 63 clear → user-mode caller   → syscall instruction → syscall_entry

libc_syscall:
    test    rsp, rsp
    js      .kernel_direct   ; sign bit set → high-half → kernel caller

    ; User-mode path: syscall instruction
    mov     rax, rdi        ; num
    mov     rdi, rsi        ; a1
    mov     rsi, rdx        ; a2
    mov     rdx, rcx        ; a3
    mov     r10, r8         ; a4
    mov     r8,  r9         ; a5
    syscall                 ; → LSTAR; result in rax
    ret

.kernel_direct:
    ; Kernel-mode path: tail-call syscall_dispatch directly
    jmp     syscall_dispatch

section .bss
syscall_user_rsp: resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
