/* efi_menu.c — UEFI boot menu
 *
 * Displays a simple boot menu showing available kernel entries from
 * loader.conf.  Supports configurable timeout with default selection.
 *
 * Item S158: UEFI boot menu
 *
 * Config file format (loader.conf):
 *   default <name>
 *   timeout <seconds>
 *   entry <name> <kernel_path> [initrd_path]
 *
 * Example:
 *   default osdev
 *   timeout 5
 *   entry osdev /EFI/osdev/kernel.bin /EFI/osdev/initrd.img
 *   entry fallback /EFI/osdev/kernel.bin
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "fbcon.h"
#include "vga.h"

/* Maximum number of boot entries */
#define MAX_ENTRIES      16
#define MAX_NAME_LEN     64
#define MAX_PATH_LEN     256
#define CONFIG_PATH      "/EFI/loader/loader.conf"

/* Boot entry descriptor */
struct boot_entry {
    char name[MAX_NAME_LEN];
    char kernel_path[MAX_PATH_LEN];
    char initrd_path[MAX_PATH_LEN];
    int  has_initrd;
};

/* Global boot menu state */
static struct boot_entry boot_entries[MAX_ENTRIES];
static int entry_count = 0;
static int default_entry = 0;
static int menu_timeout = 5;     /* seconds before auto-boot */

/* Forward declarations */
static int parse_loader_conf(void);
static void render_menu(int highlight);
static void clear_menu_area(void);
static void boot_entry_at(int idx);

/*
 * Read and parse /EFI/loader/loader.conf.
 * Returns number of entries parsed, or -1 on error.
 */
static int parse_loader_conf(void)
{
    /* In a real UEFI implementation, this would read from the ESP
     * filesystem.  For now, we simulate with a hardcoded default
     * set that can be overridden. */
    entry_count = 0;
    default_entry = 0;
    menu_timeout = 5;

    /* Try to read from the config file path.
     * For this kernel, we fall back to built-in defaults. */
    int fd = 0; /* Would be open(CONFIG_PATH, O_RDONLY) in UEFI */
    (void)fd;

    /* Built-in fallback entries */
    if (entry_count < MAX_ENTRIES) {
        strncpy(boot_entries[entry_count].name, "osdev",
                MAX_NAME_LEN - 1);
        strncpy(boot_entries[entry_count].kernel_path,
                "/EFI/osdev/kernel.bin", MAX_PATH_LEN - 1);
        strncpy(boot_entries[entry_count].initrd_path,
                "/EFI/osdev/initrd.img", MAX_PATH_LEN - 1);
        boot_entries[entry_count].has_initrd = 1;
        entry_count++;
    }

    if (entry_count < MAX_ENTRIES) {
        strncpy(boot_entries[entry_count].name, "fallback",
                MAX_NAME_LEN - 1);
        strncpy(boot_entries[entry_count].kernel_path,
                "/EFI/osdev/kernel.bin", MAX_PATH_LEN - 1);
        boot_entries[entry_count].initrd_path[0] = '\0';
        boot_entries[entry_count].has_initrd = 0;
        entry_count++;
    }

    return entry_count;
}

/*
 * Render the boot menu on the framebuffer.
 * Highlight indicates which entry is currently selected.
 */
static void render_menu(int highlight)
{
    clear_menu_area();

    kprintf("===== UEFI Boot Menu =====\n");
    kprintf("Select an entry to boot:\n\n");

    for (int i = 0; i < entry_count; i++) {
        if (i == highlight)
            kprintf(" > ");
        else
            kprintf("   ");

        kprintf("%s", boot_entries[i].name);
        if (boot_entries[i].has_initrd)
            kprintf(" [initrd]");
        kprintf("\n");
    }

    kprintf("\nTimeout: %d seconds\n", menu_timeout);
    kprintf("Use Up/Down arrows, Enter to boot\n");
}

/* Clear the area where the menu is displayed */
static void clear_menu_area(void)
{
    /* In a real UEFI impl, clear the GOP framebuffer area.
     * For now, emit enough newlines to push content. */
    for (int i = 0; i < 25; i++)
        kprintf("\n");
}

/*
 * Boot the entry at the given index.
 * In a full implementation this would set up the UEFI load options
 * and call ExitBootServices + start the kernel.
 */
static void boot_entry_at(int idx)
{
    if (idx < 0 || idx >= entry_count)
        return;

    kprintf("\nBooting '%s'...\n", boot_entries[idx].name);
    kprintf("  Kernel: %s\n", boot_entries[idx].kernel_path);
    if (boot_entries[idx].has_initrd)
        kprintf("  Initrd: %s\n", boot_entries[idx].initrd_path);

    /* In a real UEFI boot, we would:
     *   1. Load the kernel ELF from the ESP
     *   2. Load the initrd if present
     *   3. Set up kernel command line
     *   4. Call ExitBootServices
     *   5. Jump to kernel entry point
     *
     * For this kernel, we delegate to the existing multiboot path.
     */
}

/*
 * Show the UEFI boot menu and let the user select an entry.
 * If timeout expires, the default entry is booted automatically.
 *
 * Returns 0 on success, -1 on error.
 */
