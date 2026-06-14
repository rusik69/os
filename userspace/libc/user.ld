/* Linker script for userspace programs (load at 0x400000) */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    data PT_LOAD FLAGS(7);  /* PF_R | PF_W | PF_X */
}

SECTIONS
{
    /* Load address */
    . = 0x400000;

    .text : {
        *(.text*)
        *(.gnu.linkonce.t*)
    } :text

    .rodata : {
        *(.rodata*)
        *(.gnu.linkonce.r*)
    } :data

    .data : {
        *(.data*)
        *(.gnu.linkonce.d*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /* End of image */
    __end = .;

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
        *(.debug*)
    }
}
