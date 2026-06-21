/*
 * efi_menu.c — UEFI boot menu with SimpleTextInput polling
 *
 * Displays a boot menu, polls UEFI SimpleTextInput for keypresses,
 * and supports configurable timeout with default selection.
 *
 * Item S158: UEFI boot menu
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "fbcon.h"
#include "vga.h"

/* Maximum boot entries */
#define MAX_ENTRIES      16
#define MAX_NAME_LEN     64
#define MAX_PATH_LEN     256

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

/* UEFI Simple Text Input Protocol function type */
typedef uint64_t (*efi_input_read_key_t)(void *this, uint8_t *key);

/* Pointer to UEFI SimpleTextInput protocol's ReadKeyStroke
 * (set by boot stub, or NULL if not available) */
static efi_input_read_key_t g_efi_read_key = NULL;

/*
 * efi_menu_register_input — called by boot stub to register
 * the UEFI SimpleTextInput ReadKeyStroke function pointer.
 */
void efi_menu_register_input(void *read_key_fn)
{
    g_efi_read_key = (efi_input_read_key_t)read_key_fn;
}

/*
 * Read a single keypress from UEFI Simple Text Input.
 * Returns the key's Unicode char (0-0xFFFF) or 0 if no key pending.
 * In a raw UEFI environment this calls ReadKeyStroke on ConIn;
 * if no protocol is registered, returns 0 (no key) immediately.
 */
static int efi_read_keypress(void)
{
    if (!g_efi_read_key)
        return 0; /* No input available */

    uint8_t key_data[2]; /* EFI_INPUT_KEY: scan_code(2) + unicode_char(2) */
    uint64_t status = g_efi_read_key(NULL, key_data);
    if (status != 0)
        return 0; /* No key available */

    /* Return the Unicode character (little-endian uint16_t) */
    return (int)key_data[0] | ((int)key_data[1] << 8);
}

/*
 * Poll for a keypress with a timeout (in milliseconds).
 * Returns key Unicode value, or 0 on timeout.
 * Uses a simple busy-wait loop with 10ms granularity.
 */
static int efi_poll_keypress(int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int key = efi_read_keypress();
        if (key != 0)
            return key;
        /* Busy-wait ~10ms */
        for (volatile int i = 0; i < 200000; i++)
            __asm__ volatile("pause");
        waited += 10;
    }
    return 0; /* timeout */
}

/*
 * Parse built-in boot entries.
 * In a real UEFI impl, this would read /EFI/loader/loader.conf.
 */
static void parse_default_entries(void)
{
    entry_count = 0;
    default_entry = 0;
    menu_timeout = 5;

    if (entry_count < MAX_ENTRIES) {
        strncpy(boot_entries[entry_count].name, "osdev", MAX_NAME_LEN - 1);
        strncpy(boot_entries[entry_count].kernel_path,
                "/EFI/osdev/kernel.bin", MAX_PATH_LEN - 1);
        strncpy(boot_entries[entry_count].initrd_path,
                "/EFI/osdev/initrd.img", MAX_PATH_LEN - 1);
        boot_entries[entry_count].has_initrd = 1;
        entry_count++;
    }

    if (entry_count < MAX_ENTRIES) {
        strncpy(boot_entries[entry_count].name, "fallback", MAX_NAME_LEN - 1);
        strncpy(boot_entries[entry_count].kernel_path,
                "/EFI/osdev/kernel.bin", MAX_PATH_LEN - 1);
        boot_entries[entry_count].initrd_path[0] = '\0';
        boot_entries[entry_count].has_initrd = 0;
        entry_count++;
    }
}

/*
 * Render the boot menu.
 */
