; kpti_trampoline.asm — KPTI syscall entry/exit trampoline (FLAT BINARY)
;
; Assembled with: nasm -f bin -o kpti_trampoline.bin kpti_trampoline.asm
; Embedded into kernel via xxd -i and copied to trampoline page at boot.
;
; This code runs at KPTI_TRAMPOLINE_VADDR (0x7FFFFFFE0000).
; The `org` directive makes NASM generate correct absolute addresses.
; The physical page is mapped supervisor-only in both kernel and user PML4s.
;
; On SYSCALL entry (from user mode):
;   RCX = user RIP, R11 = user RFLAGS, RSP = user stack
;   RAX = syscall number, RDI..R8 = args (Linux ABI)
;
; After saving state and switching CR3 to kernel PML4, we jump
; to the real syscall handler. The real handler returns by jumping
; back to the exit trampoline, which switches CR3 to user PML4
; and does SYSRET.

%define KPTI_TRAMP_VADDR   0x00007FFFFFFE0000
%define KPTI_OFF_ENTRY     0x000
%define KPTI_OFF_EXIT      0x080
%define KPTI_OFF_CR3_KERN  0x100
%define KPTI_OFF_CR3_USER  0x108
%define KPTI_OFF_SAVE_RSP  0x110
%define KPTI_OFF_SAVE_RIP  0x118
%define KPTI_OFF_SAVE_RFL  0x120
%define KPTI_OFF_EXIT_RIP  0x128

org KPTI_TRAMP_VADDR
bits 64

; ============================================================================
; syscall_entry_trampoline — at KPTI_OFF_ENTRY (0x000)
; ============================================================================
; Saves user state, switches to kernel page table, jumps to real handler.

global syscall_entry_trampoline
syscall_entry_trampoline:

    ; Save user RSP, RIP (RCX), RFLAGS (R11)
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RSP], rsp
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RIP], rcx
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RFL], r11

    ; Load kernel CR3 value from trampoline data area
    mov     rax, [KPTI_TRAMP_VADDR + KPTI_OFF_CR3_KERN]
    mov     cr3, rax

    ; We're now in kernel page table mode.
    ; Jump to the real syscall handler at kernel VMA.
    ; The handler address was patched in by kpti_init().
    mov     rax, [KPTI_TRAMP_VADDR + KPTI_OFF_EXIT_RIP]
    jmp     rax

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
; We save the return state, switch to user CR3, then SYSRET.

global syscall_exit_trampoline
syscall_exit_trampoline:

    ; Save the return values
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RIP], rcx
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RFL], r11
    mov     [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RSP], rsp

    ; Switch to user page table
    mov     rax, [KPTI_TRAMP_VADDR + KPTI_OFF_CR3_USER]
    mov     cr3, rax

    ; Restore user state
    mov     rcx, [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RIP]
    mov     r11, [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RFL]
    mov     rsp, [KPTI_TRAMP_VADDR + KPTI_OFF_SAVE_RSP]

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
