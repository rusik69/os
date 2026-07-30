bits 64
section .text

extern kpti_user_entry_trace

; ───────────────────────────────────────────────────────────────────────────────
; CONTEXT SWITCH — Save old task context, restore new task context
;
; C prototype: void context_switch(struct cpu_context **old, struct cpu_context *new_ctx)
;   rdi = &old->context  (pointer-to-pointer to save old RSP into; may be NULL)
;   rsi = new->context   (the target task's saved RSP to restore)
;
; CALLER REQUIREMENTS
; ────────────────────
; 1. IRQs must be disabled (cli) BEFORE calling context_switch() and re-enabled
;    (sti) AFTER it returns.  A timer interrupt between register restore and
;    the final ret would re-enter schedule() and corrupt the partially-restored
;    context frame, leading to a crash or privilege escalation.
; 2. The caller must hold any necessary locks (e.g. sched_lock) or have them
;    dropped before the actual switch, depending on the locking protocol.
;
; STACK LAYOUT (after saving, before restore)
; ────────────
; After pushing callee-saved registers, the outgoing task's stack looks like
; this (from low to high addresses, RSP points at the lowest):
;
;   [r15]           ← RSP (saved into *old)
;   [r14]
;   [r13]
;   [r12]
;   [rbx]
;   [rbp]
;   [return addr]   ← where context_switch returns to (in schedule())
;
; The incoming task's stack must have the same layout so that the restore
; pops the same register order.  This is established by:
;   - process_init(): pushes 6 slots + a return address (process_entry_trampoline)
;     for newly created kernel tasks.
;   - fork/exec: copies the parent's stack frame so the child restores the
;     same registers.
;
; FLOW
; ────
;   1. If old != NULL: push callee-saved regs (rbp, rbx, r12-r15) onto the
;      current stack, then save RSP into *old.  The saved RSP value points
;      at the top of the 6-register save area.
;   2. If old == NULL (e.g. first switch on AP): skip save entirely; no
;      outgoing context to preserve.
;   3. Restore: load RSP from new_ctx (the incoming task's saved RSP), which
;      points at the top of a 6-register save area.
;   4. Pop callee-saved regs in reverse order (r15, r14, r13, r12, rbx, rbp).
;   5. ret — pops the return address from the stack.  For a running task being
;      resumed, this returns to wherever schedule() was at the time of the
;      original switch-out.  For a new task, it returns to the trampoline
;      (process_entry_trampoline or user_entry_trampoline).
;
; NOTES
; ─────
; - CR3 (page tables) is NOT switched here — the caller (schedule()) switches
;   page tables explicitly before calling context_switch(), because the
;   outgoing task's userspace may need to be accessed (e.g. for rseq) before
;   the switch.
; - KPTI CR3 patching is also handled by the caller, not here.
; - This function does not touch debug registers, FPU/SSE state, or LBR MSRs;
;   those are saved/restored by the caller.
; ───────────────────────────────────────────────────────────────────────────────
global context_switch
context_switch:
    ; Check if old context pointer is NULL (e.g. AP with no current process)
    test rdi, rdi
    jz .skip_save

    ; Save callee-saved registers
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current stack pointer to old context
    mov [rdi], rsp
    jmp .restore

.skip_save:
    ; No old context to save — just advance past the 6 saved-reg slots
    ; on the NEW stack so we can restore from the same layout
    ; (the new context was pushed as: rbp, rbx, r12, r13, r14, r15, ret_addr)
    ; so there is nothing to discard — we go straight to restore.

.restore:
    ; Load new stack pointer
    mov rsp, rsi

    ; Restore callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return to new process (rip was on the stack)
    ret

; Trampoline for newly created processes.
; context_switch returns here; the real entry point is in r15.
; Re-enables interrupts then CALLs the entry function so that a proper
; return address is on the stack for lockdep/frame-pointer tracking.
; If the entry function ever returns (it shouldn't), spin forever.
global process_entry_trampoline
process_entry_trampoline:
    sti
    call r15
    cli
    hlt

; ──────────────────────────────────────────────────────────────────
; User-mode entry trampoline for ring 3 processes.
; context_switch returns here with:
;   r15 = user RIP (entry point)
;   r14 = user RSP (user stack top)
;
; We perform an iretq to transition to ring 3:
;   Push: SS(0x1B), RSP(user), RFLAGS(IF=1), CS(0x23), RIP(user)
; ──────────────────────────────────────────────────────────────────
global user_entry_trampoline
user_entry_trampoline:
    ; Debug: trace entry into ring 3 (r15=entry, r14=user_rsp)
    push r15
    push r14
    mov rdi, r15          ; arg1 = entry point
    mov rsi, r14          ; arg2 = user RSP
    call kpti_user_entry_trace
    pop r14
    pop r15

    ; Zero general-purpose registers to avoid leaking kernel data
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    mov rbp, r14          ; set rbp = user stack for reference

    ; Build iretq frame on kernel stack
    push 0x1B             ; SS = user data selector (0x18 | RPL 3)
    push r14              ; RSP = user stack pointer
    push 0x202            ; RFLAGS = IF=1 (interrupts enabled)
    push 0x23             ; CS = user code selector (0x20 | RPL 3)
    push r15              ; RIP = user entry point

    ; Clear remaining registers
    xor r14, r14
    xor r15, r15

    iretq

; ──────────────────────────────────────────────────────────────────
; Fork child trampoline.
; Called after context_switch to start a forked process.
; context_switch already popped the 6 callee-saved registers
; (r15, r14, r13, r12, rbx, rbp), so RSP now points at user RFLAGS.
; Stack layout:
;   [user RFLAGS]   → pop into r11  (for sysret)
;   [user RIP]       → pop into rcx  (for sysret)
;   [user RSP]       → pop into rsp  (for sysret)
;
; If KPTI is active, we must switch to the user page table before
; returning to userspace, just like the KPTI syscall exit trampoline.
; ──────────────────────────────────────────────────────────────────
extern kpti_active_flag
global fork_child_trampoline
fork_child_trampoline:
    xor rax, rax           ; fork return value = 0 (child)
    pop r11                ; user RFLAGS
    pop rcx                ; user RIP
    pop rsp                ; user RSP
    cmp qword [rel kpti_active_flag], 0
    je .sysret
    ; Load user CR3 from per-CPU KPTI trampoline page
    mov r15, 0x00007FFFFFFE0108  ; CPU 0: KPTI_TRAMP_VADDR + KPTI_OFF_CR3_USER
    mov r15, [r15]
    mov cr3, r15
.sysret:
    o64 sysret

; ──────────────────────────────────────────────────────────────────
; Clone child trampoline.
; Called after context_switch to start a cloned thread.
; Stack layout (from low to high after context_switch returns):
;   [junk r15] ← RSP
;   [junk r14]
;   [junk r13]
;   [junk r12]
;   [junk rbx]
;   [junk rbp]
;   [user RFLAGS]   → pop into r11  (for sysret)
;   [user RIP]       → pop into rcx  (for sysret)
;   [user RSP]       → pop into rsp  (for sysret)
; ──────────────────────────────────────────────────────────────────
global clone_child_trampoline
clone_child_trampoline:
    xor rax, rax           ; clone return value = 0 (child)
    ; Skip the 6 junk callee-saved register slots
    add rsp, 48            ; skip r15, r14, r13, r12, rbx, rbp
    pop r11                ; user RFLAGS
    pop rcx                ; user RIP
    pop rsp                ; user RSP
    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
