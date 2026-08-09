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
global syscall_linux_entry
global libc_syscall
extern syscall_dispatch
extern syscall_linux_dispatch
extern kprintf_syscall_trace
extern zero_kernel_stack_uapi

; KPTI trampoline constants (must match kpti.h)
%define KPTI_TRAMP_VADDR   0x00007FFFFFFE0000
%define KPTI_OFF_CR3_KERN  0x100
%define KPTI_OFF_CR3_USER  0x108
%define KPTI_OFF_SAVE_RSP  0x110
%define KPTI_OFF_SAVE_RIP  0x118
%define KPTI_OFF_SAVE_RFL  0x120
%define KPTI_OFF_EXIT_RIP  0x128
%define KPTI_OFF_SAVE_RAX  0x130
%define KPTI_OFF_SAVE_R15  0x138
%define KPTI_OFF_SAVE_R10  0x140
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

; ── User R15 save for KPTI syscall_entry_full ──────────────────────
; clobbered by mov r15, KPTI_TRAMP_VADDR before push
global syscall_user_r15
syscall_user_r15: dq 0
global syscall_user_r14
syscall_user_r14: dq 0
global syscall_user_r13
syscall_user_r13: dq 0
global syscall_user_r12
syscall_user_r12: dq 0
global syscall_user_rbx
syscall_user_rbx: dq 0
global syscall_user_rbp
syscall_user_rbp: dq 0

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
    ; CRITICAL: syscall does NOT clear IF.  Disable interrupts immediately
    ; so a timer/NMI cannot fire while RSP still points at the user stack,
    ; which would push the interrupt frame onto the user stack and corrupt
    ; it (userspace `ret` then jumps to kernel addresses).  Interrupts stay
    ; disabled through dispatch (original design) and are restored by sysret.
    cli

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
    push    rax                                 ; save syscall return value (clobbered by zero_kernel_stack_uapi)
    mov     rdi, [rel syscall_entry_rsp_saved]  ; arg0 = entry RSP
    call    zero_kernel_stack_uapi
    pop     rax                                 ; restore syscall return value

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
; syscall_linux_entry — Linux ABI ring-3 path (same entry protocol)
; ============================================================================
;
; Identical to syscall_entry except it dispatches via sys_call_table[]
; (indexed by Linux __NR_* syscall numbers) instead of the internal
; syscall_dispatch (which uses SYS_* numbers).
;
; After dispatching through the table, the entry returns to userspace
; via sysret exactly as syscall_entry does.
;
; On entry (CPU has done):
;   RCX = saved user RIP (instruction after `syscall`)
;   R11 = saved user RFLAGS
;   RSP = user stack pointer
;   RAX = syscall number
;   RDI = a1, RSI = a2, RDX = a3, R10 = a4, R8 = a5, R9 = a6

syscall_linux_entry:
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

    ; Save user callee-saved registers for fork() child inheritance.
    mov     [rel syscall_user_rbp], rbp
    mov     [rel syscall_user_rbx], rbx
    mov     [rel syscall_user_r12], r12
    mov     [rel syscall_user_r13], r13
    mov     [rel syscall_user_r14], r14
    mov     [rel syscall_user_r15], r15

    ; Arg shuffle: syscall_linux_dispatch(num, a1, a2, a3, a4, a5) — SysV
    mov     r9,  r8         ; a5  (save before r8 is overwritten)
    mov     r8,  r10        ; a4  (r10 holds arg4 per Linux syscall ABI)
    mov     rcx, rdx        ; a3
    mov     rdx, rsi        ; a2
    mov     rsi, rdi        ; a1
    mov     rdi, rax        ; num

    call    syscall_linux_dispatch ; result in rax

    ; Check if execve() was called
    cmp     qword [rel execve_pending], 0
    je      .linux_normal_return

    ; Force execve return
    xor     eax, eax               ; execve returns 0
    mov     rcx, [rel execve_user_rip]
    mov     r11, [rel execve_user_rflags]
    mov     rsp, [rel execve_user_rsp]
    mov     qword [rel execve_pending], 0
    o64 sysret

