; ap_trampoline.asm — Application Processor startup trampoline
;
; This code is copied to physical address 0x7000 (28 KB).
; BSP sends INIT-SIPI-SIPI to wake APs, which start executing here
; in 16-bit real mode.
;
; Layout:
;   0x7000: 16-bit entry point (real mode)
;   0x7100: GDT for trampoline
;   0x7200: Page table pointers
;   0x7300: AP entry 64-bit C function pointer

bits 16
section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_entry_64

; These are filled in by the BSP before sending SIPI
extern ap_entry_c

; ── Entry point (real mode, CS:IP = 0x7000:0x0000) ──────────────
ap_trampoline_start:
    cli
    cld

    ; Set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x6000               ; temporary stack (below trampoline)

    ; Load temporary GDT for protected mode
    lgdt [cs:gdt_temp_ptr - ap_trampoline_start + 0x7000]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to flush prefetch and enter protected mode
    jmp dword 0x08:(pmode_entry - ap_trampoline_start + 0x7000)

bits 32
pmode_entry:
    ; Set up segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov ax, 0x00                 ; FS/GS = null
    mov fs, ax
    mov gs, ax

    ; Load BSP's page tables address (stored at 0x7200 by BSP)
    mov eax, [0x7200]
    mov cr3, eax

    ; Enable PAE + SMEP + SMAP + UMIP
    mov eax, cr4
    or eax, (1 << 5) | (1 << 20) | (1 << 21) | (1 << 11)  ; PAE | SMEP | SMAP | UMIP
    mov cr4, eax

    ; Enable long mode + NXE
    mov ecx, 0xC0000080          ; IA32_EFER MSR
    rdmsr
    or eax, (1 << 8) | (1 << 11) ; LME | NXE
    wrmsr

    ; Enable paging (PG) and write-protect (WP)
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16); PG | WP
    mov cr0, eax

    ; Far jump to 64-bit mode
    jmp dword 0x08:(long_mode_entry - ap_trampoline_start + 0x7000)

bits 64
long_mode_entry:
    ; Reload data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Load stack from info at 0x7300 (BSP wrote per-CPU stack top)
    mov rsp, [0x7300]

    ; The C entry function pointer is at 0x7308
    mov rax, [0x7308]
    call rax

.halt:
    cli
    hlt
    jmp .halt

; ── Temporary GDT for trampoline ──────────────────────────────
align 16
gdt_temp:
    dq 0                         ; null descriptor
.code: equ $ - gdt_temp
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; 32-bit + 64-bit code
.data: equ $ - gdt_temp
    dq (1<<44) | (1<<47) | (1<<41) ; writable data
gdt_temp_end:

gdt_temp_ptr:
    dw gdt_temp_end - gdt_temp - 1
    dd 0x7000 + gdt_temp - ap_trampoline_start

ap_trampoline_end:

; ── 64-bit entry point used by BSP to store AP trampoline info ──
; Not actually executed as trampoline code; this label is referenced
; from C so we can obtain its address.
ap_entry_64:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
