OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LMA = 0x100000;

/* High-half virtual offset. Kernel sections are linked at VMA = LMA + KERNEL_VMA_OFFSET.
 * PML4[256] points to the same low-memory PDPT used for identity mapping,
 * so the CPU can execute high-half code immediately after paging is enabled.
 *
 * KERNEL_TEXT_LMA: physical address where Multiboot places .text.
 * Multiboot address-fields mode copies raw file bytes starting at load_addr.
 * .text is at file offset 0xa000; first mapped byte is at file offset 0x1000
 * (physical 0x100000), so: KERNEL_TEXT_LMA = 0x100000 + (0xa000 - 0x1000) = 0x109000
 */
KERNEL_VMA_OFFSET = 0xFFFF800000000000;
KERNEL_TEXT_LMA   = 0x10A000;

SECTIONS
{
    . = KERNEL_LMA;

    .multiboot2 ALIGN(4) : {
        KEEP(*(.multiboot2))
    }

    .boot ALIGN(4) : {
        *(.boot)
    }

    /* High-half kernel sections: high VMA, low physical LMA */
    . = KERNEL_TEXT_LMA + KERNEL_VMA_OFFSET;

    .text ALIGN(4K) : AT(KERNEL_TEXT_LMA) {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA_OFFSET) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA_OFFSET) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA_OFFSET) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