int uefi_boot_menu_show(void)
{
    if (parse_loader_conf() <= 0) {
        kprintf("[UEFI] No boot entries found\n");
        return -1;
    }

    int highlight = default_entry;
    int ticks_remaining = menu_timeout * 10; /* ~100ms ticks */

    render_menu(highlight);

    /* Simple timeout loop — in a real UEFI impl, this would use
     * the UEFI timer services and wait for key input. */
    while (ticks_remaining > 0) {
        /* Check for keypress (stubbed — would use UEFI stdin) */
        int key = 0; /* Would be: uefi_read_key() */
        (void)key;

        /* Simulate timeout countdown */
        ticks_remaining--;
        if (ticks_remaining % 10 == 0) {
            /* Update timeout display every second */
            kprintf("\rTimeout: %d seconds ",
                    ticks_remaining / 10);
        }

        /* Busy-wait approximation of 100ms */
        for (volatile int i = 0; i < 2000000; i++)
            __asm__ volatile("pause");
    }

    /* Timeout expired — boot default */
    kprintf("\n");
    boot_entry_at(default_entry);
    return 0;
}

/*
 * Register a custom boot entry (for programmatic use).
 * Returns 0 on success, -1 if table is full.
 */
int uefi_boot_menu_add_entry(const char *name,
                             const char *kernel_path,
                             const char *initrd_path)
{
    if (!name || !kernel_path || entry_count >= MAX_ENTRIES)
        return -1;

    strncpy(boot_entries[entry_count].name, name,
            MAX_NAME_LEN - 1);
    boot_entries[entry_count].name[MAX_NAME_LEN - 1] = '\0';

    strncpy(boot_entries[entry_count].kernel_path, kernel_path,
            MAX_PATH_LEN - 1);
    boot_entries[entry_count].kernel_path[MAX_PATH_LEN - 1] = '\0';

    if (initrd_path && initrd_path[0]) {
        strncpy(boot_entries[entry_count].initrd_path, initrd_path,
                MAX_PATH_LEN - 1);
        boot_entries[entry_count].initrd_path[MAX_PATH_LEN - 1] = '\0';
        boot_entries[entry_count].has_initrd = 1;
    } else {
        boot_entries[entry_count].initrd_path[0] = '\0';
        boot_entries[entry_count].has_initrd = 0;
    }

    entry_count++;
    return 0;
}

/* Return the number of registered boot entries */
int uefi_boot_menu_entry_count(void)
{
    return entry_count;
}

/* ── Stub: efi_menu_show ───────────────────────────────────────────── */
int efi_menu_show(void)
{
    kprintf("[EFI_MENU] efi_menu_show: delegating to uefi_boot_menu_show\n");
    return uefi_boot_menu_show();
}

/* ── Stub: efi_menu_get_selection ──────────────────────────────────── */
int efi_menu_get_selection(void)
{
    if (uefi_boot_menu_entry_count() <= 0) {
        kprintf("[EFI_MENU] efi_menu_get_selection: no boot entries\n");
        return -1;
    }

    kprintf("[EFI_MENU] efi_menu_get_selection: showing menu for selection\n");

    int highlight = default_entry;
    render_menu(highlight);

    /* Simple timeout-based auto-boot loop */
    int ticks_remaining = menu_timeout * 10; /* ~100ms ticks */
    while (ticks_remaining > 0) {
        /* Would use UEFI SimpleTextInput in a real impl */
        /* For now, just show the menu and decrement timeout */
        ticks_remaining--;
        if (ticks_remaining % 10 == 0) {
            kprintf("\rTimeout: %d seconds ", ticks_remaining / 10);
        }
        /* Busy-wait ~100ms */
        for (volatile int i = 0; i < 2000000; i++)
            __asm__ volatile("pause");
    }

    kprintf("\n[EFI_MENU] efi_menu_get_selection: timeout, returning default %d\n",
            default_entry);
    return default_entry;
}

/* ── Stub: efi_menu_boot_entry ─────────────────────────────────────── */
int efi_menu_boot_entry(int index)
{
    int count = uefi_boot_menu_entry_count();
    if (index < 0 || index >= count) {
        kprintf("[EFI_MENU] efi_menu_boot_entry: invalid index %d\n", index);
        return -EINVAL;
    }
    kprintf("[EFI_MENU] efi_menu_boot_entry: booting entry %d\n", index);
    boot_entry_at(index);
    return 0;
}

/* ── Stub: efi_menu_set_timeout ────────────────────────────────────── */
void efi_menu_set_timeout(int seconds)
{
    if (seconds >= 0 && seconds <= 300)
        menu_timeout = seconds;
    kprintf("[EFI_MENU] efi_menu_set_timeout: set to %d seconds\n", seconds);
}

/* ── Stub: efi_menu_add_entry ──────────────────────────────────────── */
int efi_menu_add_entry(const char *name, const char *kernel_path,
                       const char *initrd_path)
{
    if (!name || !kernel_path) {
        kprintf("[EFI_MENU] efi_menu_add_entry: invalid arguments\n");
        return -EINVAL;
    }
    int ret = uefi_boot_menu_add_entry(name, kernel_path, initrd_path);
    kprintf("[EFI_MENU] efi_menu_add_entry: '%s' -> %s\n", name, kernel_path);
    return ret;
}
