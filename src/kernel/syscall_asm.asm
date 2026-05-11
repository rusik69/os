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
global libc_syscall
extern syscall_dispatch

section .data
global syscall_kernel_rsp
syscall_kernel_rsp: dq 0   ; Set by scheduler when switching to a user process

; ============================================================================
; syscall_entry — ring-3 path only
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
    mov     rsp, [rel syscall_kernel_rsp]      ; switch to kernel stack

    push    qword [rel syscall_user_rsp]       ; saved user RSP   (frame 1)
    push    rcx                                ; saved user RIP   (frame 2)
    push    r11                                ; saved user RFLAGS (frame 3)
    push    rbp                                ; (4)
    push    rbx                                ; (5)
    push    r12                                ; (6)
    push    r13                                ; (7)
    push    r14                                ; (8)
    push    r15                                ; (9)

    ; Arg shuffle: syscall_dispatch(num, a1, a2, a3, a4, a5) — SysV
    ;   target: rdi=num  rsi=a1  rdx=a2  rcx=a3  r8=a4  r9=a5
    mov     r9,  r8         ; a5  (save before r8 is overwritten)
    mov     r8,  r10        ; a4  (r10 holds arg4 per Linux syscall ABI)
    mov     rcx, rdx        ; a3
    mov     rdx, rsi        ; a2
    mov     rsi, rdi        ; a1
    mov     rdi, rax        ; num

    call    syscall_dispatch ; result in rax

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
;                       (args already in the right SysV registers)
;   RSP bit 63 clear → user-mode caller   → syscall instruction → syscall_entry
;                       (remap SysV args to Linux syscall register convention)

libc_syscall:
    test    rsp, rsp
    js      .kernel_direct   ; sign bit set → high-half → kernel caller

    ; ----------------------------------------------------------------
    ; User-mode path (ring 3): issue the syscall instruction.
    ; Remap: SysV (rdi,rsi,rdx,rcx,r8,r9) → syscall (rax,rdi,rsi,rdx,r10,r8)
    ; ----------------------------------------------------------------
    mov     rax, rdi        ; num
    mov     rdi, rsi        ; a1
    mov     rsi, rdx        ; a2
    mov     rdx, rcx        ; a3  (rcx is later clobbered by `syscall` itself,
                            ;       but we've already moved the value to rdx here)
    mov     r10, r8         ; a4  (syscall uses r10 for arg4, not rcx)
    mov     r8,  r9         ; a5
    syscall                 ; → syscall_entry; result in rax
    ret

.kernel_direct:
    ; ----------------------------------------------------------------
    ; Kernel-mode path (ring 0): tail-call syscall_dispatch directly.
    ; syscall_dispatch has the SAME SysV prototype as libc_syscall:
    ;   (uint64_t num, a1, a2, a3, a4, a5) → rdi, rsi, rdx, rcx, r8, r9
    ; All args are already in exactly the right registers — no shuffle needed.
    ; Tail call: syscall_dispatch returns directly to our caller.
    ; ----------------------------------------------------------------
    jmp     syscall_dispatch

section .bss
syscall_user_rsp: resq 1
