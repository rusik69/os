; boot.asm - Multiboot1 entry point, sets up long mode and jumps to kernel_main
bits 32

; Multiboot1 constants
MULTIBOOT_MAGIC     equ 0x1BADB002
MULTIBOOT_FLAGS     equ 0x00010807  ; align + memory map + video mode + VBE mode + address fields
MULTIBOOT_CHECKSUM  equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

extern _kernel_end

section .multiboot2
align 4
multiboot_header:
    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM
    dd multiboot_header          ; header_addr
    dd 0x100000                  ; load_addr (start of .multiboot2)
    dd 0                         ; load_end_addr (0 = load entire file)
    dd 0                         ; bss_end_addr (0 = no BSS clearing by loader)
    dd _start                    ; entry_addr
    dd 0                         ; mode_type: linear graphics
    dd 1024                      ; width
    dd 768                       ; height
    dd 32                        ; depth

section .boot
align 4

; Page tables - identity map first 1GB and map higher-half
; These are in .boot so they're at low physical addresses
align 4096
boot_pml4:
    times 512 dq 0

align 4096
boot_pdpt:
    times 512 dq 0

align 4096
boot_pd:
    times 512 dq 0

align 4096
boot_pd2:
    times 512 dq 0

; Bootstrap stack
align 16
boot_stack_bottom:
    times 16384 db 0
boot_stack_top:

; GDT for 64-bit mode
align 16
gdt64:
    dq 0                            ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; code segment: executable, code/data, present, 64-bit
.data: equ $ - gdt64
    dq (1<<44) | (1<<47) | (1<<41)  ; data segment: code/data, present, writable
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

global _start
_start:
    ; Save multiboot info pointer and magic
    mov edi, eax                    ; multiboot2 magic
    mov esi, ebx                    ; multiboot2 info struct pointer

    ; Set up stack
    mov esp, boot_stack_top

    ; Set up page tables
    ; PML4[0] -> boot_pdpt (identity map for low addresses)
    mov eax, boot_pdpt
    or eax, 0x03                    ; present + writable
    mov [boot_pml4], eax

    ; PML4[256] -> boot_pdpt (high-half kernel at 0xFFFF800000000000)
    ; Same PDPT: physical 0x0..0x3FFFFFFF maps to both 0x0 and 0xFFFF800000000000
    mov [boot_pml4 + 256 * 8], eax

    ; boot_pdpt[0] -> boot_pd  (identity: 0x00000000 - 0x3FFFFFFF)
    mov eax, boot_pd
    or eax, 0x03
    mov [boot_pdpt], eax

    ; boot_pdpt[3] -> boot_pd2 (identity: 0xC0000000 - 0xFFFFFFFF, covers PCI MMIO)
    mov eax, boot_pd2
    or eax, 0x03
    mov [boot_pdpt + 3 * 8], eax

    ; Fill boot_pd with 2MB pages (identity map 0x0 - 0x3FFFFFFF)
    mov ecx, 0
.fill_pd:
    mov eax, ecx
    shl eax, 21                     ; 2MB per entry
    or eax, 0x83                    ; present + writable + huge page
    mov [boot_pd + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .fill_pd

    ; Fill boot_pd2 with 2MB pages (identity map 0xC0000000 - 0xFFFFFFFF)
    ; Entry N maps physical 0xC0000000 + N*2MB
    mov ecx, 0
.fill_pd2:
    mov eax, ecx
    shl eax, 21                     ; N * 2MB
    add eax, 0xC0000000             ; base at 3GB
    or eax, 0x83                    ; present + writable + huge page
    mov [boot_pd2 + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .fill_pd2

    ; Load PML4 into CR3
    mov eax, boot_pml4
    mov cr3, eax

    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)               ; CR4.PAE
    mov cr4, eax

    ; Enable long mode
    mov ecx, 0xC0000080             ; IA32_EFER MSR
    rdmsr
    or eax, (1 << 8)               ; LME (Long Mode Enable)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)              ; CR0.PG
    mov cr0, eax

    ; Load 64-bit GDT and far jump to 64-bit code
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_entry

bits 64
long_mode_entry:
    ; Reload data segments
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack
    mov rsp, boot_stack_top

    ; Save multiboot magic and info before BSS clear
    mov r12d, edi
    mov r13d, esi

    ; Clear direction flag
    cld

    ; Zero out BSS
    extern _bss_start, _bss_end
    mov rdi, _bss_start
    mov rcx, _bss_end
    sub rcx, rdi
    shr rcx, 3
    xor rax, rax
    rep stosq

    ; Prepare arguments for kernel_main
    mov edi, r12d
    mov esi, r13d

    extern kernel_main
    mov rax, kernel_main            ; absolute call - kernel_main is at high VMA
    call rax

.halt:
    cli
    hlt
    jmp .halt
