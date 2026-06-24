/*
 * multiboot.c — Multiboot info structure handling
 *
 * Saves the multiboot info pointer from the bootloader and provides
 * helper functions to access modules and other info.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* Multiboot1 info structure (as used elsewhere in the kernel) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

/* Module entry in multiboot module list */
struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t reserved;
} __attribute__((packed));

static uint64_t g_mbi_phys = 0;
static struct multiboot_info *g_mbi = NULL;

/*
 * multiboot_save_info — Store a pointer to the multiboot info structure.
 *
 * Called early in boot to save the physical address of the multiboot
 * info structure provided by the bootloader.  Subsequent calls to
 * multiboot_get_module() use this saved pointer.
 *
 * Parameters:
 *   mbi_phys — physical address of the multiboot_info structure
 */
void multiboot_save_info(uint64_t mbi_phys)
{
    g_mbi_phys = mbi_phys;
    g_mbi = (struct multiboot_info *)(uintptr_t)mbi_phys;
    kprintf("[MULTIBOOT] Saved multiboot info at 0x%llx\n",
            (unsigned long long)mbi_phys);

    if (g_mbi) {
        kprintf("[MULTIBOOT] flags=0x%x, mods=%u\n",
                g_mbi->flags, g_mbi->mods_count);
    }
}

/*
 * multiboot_get_module — Get details about a multiboot module by index.
 *
 * Iterates through the multiboot module list and returns the start
 * address, end address, and command-line string for the module at
 * the given index.
 *
 * Parameters:
 *   index   — zero-based module index
 *   start   — (out) physical start address of the module, or NULL
 *   end     — (out) physical end address (exclusive), or NULL
 *   cmdline — (out) pointer to the module's command-line string, or NULL
 *
 * Returns:
 *   0 on success, -ENOENT if no multiboot info or index out of range.
 */
int multiboot_get_module(int index, uint64_t *start, uint64_t *end,
                         const char **cmdline)
{
    if (!g_mbi)
        return -ENOENT;

    if (!(g_mbi->flags & (1U << 3))) /* mods flag = bit 3 */
        return -ENOENT;

    if (index < 0 || (uint32_t)index >= g_mbi->mods_count)
        return -ENOENT;

    struct multiboot_module *mods =
        (struct multiboot_module *)(uintptr_t)g_mbi->mods_addr;

    if (start)
        *start = mods[index].mod_start;
    if (end)
        *end = mods[index].mod_end;
    if (cmdline) {
        if (mods[index].cmdline != 0)
            *cmdline = (const char *)(uintptr_t)mods[index].cmdline;
        else
            *cmdline = NULL;
    }

    kprintf("[MULTIBOOT] Module %d: start=0x%x end=0x%x cmdline=%s\n",
            index, mods[index].mod_start, mods[index].mod_end,
            mods[index].cmdline
                ? (const char *)(uintptr_t)mods[index].cmdline
                : "");

    return 0;
}

/*
 * multiboot_get_info — Return the saved multiboot info pointer.
 *
 * Returns the physical address of the multiboot info structure,
 * or 0 if multiboot_save_info() has not been called.
 */
uint64_t multiboot_get_info_phys(void)
{
    return g_mbi_phys;
}
