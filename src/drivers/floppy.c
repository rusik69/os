#include "floppy.h"
#include "printf.h"

int floppy_init(void) {
    /* Stub: no floppy controller present in typical QEMU/VM setup */
    kprintf("[--] Floppy: no controller found\n");
    return -1;
}

int floppy_is_present(void) {
    return 0;
}

int floppy_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf) {
    (void)drive;
    (void)lba;
    (void)count;
    (void)buf;
    /* Stub: always fails */
    return -1;
}
