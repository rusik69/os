/*
 * dos_load.c – DOS program loader (COM & MZ)
 *
 * Provides dos_load_com() and dos_load_mz() to load DOS executables
 * into the emulator's 1 MB conventional memory, and dos_exec() as a
 * top-level "load + init + run" entry point.
 *
 * Memory is allocated as a static buffer for this minimal emulator.
 *
 * When compiled as a loadable kernel module (MODULE defined), the ELF
 * module loader calls init_module() which logs availability.  The
 * built-in kernel path provides dos_exec() directly for cmd_dosbox.
 */

#include "dos.h"
#include "string.h"
#include "printf.h"
#include "scheduler.h"
#include "vfs.h"
#ifdef MODULE
#include "module.h"
#endif

/* ------------------------------------------------------------------ */
/* Emulator functions defined in dos_emu.c                             */
/* ------------------------------------------------------------------ */
extern void dos_emu_init(struct dos_cpu_state *state);
extern void dos_emu_run(struct dos_cpu_state *state);
extern void dos_emu_stop(struct dos_cpu_state *state);

/* Static 1 MB conventional memory pool */
static uint8_t dos_memory_pool[DOS_CONV_MEM_SIZE];

/* ------------------------------------------------------------------ */
/* dos_load_com – load a .COM program                                  */
/*                                                                     */
/* .COM layout:                                                        */
/*   offset 0x0000 – PSP (256 bytes)                                   */
/*   offset 0x0100 – program image                                     */
/*   All segment registers set to 0.                                   */
/*   SP = 0xFFFE (top of the 64 KB segment minus 2).                  */
/* ------------------------------------------------------------------ */
int dos_load_com(struct dos_cpu_state *state, const uint8_t *data, uint32_t size)
{
    state->memory = dos_memory_pool;
    __builtin_memset(state->memory, 0, DOS_CONV_MEM_SIZE);

    /* Set up minimal PSP at segment 0, offset 0 */
    state->memory[0] = 0xCD;       /* INT */
    state->memory[1] = 0x20;       /* 20h */
    state->memory[2] = 0xFF;       /* end of memory (top of segment) */
    state->memory[3] = 0xFF;
    state->memory[0x2C] = 0;       /* environment segment = 0 */
    state->memory[0x2D] = 0;
    state->memory[0x80] = 0;       /* command tail length = 0 */

    /* Load program image at offset 0x100 */
    uint32_t copy_size = size;
    if (copy_size > DOS_CONV_MEM_SIZE - 0x100)
        copy_size = DOS_CONV_MEM_SIZE - 0x100;
    __builtin_memcpy(state->memory + 0x100, data, copy_size);

    /* Set initial register state */
    state->cs = 0;
    state->ds = 0;
    state->es = 0;
    state->ss = 0;
    state->ip = 0x100;
    state->sp = 0xFFFE;
    state->flags = 0x0002;          /* bit 1 always set */

    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_load_mz – load an MZ executable                                 */
/*                                                                     */
/* Reads the MZ header to determine load layout, copies the image,     */
/* applies relocations, and sets initial CS:IP / SS:SP.                */
/* ------------------------------------------------------------------ */
int dos_load_mz(struct dos_cpu_state *state, const uint8_t *data, uint32_t size)
{
    if (size < sizeof(struct mz_header))
        return -1;

    const struct mz_header *hdr = (const struct mz_header *)data;
    if (hdr->e_magic != MZ_MAGIC)
        return -1;

    state->memory = dos_memory_pool;
    __builtin_memset(state->memory, 0, DOS_CONV_MEM_SIZE);

    /* Load segment: we place the image at a conventional address.
     * Use load_seg = 0x60 so that the image starts at linear 0x600,
     * just after a 256-byte PSP (segment 0x00, offset 0x000-0x0FF). */
    uint16_t load_seg = 0x60;

    /* Set up minimal PSP at segment 0 */
    state->memory[0] = 0xCD;       /* INT */
    state->memory[1] = 0x20;       /* 20h */
    state->memory[2] = 0xFF;
    state->memory[3] = 0xFF;
    state->memory[0x2C] = 0;
    state->memory[0x2D] = 0;
    state->memory[0x80] = 0;

    /* Copy load image from file offset (e_cparhdr * 16) */
    uint32_t image_off = (uint32_t)hdr->e_cparhdr * 16;
    uint32_t image_sz  = size - image_off;
    uint32_t load_linear = (uint32_t)load_seg << 4;
    if (image_sz > DOS_CONV_MEM_SIZE - load_linear)
        image_sz = DOS_CONV_MEM_SIZE - load_linear;

    __builtin_memcpy(state->memory + load_linear, data + image_off, image_sz);

    /* Apply relocations */
    uint32_t reloc_count = hdr->e_crlc;
    uint32_t reloc_tab   = hdr->e_lfarlc;

    for (uint32_t i = 0; i < reloc_count; i++) {
        uint32_t entry_off = reloc_tab + i * 4;
        if (entry_off + 4 > size) break;

        uint16_t reloc_off = (uint16_t)data[entry_off]
                           | ((uint16_t)data[entry_off + 1] << 8);
        uint16_t reloc_seg = (uint16_t)data[entry_off + 2]
                           | ((uint16_t)data[entry_off + 3] << 8);

        uint32_t target = load_linear
                        + ((uint32_t)reloc_seg << 4)
                        + reloc_off;
        if (target + 2 > DOS_CONV_MEM_SIZE) continue;

        uint16_t old = (uint16_t)state->memory[target]
                     | ((uint16_t)state->memory[target + 1] << 8);
        uint16_t newv = old + load_seg;
        state->memory[target]     = (uint8_t)newv;
        state->memory[target + 1] = (uint8_t)(newv >> 8);
    }

    /* Set initial register state */
    state->cs = load_seg + hdr->e_cs;
    state->ip = hdr->e_ip;
    state->ss = load_seg + hdr->e_ss;
    state->sp = hdr->e_sp;
    state->ds = load_seg;
    state->es = load_seg;
    state->flags = 0x0002;

    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_exec – top-level load-and-run entry point                       */
/*                                                                     */
/* Reads the file at 'path', detects COM vs MZ by magic bytes,         */
/* loads, initialises, and runs the emulator.                          */
/* Returns 0 on success, -1 on error.                                  */
/* ------------------------------------------------------------------ */
int dos_exec(const char *path)
{
    uint8_t file_buf[65536];
    uint32_t file_size = 0;

    if (vfs_read(path, file_buf, sizeof(file_buf), &file_size) != 0) {
        kprintf("dos_exec: cannot read '%s'\n", path);
        return -1;
    }
    if (file_size < 2) {
        kprintf("dos_exec: empty file '%s'\n", path);
        return -1;
    }

    struct dos_cpu_state state;
    dos_emu_init(&state);

    uint16_t magic = (uint16_t)file_buf[0] | ((uint16_t)file_buf[1] << 8);
    int ret;

    if (magic == MZ_MAGIC) {
        ret = dos_load_mz(&state, file_buf, file_size);
    } else {
        ret = dos_load_com(&state, file_buf, file_size);
    }

    if (ret != 0) {
        kprintf("dos_exec: load failed for '%s'\n", path);
        return -1;
    }

    dos_emu_run(&state);
    return 0;
}

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    kprintf("[dos] DOS emulator module loaded\n");
    return 0;  /* success */
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    kprintf("[dos] DOS emulator module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("DOS emulator — loads and runs .COM and .MZ executables");
MODULE_VERSION("1.0");
#endif /* MODULE */
