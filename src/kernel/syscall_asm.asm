bits 64

; Syscall entry point: set as LSTAR MSR
; On syscall: RCX=saved RIP, R11=saved RFLAGS
; Args: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5
global syscall_entry
extern syscall_dispatch

syscall_entry:
    ; Save caller RCX (user RIP) and R11 (user RFLAGS)
    push    rcx
    push    r11

    ; Save callee-saved registers that C might clobber
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    ; Linux syscall calling convention: arg4 in R10, not RCX
    mov     rcx, r10

    ; Call C dispatcher: syscall_dispatch(num=rax, a1=rdi, a2=rsi, a3=rdx, a4=r10, a5=r8)
    call    syscall_dispatch

    ; Restore callee-saved registers
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp

    ; Restore user RIP and RFLAGS
    pop     r11
    pop     rcx

    ; Return to user mode (restores RIP from RCX, RFLAGS from R11)
    sysretq
