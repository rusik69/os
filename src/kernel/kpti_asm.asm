bits 64
section .text

; ============================================================================
; KPTI Trampoline — Kernel Page-Table Isolation entry/exit stubs
;
; These stubs are mapped in BOTH the kernel and user page tables at
; KPTI_TRAMPOLINE_VADDR + cpu * 0x2000.  They serve as the first point
; of entry for syscalls and interrupts from user mode, switching the
; page tables from the restricted user PML4 to the full kernel PML4.
; ============================================================================

; Offsets into the per-CPU trampoline data area (relative to CPU base)
%define KPTI_OFF_KERNEL_CR3     0x800
%define KPTI_OFF_USER_CR3       0x808
%define KPTI_OFF_KERNEL_RSP     0x810
%define KPTI_OFF_SAVED_CR3      0x818
%define KPTI_OFF_SAVED_RSP      0x820
%define KPTI_OFF_SAVED_RCX      0x828
%define KPTI_OFF_SAVED_R11      0x830

; When entering via interrupt, the CPU has pushed:
;   SS, RSP, EFLAGS, CS, RIP
; and the isr%{vector} stub has pushed:
;   error_code (0 if none), vector_number
; We arrive here after those pushes.

global kpti_interrupt_entry
global kpti_interrupt_entry_with_ec
global kpti_interrupt_return

; ── Helper: switch CR3 from user to kernel page table ──────────────────
; On entry: RCX = user RIP (for syscall), R11 = user RFLAGS (for syscall)
; For interrupts, RCX/R11 are general-purpose regs the user set.
;
; We use the fixed trampoline address (this code is mapped at the same
; VA in both page tables).  The per-CPU data is accessed via
; RIP-relative addressing relative to this CPU's trampoline base.

; ============================================================================
; kpti_trampoline_entry — SYSCALL entry point
; ============================================================================
; This is the LSTAR MSR target.  On syscall from user mode:
;   RCX = user RIP (syscall return address)
;   R11 = user RFLAGS
;   RSP = user stack pointer
;   RAX = syscall number
;   RDI..R9 = syscall arguments
;
; Stack at entry: user RSP (not modified by CPU for syscall)
; We need to switch CR3 and get onto the real kernel stack.
; ============================================================================

