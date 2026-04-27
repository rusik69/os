bits 64
section .text

global gdt_load
global tss_load

; void gdt_load(struct gdt_pointer *ptr, uint16_t code_seg, uint16_t data_seg)
; rdi = ptr, rsi = code_seg, rdx = data_seg
gdt_load:
    lgdt [rdi]

    ; Reload data segments
    mov ax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far return to reload CS
    pop rax                    ; return address
    push rsi                   ; code segment
    push rax                   ; return address
    retfq                      ; far return

; void tss_load(uint16_t selector)
; rdi = selector
tss_load:
    mov ax, di
    ltr ax
    ret
