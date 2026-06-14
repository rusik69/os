/* Linker script for userspace programs (load at 0x400000) */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);  /* PF_R */
    data   PT_LOAD FLAGS(6);  /* PF_R | PF_W */
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
    } :rodata

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

    /* Mark stack as non-executable */
    .note.GNU-stack : {
        *(.note.GNU-stack)
    } :rodata

    /* End of image */
    __end = .;

    /DISCARD/ : {
        *(.comment*)
        *(.eh_frame*)
        *(.debug*)
    }
}
