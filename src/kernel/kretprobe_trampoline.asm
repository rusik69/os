bits 64
section .text

; ── Kretprobes Return Trampolines (Items 204/373) ─────────────────────
;
; These trampolines are jumped to when a kretprobed function returns.
; Each trampoline stub passes its instance index to the C handler,
; which looks up the original return address and calls the user's
; kretprobe handler.
;
; Stack at entry (after function's `ret` pops our trampoline address):
;   [rsp]       = whatever was on stack before CALL (args, etc.)
;   RAX         = function's return value
;
; We save RAX, call the C handler, then jump to the original return
; address.  The function's return value is preserved in RAX for the
; original caller.
;
; ── Kretprobe instance table (defined in kprobes.c) ─────────────────
;
; struct kretprobe_instance {
;     uint64_t ret_addr;    /* original return address */
;     struct kretprobe *rp; /* back-pointer */
;     int in_use;
; };
;
; extern struct kretprobe_instance kretprobe_instances[KRETPROBE_MAX_INSTANCES];

extern kretprobe_trampoline_handler

; ── Generate KRETPROBE_MAX_INSTANCES trampoline stubs ─────────────

%define KRETPROBE_MAX_INSTANCES 64

; Each trampoline stub does:
;   push rax           ; save return value (1 byte: 50)
;   mov edi, N         ; instance index (5 bytes: BF NN 00 00 00)
;   lea rsi, [rsp+0]   ; pointer to saved RAX (4 bytes: 48 8D 74 24 00)
;   call handler       ; kretprobe_trampoline_handler(instance_id, &rax)
;   mov rcx, rax       ; save original return address (3 bytes: 48 89 C1)
;   pop rax            ; restore return value (1 byte: 58)
;   jmp rcx            ; tail-call to original return address (2 bytes: FF E1)
;
; Total: 1 + 5 + 4 + 5 + 3 + 1 + 2 = 21 bytes.  Align to 32 bytes.

; Common handler entry (called by each stub)
kretprobe_common_entry:

; Trampoline stubs
%assign i 0
%rep KRETPROBE_MAX_INSTANCES
global kretprobe_trampoline_%+i
kretprobe_trampoline_%+i:
    push rax                    ; save return value (1 byte)
    mov edi, i                  ; instance index (5 bytes)
    lea rsi, [rsp]              ; pointer to saved RAX (4 bytes)
    call kretprobe_trampoline_handler  ; (5 bytes)
    mov rcx, rax                ; save original return address (3 bytes)
    pop rax                     ; restore return value (1 byte)
    jmp rcx                     ; tail-call to original return addr (2 bytes)
    ; Pad to 32 bytes
    times (32 - ($ - kretprobe_trampoline_%+i)) nop
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
