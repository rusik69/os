#include "shell_cmds.h"
#include "usb.h"
#include "printf.h"

void cmd_lsusb(void) {
    if (!usb_is_present()) {
        kprintf("No USB host controllers detected\n");
        return;
    }
    int n = usb_get_device_count();
    kprintf("USB devices: %d\n", (uint64_t)n);
    for (int i = 0; i < n; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) continue;
        const char *spd = dev->speed == 2 ? "High" :
                          dev->speed == 1 ? "Low"  : "Full";
        kprintf("  Bus %03d Device %03d: %s-speed class=%02x\n",
                (uint64_t)1, (uint64_t)dev->addr,
                spd, (uint64_t)dev->class_code);
    }
    if (n == 0)
        kprintf("  (no devices connected)\n");
}