.linux_normal_return:
    ; ── Zero kernel stack ────────────────────────────────────────────
    push    rax                                 ; save syscall return value (clobbered by zero_kernel_stack_uapi)
    mov     rdi, [rel syscall_entry_rsp_saved]  ; arg0 = entry RSP
    call    zero_kernel_stack_uapi
    pop     rax                                 ; restore syscall return value

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
    ; Read the REAL user R15 saved by the entry trampoline (it clobbered
    ; r15 with the data-area base before jumping here).  The old code read
    ; the clobbered r15, so fork() inherited the trampoline address as the
    ; child's R15 — and every normal syscall returned with R15 pointing at
    ; the KPTI data area, crashing gcc15 userspace with a #GP.
    mov     rax, [r15 + KPTI_OFF_SAVE_R15]
    mov     [rel syscall_user_r15], rax
    ; Read saved user state from trampoline page
    mov     rcx, [r15 + KPTI_OFF_SAVE_RIP]   ; user RIP
    mov     r11, [r15 + KPTI_OFF_SAVE_RFL]    ; user RFLAGS
    mov     rax, [r15 + KPTI_OFF_SAVE_RSP]    ; user RSP
    mov     [rel syscall_user_rsp], rax
    ; Read saved syscall number (trampoline saved RAX before clobbering it)
    mov     rax, [r15 + KPTI_OFF_SAVE_RAX]    ; syscall number

    ; Switch to per-CPU process kernel stack
    mov     rsp, [gs:CPU_INFO_KERNEL_RSP_OFF]

    ; Debug: trace syscall entry (rax = syscall number)
    ; (disabled — per-syscall kprintf floods the boot log)
    ; push    rax
    ; push    rcx
    ; push    r11
    ; mov     rdi, rax
    ; xor     esi, esi
    ; call    kprintf_syscall_trace
    ; pop     r11
    ; pop     rcx
    ; pop     rax

    ; Push saved state (same frame as syscall_entry)
    push    qword [rel syscall_user_rsp]       ; saved user RSP
    push    rcx                                ; saved user RIP
    push    r11                                ; saved user RFLAGS
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    qword [rel syscall_user_r15]       ; saved user R15 (not KPTI base!)

    mov     [rel syscall_entry_rsp_saved], rsp

    ; Save RIP/RFLAGS for clone() — read back from trampoline
    mov     rcx, [r15 + KPTI_OFF_SAVE_RIP]
    mov     r11, [r15 + KPTI_OFF_SAVE_RFL]
    mov     [rel syscall_user_rip], rcx
    mov     [rel syscall_user_rflags], r11

    ; Save arg6
    mov     [rel syscall_arg6], r9

    ; Save the remaining user callee-saved registers for fork(): the
    ; fork child must inherit the parent's rbp/rbx/r12/r13/r14/r15
    ; (userspace relies on them across the fork call — e.g. init keeps
    ; argv/envp pointers in r13/rbx for the getty exec).  r15 was saved
    ; above; the rest are still pristine here (only rax/rcx/r11/rsp/r15
    ; and the arg registers have been touched since the syscall entry).
    mov     [rel syscall_user_rbp], rbp
    mov     [rel syscall_user_rbx], rbx
    mov     [rel syscall_user_r12], r12
    mov     [rel syscall_user_r13], r13
    mov     [rel syscall_user_r14], r14

    ; Arg shuffle (same as syscall_entry) — RAX now holds syscall number
    mov     r9,  r8
    mov     r8,  r10
    mov     rcx, rdx
    mov     rdx, rsi
    mov     rsi, rdi
    mov     rdi, rax        ; num

    call    syscall_dispatch

    ; Check execve
    cmp     qword [rel execve_pending], 0
    je      .full_normal_return

    ; Execve path: jump to exit trampoline (process replacement — no R15 restore needed)
    ; CRITICAL: disable interrupts BEFORE switching RSP to the user stack.
    ; From here to sysret the stack pointer is the USER stack, and a timer
    ; interrupt in this window would push its frame onto the user stack,
    ; corrupting it (userspace #GP at the exec'd binary's _start, ~1 in 5
    ; boots).  The .full_normal_return path has the same cli for the same
    ; reason; the exit trampoline re-clears IF defensively.
    cli
    xor     eax, eax
    mov     rcx, [rel execve_user_rip]
    mov     r11, [rel execve_user_rflags]
    mov     rsp, [rel execve_user_rsp]
    mov     qword [rel execve_pending], 0

    ; Fresh exec: the new process starts with zeroed callee-saved regs
    ; (Linux ABI: all regs except RSP are zeroed at exec entry).  Zero the
    ; trampoline's saved R15/R10 slots so the exit trampoline restores 0
    ; instead of the old process's values.  r15 is still KPTI_TRAMP_VADDR
    ; here (set at syscall_entry_full entry), so write via r15-relative.
    mov     qword [r15 + KPTI_OFF_SAVE_R15], 0
    mov     qword [r15 + KPTI_OFF_SAVE_R10], 0

    ; Zero the remaining GP registers too — the exit trampoline only
    ; restores RAX/RCX/R11/RSP/R15/R10, so RSI/RDX/RBX/R8/R9/R12/R13/R14
    ; pass through from syscall_dispatch's return state (kernel
    ; addresses).  A fresh exec must present a clean register file
    ; (observed: userspace #GP at getty _start with RSI=0xffff8000... and
    ; garbage R12/R13/R14 leaking from the old process).
    xor     esi, esi
    xor     edx, edx
    xor     ebx, ebx
    xor     r8d, r8d
    xor     r9d, r9d
    xor     r12d, r12d
    xor     r13d, r13d
    xor     r14d, r14d
    xor     ebp, ebp
    xor     edi, edi

    ; Jump to exit trampoline WITHOUT clobbering RAX.  The exit trampoline
    ; saves RAX into KPTI_OFF_SAVE_RAX and restores it as the new process's
    ; return value — so RAX must arrive here already 0 (Linux ABI: RAX=0 at
    ; exec entry).  Jumping via `mov rax, KPTI_TRAMP_VADDR+KPTI_OFF_EXIT;
    ; jmp rax` made the trampoline restore the trampoline address into RAX
    ; (observed: userspace #GP at getty _start with RAX=0x7ffffffe0080).
    ; r15 is safe as the jump register: the exit trampoline's first
    ; instruction is `lea r15, [rel base]` which clobbers it, and it then
    ; restores r15 from the (zeroed) SAVE_R15 slot — so the new process
    ; starts with r15 = 0 per the ABI.
    mov     r15, KPTI_TRAMP_VADDR + KPTI_OFF_EXIT
    jmp     r15

.full_normal_return:
    ; Zero kernel stack
    push    rax                                 ; save syscall return value (clobbered by zero_kernel_stack_uapi)
    mov     rdi, [rel syscall_entry_rsp_saved]
    call    zero_kernel_stack_uapi
    pop     rax                                 ; restore syscall return value

    ; Disable interrupts BEFORE popping the user RSP — from here to sysret
    ; the stack pointer will be the USER stack, and a timer interrupt in
    ; this window would push its frame onto the user stack, corrupting it
    ; (userspace `ret` then jumps to kernel addresses).
    cli

    pop     r15                                 ; user R15 (saved via syscall_user_r15 global)
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    pop     r11             ; user RFLAGS
    pop     rcx             ; user RIP
    pop     rsp             ; user RSP

    ; Jump to exit trampoline via R10 (preserves RAX = syscall return value)
    mov     r10, KPTI_TRAMP_VADDR + KPTI_OFF_EXIT
    jmp     r10

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
global syscall_user_rsp
syscall_user_rsp: resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
