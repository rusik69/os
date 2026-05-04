bits 64
section .text

; void context_switch(struct cpu_context **old, struct cpu_context *new_ctx)
; rdi = &old->context (pointer to pointer)
; rsi = new->context
;
; IMPORTANT: caller must disable interrupts (cli) before calling this
; function and re-enable them (sti) after it returns.  A timer interrupt
; between register restore and ret would re-enter schedule() and corrupt
; the partially-restored context frame.
global context_switch
context_switch:
    ; Save callee-saved registers
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current stack pointer to old context
    mov [rdi], rsp

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
; Re-enables interrupts then jumps to the entry function.
global process_entry_trampoline
process_entry_trampoline:
    sti
    jmp r15

; ──────────────────────────────────────────────────────────────────
; User-mode entry trampoline for ring 3 processes.
; context_switch returns here with:
;   r15 = user RIP (entry point)
;   r14 = user RSP (user stack top)
;
; We perform an iretq to transition to ring 3:
;   Push: SS(0x23), RSP(user), RFLAGS(IF=1), CS(0x1B), RIP(user)
; ──────────────────────────────────────────────────────────────────
global user_entry_trampoline
user_entry_trampoline:
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
    push 0x23             ; SS = user data selector (0x20 | RPL 3)
    push r14              ; RSP = user stack pointer
    push 0x202            ; RFLAGS = IF=1 (interrupts enabled)
    push 0x1B             ; CS = user code selector (0x18 | RPL 3)
    push r15              ; RIP = user entry point

    ; Clear remaining registers
    xor r14, r14
    xor r15, r15

    iretq
