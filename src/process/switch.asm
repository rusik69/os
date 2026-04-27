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
