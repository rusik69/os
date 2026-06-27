/* test_module.c — minimal loadable kernel module for testing (M39)
 *
 * This trivial module verifies the .ko build system.
 * It logs a message on load and unload.
 *
 * Usage:
 *   make obj-m="test_module.ko" modules          # build
 *   insmod /modules/test_module.ko              # load
 *   rmmod test_module                           # unload
 */

#include "module.h"
#include "printf.h"

/* Module entry points — ELF loader looks for "init_module" / "cleanup_module" */
int __init init_module(void) {
    kprintf("[MOD] Test module loaded successfully!\n");
    return 0;
}

void __exit cleanup_module(void) {
    kprintf("[MOD] Test module unloaded.\n");
}

/* Module metadata */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Kernel Team");
MODULE_DESCRIPTION("Minimal test module for .ko build verification (M39)");
MODULE_VERSION("1.0");

/* ── test_module_init ─────────────────────────────────── */
int test_module_init(void)
{
    kprintf("[test_mod] Test module initialized\n");
    return 0;
}
/* ── test_module_exit ─────────────────────────────────── */
int test_module_exit(void)
{
    kprintf("[test_mod] Test module exited\n");
    return 0;
}
/* ── test_module_ioctl ────────────────────────────────── */
int test_module_ioctl(void *file, int cmd, void *arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    return 0;
}