static void render_menu(int highlight)
{
    for (int i = 0; i < 25; i++) kprintf("\n");

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

/*
 * Boot the entry at the given index.
 */
static void boot_entry_at(int idx)
{
    if (idx < 0 || idx >= entry_count)
        return;

    kprintf("\nBooting '%s'...\n", boot_entries[idx].name);
    kprintf("  Kernel: %s\n", boot_entries[idx].kernel_path);
    if (boot_entries[idx].has_initrd)
        kprintf("  Initrd: %s\n", boot_entries[idx].initrd_path);
}

/*
 * Show the UEFI boot menu and let the user select an entry.
 * Polls SimpleTextInput for keypresses (Up/Down/Enter).
 * If timeout expires (or no input available), boots the default.
 *
 * Returns 0 on success, -1 on error.
 */
int uefi_boot_menu_show(void)
{
    parse_default_entries();
    if (entry_count <= 0) {
        kprintf("[UEFI] No boot entries found\n");
        return -1;
    }

    int highlight = default_entry;
    int timeout_deciseconds = menu_timeout * 10; /* 100ms per tick */

    render_menu(highlight);

    /* Main polling loop: 100ms per iteration */
    while (timeout_deciseconds > 0) {
        /* Refresh timeout display every second */
        if (timeout_deciseconds % 10 == 0) {
            kprintf("\rTimeout: %d seconds ", timeout_deciseconds / 10);
        }

        /* Poll for a key with 100ms timeout per iteration */
        int key = efi_poll_keypress(100);
        if (key != 0) {
            /* Key pressed — handle navigation */
            if (key == '\r' || key == '\n') {
                /* Enter — boot selected entry */
                break;
            } else if (key == 0x1B) {
                /* Escape — not used here, but could be cancel */
            } else if (key == 'A' - 0x40) {
                /* Up arrow (Ctrl-A / ASCII 0x01) in some UEFI mappings */
                highlight = (highlight - 1 + entry_count) % entry_count;
                render_menu(highlight);
            } else if (key == 'B' - 0x40) {
                /* Down arrow (Ctrl-B / ASCII 0x02) in some UEFI mappings */
                highlight = (highlight + 1) % entry_count;
                render_menu(highlight);
            }
            /* Reset timeout on keypress */
            timeout_deciseconds = menu_timeout * 10;
        } else {
            timeout_deciseconds--;
        }
    }

    /* Timeout expired or Enter pressed — boot the selection */
    kprintf("\n");
    boot_entry_at(highlight);
    return 0;
}

/*
 * Register a custom boot entry (for programmatic use).
 */
int uefi_boot_menu_add_entry(const char *name,
                              const char *kernel_path,
                              const char *initrd_path)
{
    if (!name || !kernel_path || entry_count >= MAX_ENTRIES)
        return -1;

    strncpy(boot_entries[entry_count].name, name, MAX_NAME_LEN - 1);
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
    (void)0;
    kprintf("[EFI_MENU] efi_menu_show: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: efi_menu_get_selection ──────────────────────────────────── */
int efi_menu_get_selection(void)
{
    kprintf("[EFI_MENU] efi_menu_get_selection: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: efi_menu_boot_entry ─────────────────────────────────────── */
int efi_menu_boot_entry(int index)
{
    (void)index;
    kprintf("[EFI_MENU] efi_menu_boot_entry: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: efi_menu_set_timeout ────────────────────────────────────── */
void efi_menu_set_timeout(int seconds)
{
    (void)seconds;
    kprintf("[EFI_MENU] efi_menu_set_timeout: not yet implemented\n");
}

/* ── Stub: efi_menu_add_entry ──────────────────────────────────────── */
int efi_menu_add_entry(const char *name, const char *kernel_path,
                       const char *initrd_path)
{
    (void)name; (void)kernel_path; (void)initrd_path;
    kprintf("[EFI_MENU] efi_menu_add_entry: not yet implemented\n");
    return -ENOSYS;
}

/* Set timeout (in seconds) for the boot menu */
void uefi_boot_menu_set_timeout(int seconds)
{
    if (seconds >= 0 && seconds <= 300)
        menu_timeout = seconds;
}
