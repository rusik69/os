; boot.asm — x86-64 kernel boot entry point
;
; BOOT FLOW OVERVIEW
; ─────────────────
; This file implements the full x86-64 bootstrap sequence:
;
; 1. MULTIBOOT HEADER (16-bit/32-bit compat)
;    The .multiboot section contains a Multiboot1-compliant header at the start
;    of the kernel image. The bootloader (GRUB, QEMU -kernel, etc.) reads this
;    header to determine load address, alignment, and entry point. The header
;    specifies load_addr=0x100000 (1MB), which is the conventional x86 kernel
;    load point above the 640KB-1MB region reserved for BIOS/ROM.
;
; 2. 32-BIT STARTUP (_start)
;    Execution begins in 32-bit protected mode at _start. The bootloader has
;    loaded the kernel to 0x100000 and passed a multiboot info structure in
;    ebx (with magic 0x2BADB002 in eax). The entry routine:
;    - Saves the multiboot info pointer and magic value
;    - Sets up a bootstrap stack (128KB in .boot section)
;    - Builds page tables for identity-mapping the first 1GB (0x0-0x3FFFFFFF)
;      and a second identity map for high PCI MMIO (0xC0000000-0xFFFFFFFF)
;    - Sets up the higher-half recursive map: PML4[256] → same PDPT → kernel
;      addresses at 0xFFFF800000000000+ map to physical 0x0-0x3FFFFFFF
;
; 3. PAGE TABLE LAYOUT
;    PML4[0]   → boot_pdpt (identity: 0x0 - 0x3FFFFFFF)
;    PML4[256] → boot_pdpt (higher-half alias: 0xFFFF800000000000 - 0xFFFF803FFFFFFFFF)
;    PDPT[0]   → boot_pd  (512 × 2MB huge pages = 1GB, phys 0x0-0x3FFFFFFF)
;    PDPT[3]   → boot_pd2 (512 × 2MB huge pages = 1GB, phys 0xC0000000-0xFFFFFFFF)
;
; 4. MODE SWITCH (32-bit → 64-bit long mode)
;    The transition follows Intel's prescribed sequence:
;    - Enable PAE (CR4.PAE)
;    - Set IA32_EFER.LME (Long Mode Enable MSR 0xC0000080 bit 8)
;    - Enable paging (CR0.PG) — this activates long mode atomically
;    - Load 64-bit GDT with a long-mode-compatible code segment descriptor
;      (D=0, L=1) and data segment
;    - Far-jump into 64-bit mode (long_mode_entry)
;
; 5. 64-BIT ENTRY (long_mode_entry)
;    After the mode switch:
;    - Reload all data segments with 64-bit data selector
;    - Zero BSS (.bss section cleared)
;    - Initialize KASLR offset (currently stubbed to 0)
;    - Switch to high-half stack (rsp += KERNEL_VMA_OFFSET) so C code uses
;      high-half VMA addresses even before the identity map is removed
;    - Call kernel_main(multiboot_magic, multiboot_info)
;    - If kernel_main returns, halt the CPU permanently
;
; MEMORY LAYOUT AT ENTRY
; ─────────────────────
;  Physical         | Contents
;  ─────────────────┼──────────────────────────
;  0x000000-0x000FFF| Real-mode IVT (unused)
;  0x001000-0x09FFFF| Conventional memory
;  0x100000+        | Kernel image (loaded by bootloader)
;  0x100000         | .multiboot header
;  0x101000+        | .boot section (page tables, stack ~128KB)
;  0x?              | .text, .rodata, .data, .bss
;
; BOOT SECTION (.boot)
; ──────────────────
; The .boot section is placed at a low physical address so that page tables
; and the early stack are identity-mapped and accessible before paging is
; fully configured. After the switch to long mode and the high-half stack,
; this section's identity mapping could be removed; currently it is retained
; for simplicity.

bits 32

; Multiboot1 constants
MULTIBOOT_MAGIC     equ 0x1BADB002
MULTIBOOT_FLAGS     equ 0x00010003  ; align + mem + aout (no VBE — QEMU -kernel ignores it)
MULTIBOOT_CHECKSUM  equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

extern _kernel_end

; Constants
KERNEL_VMA_OFFSET equ 0xFFFF800000000000
KASLR_ALIGN       equ 0x200000       ; 2MB alignment for KASLR
KASLR_MAX_OFFSET  equ 0x20000000     ; 512MB max offset

