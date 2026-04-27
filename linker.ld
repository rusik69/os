OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LMA = 0x100000;

SECTIONS
{
    . = KERNEL_LMA;

    .multiboot2 ALIGN(4) : {
        KEEP(*(.multiboot2))
    }

    .boot ALIGN(4) : {
        *(.boot)
    }

    .text ALIGN(4K) : {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    }

    .rodata ALIGN(4K) : {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    }

    .data ALIGN(4K) : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss ALIGN(4K) : {
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
