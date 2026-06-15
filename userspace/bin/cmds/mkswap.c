/* mkswap.c — set up swap area on device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkswap DEVICE\n");
        return 1;
    }

    const char *device = argv[1];

    int fd = open(device, O_RDWR, 0);
    if (fd < 0) {
        printf("mkswap: cannot open '%s'\n", device);
        return 1;
    }

    /* Write swap signature at offset 0x0FFE (for 4K pages) */
    /* Swap header structure:
     * Offset 0x0FFE: 10 bytes signature "SWAPSPACE2" (or "SWAP-SPACE")
     * For 4K pages, signature at 0x0FFE = 4094
     */

    /* First 1KB: all zeroes (bitmap for bad blocks) */
    unsigned char zerobuf[1024];
    memset(zerobuf, 0, sizeof(zerobuf));
    write(fd, zerobuf, sizeof(zerobuf));

    /* Write swap signature at 0x0FFE (page size - 2) */
    unsigned long page_size = 4096;
    unsigned long sig_offset = page_size - 10;

    lseek(fd, (long)sig_offset, SEEK_SET);
    const char *signature = "SWAPSPACE2";
    write(fd, signature, 10);

    /* Write version at page 0 offset 0x0FFC (4 bytes, version 1) */
    lseek(fd, (long)(page_size - 4), SEEK_SET);
    unsigned int version = 1;
    write(fd, &version, sizeof(version));

    /* Set up swap label area (page 1): all zeros */
    unsigned char page[4096];
    memset(page, 0, sizeof(page));

    /* Write UUID (just zeroed for now) */

    lseek(fd, (long)page_size, SEEK_SET);
    write(fd, page, sizeof(page));

    close(fd);

    printf("mkswap: swapped space created on %s (page size=%lu)\n",
           device, page_size);
    return 0;
}