section .multiboot
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

section .boot
align 4

; Page tables — identity map first 1GB and map higher-half kernel
; These reside in .boot so they're accessible at low physical addresses
; before paging is active. Each table is page-aligned for CR3 loading.
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
    times 131072 db 0
boot_stack_top:

; KASLR offset storage (filled in 64-bit mode before kernel_main)
align 8
global kaslr_boot_offset
kaslr_boot_offset:
    dq 0

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
    or eax, 0x93                    ; present + writable + huge + PCD (uncacheable MMIO)
    mov [boot_pd2 + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .fill_pd2

    ; ── ENABLE PAGING & TRANSITION TO 64-BIT MODE ──
    ; Step 1: Load PML4 base into CR3 (page-table root)
    mov eax, boot_pml4
    mov cr3, eax

    ; Step 2: Enable Physical Address Extension (CR4.PAE)
    ; Required for long mode — PAE provides 64-bit page table entries
    ; even before the CPU enters 64-bit mode.
    mov eax, cr4
    or eax, (1 << 5)               ; CR4.PAE
    mov cr4, eax

    ; Step 3: Enable Long Mode via IA32_EFER MSR
    ; IA32_EFER (0xC0000080) bit 8 = LME (Long Mode Enable).
    ; This tells the CPU to enter long mode when paging is enabled.
    mov ecx, 0xC0000080             ; IA32_EFER MSR
    rdmsr
    or eax, (1 << 8)               ; LME (Long Mode Enable)
    wrmsr

    ; Step 4: Enable paging (CR0.PG) — atomically activates long mode
    ; since PAE and LME are already set.
    mov eax, cr0
    or eax, (1 << 31)              ; CR0.PG
    mov cr0, eax

    ; Step 5: Load 64-bit GDT and far-jump into 64-bit mode
    ; The far-jump forces the CPU to reload CS with the new descriptor,
    ; transitioning from 32-bit compat mode to 64-bit long mode.
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_entry

; ── 64-BIT LONG MODE ENTRY ──
; At this point the CPU is in 64-bit long mode. The identity-mapped page tables
; let us execute the next instructions at their physical addresses.
bits 64
long_mode_entry:
    ; Reload all data segments with the 64-bit data selector
    ; In long mode, segment bases/limits are ignored (flat model), but we must
    ; load valid selectors to satisfy segmentation checks.
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack pointer (still physical address for now)
    mov rsp, boot_stack_top

    ; Preserve multiboot info across BSS zeroing
    ; r12d = magic number, r13d = multiboot info struct pointer
    mov r12d, edi
    mov r13d, esi

    ; Clear direction flag (forward string operations)
    cld

    ; ── ZERO BSS SEGMENT ──
    ; All uninitialized global variables (.bss) must be zeroed before first use
    ; by C code. The linker provides _bss_start and _bss_end symbols.
    extern _bss_start, _bss_end
    mov rdi, _bss_start
    mov rcx, _bss_end
    sub rcx, rdi
    shr rcx, 3                     ; convert bytes to qwords
    xor rax, rax
    rep stosq

    ; Restore multiboot arguments for kernel_main
    mov edi, r12d
    mov esi, r13d

    ; ── KASLR: Store initial offset (will be set by kaslr_init in C) ─
    ; The C function kaslr_init() called from kernel_main() will set
    ; the actual randomized offset.  We store 0 here initially.
    ; When full PIE support is added, boot.asm will call kaslr_get_offset
    ; directly and adjust the page tables before jumping to kernel_main.
    xor rax, rax
    mov [kaslr_boot_offset], rax

    ; ── SWITCH TO HIGH-HALF STACK ──
    ; Shift stack pointer to the higher-half virtual address range so all C
    ; code uses high-half VMA addresses. This prevents local-variable references
    ; from producing low identity-mapped addresses that become invalid when the
    ; identity map is removed by the VMM later.
    mov rsp, boot_stack_top
    mov rax, KERNEL_VMA_OFFSET
    add rsp, rax

    ; ── CALL KERNEL MAIN ──
    ; kernel_main is linked at a high VMA address (0xFFFF800000000000+).
    ; The absolute call through rax uses the full 64-bit address.
    extern kernel_main
    mov rax, kernel_main
    call rax

    ; ── HALT (should never reach here) ──
    ; If kernel_main returns, disable interrupts and halt forever.
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
