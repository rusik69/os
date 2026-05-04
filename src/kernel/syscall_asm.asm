bits 64

; Syscall entry point: set as LSTAR MSR
; On syscall: RCX=saved RIP, R11=saved RFLAGS, RSP is still user RSP
; Args: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5
;
; We need to switch to the kernel stack before doing anything.
; The kernel stack (RSP0) is stored in the TSS. Since we can't access TSS
; directly without trashing a register, we use a per-CPU scratch area.
; Simpler approach: we store kernel RSP in a known location.
global syscall_entry
extern syscall_dispatch

section .data
global syscall_kernel_rsp
syscall_kernel_rsp: dq 0   ; Set by scheduler on context switch

section .text
syscall_entry:
    ; Save user RSP, load kernel RSP
    ; We use r11 saved by CPU (RFLAGS) — but we also need scratch space.
    ; On syscall: RCX = user RIP, R11 = user RFLAGS
    ; User RSP is still in RSP. We swap it with kernel RSP.

    ; Save user RSP to r11-scratch via xchg with memory
    ; Actually, let's use the swapgs-free approach: save user rsp, load kernel rsp
    mov     [rel syscall_user_rsp], rsp
    mov     rsp, [rel syscall_kernel_rsp]

    ; Now on kernel stack. Push the user state.
    push    qword [rel syscall_user_rsp]  ; saved user RSP
    push    rcx                            ; saved user RIP
    push    r11                            ; saved user RFLAGS

    ; Save callee-saved registers that C might clobber
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    ; Linux syscall calling convention: arg4 in R10, not RCX
    mov     rcx, r10

    ; Call C dispatcher: syscall_dispatch(num=rax, a1=rdi, a2=rsi, a3=rdx, a4=rcx, a5=r8)
    ; rax already has syscall number, rdi/rsi/rdx are already arg1-3
    ; rcx now holds r10 (arg4), r8 is arg5
    push    r8                             ; 6th arg on stack (if needed)
    push    rcx                            ; 5th arg ... actually SysV passes in regs
    ; SysV ABI: rdi, rsi, rdx, rcx, r8, r9
    ; Our dispatch proto: (rax, rdi, rsi, rdx, rcx, r8)
    ; Shuffle: move syscall# (rax) to rdi, shift others
    mov     r9, r8          ; a5
    mov     r8, rcx         ; a4
    mov     rcx, rdx        ; a3
    mov     rdx, rsi        ; a2
    mov     rsi, rdi        ; a1
    mov     rdi, rax        ; num
    pop     rax             ; discard pushed rcx
    pop     rax             ; discard pushed r8
    call    syscall_dispatch

    ; rax = return value

    ; Restore callee-saved registers
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp

    ; Restore user state
    pop     r11             ; user RFLAGS
    pop     rcx             ; user RIP
    pop     rsp             ; user RSP

    ; Return to user mode (restores RIP from RCX, RFLAGS from R11)
    o64 sysret

section .bss
syscall_user_rsp: resq 1
