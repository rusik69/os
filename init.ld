/*
 * init.ld — Linker script for standalone userspace ELF binaries
 *
 * Produces a PIE (position-independent executable) that starts at 0x0
 * so the kernel's ELF loader can place it anywhere in the address space.
 * The loader uses the file segments (program headers) directly.
 *
 * Output is a raw ELF64 executable (no sections needed at runtime).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS(5);   /* PF_R | PF_X */
    rodata  PT_LOAD    FLAGS(4);   /* PF_R */
    data    PT_LOAD    FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x0 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .data : {
        *(.data .data.*)
        *(.got .got.*)
        *(.got.plt)
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
        *(.note.gnu.property)
        *(.gnu.hash)
        *(.plt)
        *(.plt.got)
    }
}