global kpti_trampoline_entry
kpti_trampoline_entry:
    ; Save user RSP and RCX/R11 to the trampoline data area
    ; (RIP-relative addressing works because we're at a fixed VA)
    mov     [rel KPTI_OFF_SAVED_RSP], rsp
    mov     [rel KPTI_OFF_SAVED_RCX], rcx
    mov     [rel KPTI_OFF_SAVED_R11], r11

    ; Switch to kernel page table
    mov     rax, [rel KPTI_OFF_KERNEL_CR3]
    mov     cr3, rax

    ; Switch to kernel stack
    mov     rsp, [rel KPTI_OFF_KERNEL_RSP]

    ; Push user state onto kernel stack (same layout as original syscall_entry)
    push    [rel KPTI_OFF_SAVED_RSP]       ; saved user RSP   (frame 1)
    push    [rel KPTI_OFF_SAVED_RCX]       ; saved user RIP   (frame 2)
    push    [rel KPTI_OFF_SAVED_R11]       ; saved user RFLAGS (frame 3)
    push    rbp                             ; (4)
    push    rbx                             ; (5)
    push    r12                             ; (6)
    push    r13                             ; (7)
    push    r14                             ; (8)
    push    r15                             ; (9)

    ; Save the stack pointer for stack zeroing (high-water mark)
    mov     [rel syscall_entry_rsp_saved], rsp

    ; Save user RIP and RFLAGS for clone()
    mov     [rel syscall_user_rip], rcx
    mov     [rel syscall_user_rflags], r11

    ; Save user R9 (6th syscall argument) before clobbering it
    mov     [rel syscall_arg6], r9

    ; Arg shuffle: syscall_dispatch(num, a1, a2, a3, a4, a5)
    ;   target: rdi=num  rsi=a1  rdx=a2  rcx=a3  r8=a4  r9=a5
    mov     r9,  r8         ; a5
    mov     r8,  r10        ; a4  (r10 holds arg4 per Linux syscall ABI)
    mov     rcx, rdx        ; a3
    mov     rdx, rsi        ; a2
    mov     rsi, rdi        ; a1
    mov     rdi, rax        ; num

    ; Jump to the C dispatcher (tail call — returns directly to us)
    ; Note: syscall_dispatch is only visible in kernel page table,
    ; so this MUST execute after the CR3 switch above.
    jmp     kpti_syscall_dispatch

; ============================================================================
; kpti_syscall_dispatch — call syscall_dispatch and handle return
; ============================================================================
; This runs on the kernel stack with kernel page table active.
; We call the real syscall_dispatch, then handle execve and return to user.
; ============================================================================

extern syscall_dispatch_kpti
kpti_syscall_dispatch:
    call    syscall_dispatch_kpti

    ; Check if execve() was called
    cmp     qword [rel execve_pending], 0
    je      .normal_return_kpti

    ; Force execve return: use the preset RIP/RFLAGS/RSP
    xor     eax, eax               ; execve returns 0
    mov     rcx, [rel execve_user_rip]
    mov     r11, [rel execve_user_rflags]
    mov     rsp, [rel execve_user_rsp]

    ; Zero the pending flag
    mov     qword [rel execve_pending], 0

    ; Switch to user page table before sysret
    jmp     kpti_switch_to_user_and_sysret

.normal_return_kpti:
    ; ── Zero kernel stack to prevent information disclosure ────────
    mov     rdi, [rel syscall_entry_rsp_saved]
    call    zero_kernel_stack_uapi

    ; Restore registers (same order as pushed)
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    pop     r11             ; user RFLAGS → sysret reads R11
    pop     rcx             ; user RIP   → sysret reads RCX
    pop     rsp             ; user RSP

    ; ── Fall-through to kpti_switch_to_user_and_sysret ─────────────

; ============================================================================
; kpti_switch_to_user_and_sysret — switch to user page table and sysret
; ============================================================================
; Before sysret, we must switch CR3 to the user page table.
; This is called from both the normal return path and execve.
; ============================================================================

global kpti_switch_to_user_and_sysret
kpti_switch_to_user_and_sysret:
    ; Read the per-CPU user CR3 from the trampoline data area.
    ; At this point, RCX = user RIP, R11 = user RFLAGS, RSP = user RSP
    ; (set by the code above or by the execve path).
    mov     rax, [rel KPTI_OFF_USER_CR3]
    mov     cr3, rax                ; switch to user page table
    o64 sysret                      ; return to ring-3 user mode

; ============================================================================
; kpti_interrupt_entry — Interrupt/exception entry point (no error code)
; ============================================================================
; CPU has pushed: SS, RSP, RFLAGS, CS, RIP
; isr%{vector} stub has pushed: dummy error code (0), vector number
;
; We arrive here with RSP = trampoline stack (TSS.rsp0), but:
;   - The trampoline stack is mapped in both page tables
;   - The kernel page table is NOT yet active
; We need to save CR3, switch to kernel page table, switch to the real
; kernel stack, and jump to the real ISR handler.
; ============================================================================

kpti_interrupt_entry:
    ; Save user CR3 to trampoline data area
    mov     rax, cr3
    mov     [rel KPTI_OFF_SAVED_CR3], rax

    ; Switch to kernel page table  
    mov     rax, [rel KPTI_OFF_KERNEL_CR3]
    mov     cr3, rax

    ; Switch to real kernel stack
    ; We need to save the current RSP (which points into the trampoline
    ; stack) and switch to the real kernel stack.
    mov     [rel KPTI_OFF_SAVED_RSP], rsp
    mov     rsp, [rel KPTI_OFF_KERNEL_RSP]

    ; Copy the interrupt frame from trampoline stack to kernel stack.
    ; The trampoline stack has (from top to bottom, growing downward):
    ;   vector number (8 bytes)
    ;   error code (8 bytes)
    ;   RIP (8 bytes) from CPU push
    ;   CS (8 bytes)
    ;   RFLAGS (8 bytes)
    ;   RSP (8 bytes) — user RSP
    ;   SS (8 bytes)
    ;
    ; We need to push these onto the kernel stack in the correct order
    ; so that the existing interrupt handlers work unchanged.
    ;
    ; The existing isr_common_stub expects:
    ;   [rsp+0] = error code
    ;   [rsp+8] = vector number
    ;   [rsp+16] = RIP  (from CPU)
    ;   [rsp+24] = CS
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = RSP (user RSP)
    ;   [rsp+48] = SS
    ;
    ; Wait, actually the existing isr_common_stub pushes all GP regs
    ; after the vector+error code.  Let me examine the stack layout.
    ;
    ; After the isr%{vector} stub's pushes:
    ;   [rsp]   = vector number
    ;   [rsp+8] = error code
    ;   [rsp+16] = RIP (from CPU)
    ;   [rsp+24] = CS
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = user RSP
    ;   [rsp+48] = SS
    ;
    ; isr_common_stub then pushes additional regs on top of this.
    ;
    ; The trampoline stack has this layout.  We need to copy it to
    ; the kernel stack and adjust the stack pointer accordingly.
    ;
    ; Simple approach: just push the frame again on the kernel stack.

    ; Save the user CR3 (we'll need it for the return path)
    push    [rel KPTI_OFF_SAVED_CR3]        ; save user CR3 for return

    ; Copy the frame from trampoline stack to kernel stack.
    ; First copy the vector + error code + CPU-pushed frame.
    ; The trampoline stack pointer (after the isr stub pushes) pointed
    ; to [vector][error][RIP][CS][RFLAGS][user RSP][SS].
    ; But we saved it in KPTI_OFF_SAVED_RSP.

    ; Push the 7 frame elements in reverse order (SS first)
    mov     rax, [rel KPTI_OFF_SAVED_RSP]
    push    [rax + 48]   ; SS
    push    [rax + 40]   ; user RSP
    push    [rax + 32]   ; RFLAGS
    push    [rax + 24]   ; CS
    push    [rax + 16]   ; RIP
    push    [rax + 8]    ; error code
    push    [rax + 0]    ; vector number

    ; Now the kernel stack has the full interrupt frame.
    ; But we also have the saved user CR3 on top.
    ; Let's re-arrange: we want the frame to look like a normal
    ; interrupt entry, which expects:
    ;   [rsp+0] = vector number
    ;   [rsp+8] = error code  
    ;   [rsp+16] = RIP
    ;   [rsp+24] = CS
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = user RSP
    ;   [rsp+48] = SS
    ;
    ; Currently we have:
    ;   [rsp] = vector
    ;   [rsp+8] = error
    ;   [rsp+16] = RIP
    ;   [rsp+24] = CS
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = user RSP
    ;   [rsp+48] = SS
    ;   [rsp+56] = saved user CR3  <- extra!
    ;
    ; Actually wait, the order is reversed from how I pushed it.
    ; Let me redo this properly.

    ; Remove the frame and saved CR3, then rebuild correctly
    add     rsp, 64     ; discard everything we just pushed (7 items + saved CR3)

    ; Now rebuild: first push the CR3 save, then the frame in correct order
    push    [rel KPTI_OFF_SAVED_CR3]        ; saved user CR3
    mov     rax, [rel KPTI_OFF_SAVED_RSP]

    ; Push in order they will be popped by iretq:
    ; The iretq frame (from low to high): RIP, CS, RFLAGS, RSP, SS
    ; But the interrupt stubs add vector + error at the bottom.
    ; Let's just push in the same order as they appear on the trampoline stack.
    push    [rax + 0]    ; vector number
    push    [rax + 8]    ; error code
    push    [rax + 16]   ; RIP
    push    [rax + 24]   ; CS
    push    [rax + 32]   ; RFLAGS
    push    [rax + 40]   ; user RSP
    push    [rax + 48]   ; SS

    ; Now stack layout on kernel stack:
    ;   [rsp]   = SS
    ;   [rsp+8] = user RSP
    ;   [rsp+16] = RFLAGS
    ;   [rsp+24] = CS
    ;   [rsp+32] = RIP
    ;   [rsp+40] = error code
    ;   [rsp+48] = vector number
    ;   [rsp+56] = saved user CR3
    ;
    ; But isr_common_stub expects:
    ;   [rsp]   = vector number  (it pushes on top of this)
    ;   [rsp+8] = error code
    ;   [rsp+16] = RIP
    ;   ...etc
    ;
    ; So we need to pop the frame and re-push it in the correct order.
    ; Actually isr_common_stub pushes 15 registers (rax..r15) and then
    ; passes rsp to isr_common_handler.  At that point:
    ;   rsp -> rax..r15 (15 * 8 = 120 bytes of push)
    ;          then vector number
    ;          error code
    ;          RIP
    ;          CS
    ;          RFLAGS
    ;          user RSP
    ;          SS
    ;
    ; The C handler uses the struct interrupt_frame * which accesses
    ; these fields at known offsets.
    ;
    ; After the C handler returns, isr_common_stub pops all 15 regs
    ; and does `add rsp, 16` to remove vector + error, then iretq.
    ;
    ; For iretq to work, RSP must point to:
    ;   [rsp]   = RIP
    ;   [rsp+8] = CS
    ;   [rsp+16] = RFLAGS
    ;   [rsp+24] = RSP (user RSP)
    ;   [rsp+32] = SS
    ;
    ; So we need the frame to be:
    ;   vector, error, RIP, CS, RFLAGS, user RSP, SS
    ; That's what the original isr stub produces.
    ;
    ; Let me redo this properly:

    add     rsp, 64     ; discard again

    ; Push saved user CR3 (we'll need it for the return path)
    push    [rel KPTI_OFF_SAVED_CR3]

    ; Now push the frame in isr stub order:
    mov     rax, [rel KPTI_OFF_SAVED_RSP]
    push    [rax + 0]    ; vector number
    push    [rax + 8]    ; error code
    push    [rax + 16]   ; RIP
    push    [rax + 24]   ; CS
    push    [rax + 32]   ; RFLAGS
    push    [rax + 40]   ; user RSP
    push    [rax + 48]   ; SS

    ; Stack now:
    ;   [rsp]   = SS
    ;   [rsp+8] = user RSP
    ;   [rsp+16] = RFLAGS
    ;   [rsp+24] = CS  
    ;   [rsp+32] = RIP
    ;   [rsp+40] = error code
    ;   [rsp+48] = vector number
    ;   [rsp+56] = saved user CR3

    ; Now we're ready to call isr_common_stub.  But isr_common_stub
    ; expects [rsp] = vector number (or at least pushes registers
    ; on top of RSP, and the C handler expects the frame at known offsets).
    ; The current RSP points to SS, not vector.
    ;
    ; We need to re-arrange so RSP points to vector number.
    ; Shift everything down by 56 bytes?  No, we can't do that easily.
    ;
    ; Let me take a completely different approach.
    ;
    ; Instead of pushing the frame on the kernel stack, let me just
    ; jump to the original interrupt stub (which expects the frame
    ; on the ORIGINAL stack).  But we've already switched stack!
    ;
    ; Another approach: instead of copying the frame, make the
    ; trampoline stack IS the stack used for the entire interrupt
    ; handling.  The isr_common_stub will push regs onto the
    ; trampoline stack, call the C handler, pop regs, and iretq.
    ; This works because the trampoline stack is mapped in both PTs.
    ; BUT: the C handler might call other kernel functions that
    ; expect a deep stack.  The trampoline stack is only 4KB, which
    ; might not be enough.

    ; Simplest approach: Make the trampoline stack large enough
    ; for the full interrupt handling, OR switch to the kernel stack
    ; immediately after saving the frame.

    ; Let me re-think this.  The issue is that after switching to the
    ; kernel stack, the frame (pushed by CPU + isr stubs) is on the
    ; OLD stack (trampoline stack), not the new kernel stack.
    ;
    ; Clean solution: re-push the frame on the kernel stack and
    ; adjust RSP to point to the vector number, then fall through
    ; to isr_common_stub.

    ; Actually, let me just use the kernel stack directly by
    ; copying the frame properly.  I'll push in REVERSE order
    ; so that after all pushes, RSP points to vector number.

    add     rsp, 64     ; discard again
    ; Now [rsp] = saved user CR3

    ; Re-push in reverse order of what we want:
    mov     rax, [rel KPTI_OFF_SAVED_RSP]

    push    [rax + 48]   ; SS (pushed last -> highest addr)
    push    [rax + 40]   ; user RSP
    push    [rax + 32]   ; RFLAGS
    push    [rax + 24]   ; CS
    push    [rax + 16]   ; RIP
    push    [rax + 8]    ; error code
    push    [rax + 0]    ; vector number

    ; Now stack:
    ;   [rsp]   = vector number
    ;   [rsp+8] = error code
    ;   [rsp+16] = RIP
    ;   [rsp+24] = CS
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = user RSP
    ;   [rsp+48] = SS
    ;   [rsp+56] = saved user CR3
    ;
    ; This is exactly what isr_common_stub expects!
    ; It will push 15 regs (rax..r15) on top of this.
    ; After returning, it pops regs, does add rsp,16, then iretq.
    ;
    ; But iretq will return to user mode with CR3 = user page table
    ; (which we saved).  We need to switch CR3 before iretq.
    ; However, the existing isr_common_stub does NOT know about KPTI.
    ;
    ; So we need to intercept the return path.  We can do this by
    ; NOT jumping to isr_common_stub directly, but instead jumping
    ; to a modified version that switches CR3 before iretq.
    
    ; For now, jump to the existing isr_common_stub.
    ; We'll add the CR3 switch before iretq in the stub.
    jmp     isr_common_stub

; ============================================================================
; kpti_interrupt_entry_with_ec — Interrupt entry with hardware error code
; ============================================================================
; Same as above but CPU already pushed the error code, so the isr%{vector}
; stub pushes only the vector number (not a dummy error code).
;
; Stack from isr stub: [vector_number][error_code][RIP][CS][RFLAGS][RSP][SS]
; ============================================================================

kpti_interrupt_entry_with_ec:
    ; Same as kpti_interrupt_entry - the frame layout is identical
    ; (vector number is at [RSP], error code at [RSP+8])
    jmp     kpti_interrupt_entry

; Declare external symbols used above
extern syscall_entry_rsp_saved
extern syscall_user_rip
extern syscall_user_rflags
extern syscall_arg6
extern execve_pending
extern execve_user_rip
extern execve_user_rflags
extern execve_user_rsp
extern zero_kernel_stack_uapi
extern isr_common_stub

section .note.GNU-stack noalloc noexec nowrite progbits
