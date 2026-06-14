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
KERNEL_TEXT_LMA   = 0x126000;

SECTIONS
{
    . = KERNEL_LMA;

    .multiboot ALIGN(4) : {
        KEEP(*(.multiboot))
    }

    .boot ALIGN(4) : {
        *(.boot)
    }

    /* ── KASLR info ────────────────────────────────────────────────
     * Small data section for boot-time KASLR offset storage.
     * This is at a known physical address so boot.asm can find it. */
    .kaslr_info ALIGN(8) : {
        _kaslr_info_start = .;
        *(.kaslr_info)
        _kaslr_info_end = .;
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

    .initcall ALIGN(8) : AT(ADDR(.initcall) - KERNEL_VMA_OFFSET) {
        __initcall_start = .;
        KEEP(*(.initcall.0))
        KEEP(*(.initcall.1))
        KEEP(*(.initcall.2))
        KEEP(*(.initcall.3))
        KEEP(*(.initcall.4))
        KEEP(*(.initcall.5))
        __initcall_end = .;
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

    .lbss ALIGN(4K) : AT(ADDR(.lbss) - KERNEL_VMA_OFFSET) {
        _lbss_start = .;
        *(.lbss .lbss.*)
        _lbss_end = .;
    }

    /* ── Kernel symbol export table ────────────────────────────────
     * Collected from EXPORT_SYMBOL / EXPORT_SYMBOL_GPL uses across
     * the kernel.  Used by the module loader for symbol resolution. */
    .ksymtab ALIGN(16) : AT(ADDR(.ksymtab) - KERNEL_VMA_OFFSET) {
        __ksymtab_start = .;
        KEEP(*(.ksymtab))
        __ksymtab_end = .;
    }

    /* ── Comprehensive kallsyms table (all global symbols) ─────────
     * In addition to EXPORT_SYMBOL entries, this section collects
     * ALL global kernel symbols (functions and variables) for use
     * as a fallback resolution source by the module loader.
     *
     * Entries use the same struct ksym_entry format as .ksymtab
     * but all have gpl_only = 0.
     *
     * Source files can add their symbols here via the __KALLSYMS()
     * macro, and the linker collects them between __kallsyms_start
     * and __kallsyms_end. */
    .kallsyms ALIGN(16) : AT(ADDR(.kallsyms) - KERNEL_VMA_OFFSET) {
        __kallsyms_start = .;
        KEEP(*(.kallsyms))
        __kallsyms_end = .;
    }

    /* ── Module metadata (.modinfo) section (M41) ──────────────────
     * Collects all MODULE_LICENSE, MODULE_AUTHOR, MODULE_DESCRIPTION,
     * MODULE_VERSION, MODULE_ALIAS, and MODULE_DEPENDS entries from
     * both built-in code and loadable modules.  The module loader
     * parses these key=value strings at load time for dependency
     * resolution, alias matching, and vermagic verification.
     *
     * For built-in (non-modular) code with MODULE_* annotations,
     * the data still lives here but is accessible at link time.
     * The __modinfo_start / __modinfo_end symbols allow the kernel
     * to enumerate all built-in modinfo entries at boot. */
    .modinfo ALIGN(8) : AT(ADDR(.modinfo) - KERNEL_VMA_OFFSET) {
        __modinfo_start = .;
        KEEP(*(.modinfo))
        __modinfo_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
