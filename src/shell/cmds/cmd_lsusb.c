#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"

/* cmd_lsusb — list USB devices */
void cmd_lsusb(void) {
    extern int usb_is_present(void);
    extern int usb_get_device_count(void);

    int present = usb_is_present();
    int devs = usb_get_device_count();

    if (!present || devs <= 0) {
        kprintf("USB: no controllers or devices detected\n");
        return;
    }

    kprintf("USB device listing:\n");
    kprintf("  Host controllers: %d\n", present ? 1 : 0);
    kprintf("  Devices: %d\n\n", devs);

    for (int i = 0; i < devs; i++) {
        extern void *usb_get_device(int idx);
        void *dev = usb_get_device(i);
        if (dev) {
            /* Access device fields via offset-based approach */
            uint8_t addr = *(uint8_t*)dev;
            uint8_t speed = *((uint8_t*)dev + 1);
            uint16_t vendor_id = *((uint16_t*)((uint8_t*)dev + 2));
            uint16_t product_id = *((uint16_t*)((uint8_t*)dev + 4));
            uint8_t class_code = *((uint8_t*)dev + 6);

            const char *speed_str = "full";
            if (speed == 1) speed_str = "low";
            else if (speed == 2) speed_str = "high";

            const char *class_str = "generic";
            if (class_code == 0x03) class_str = "HID";
            else if (class_code == 0x08) class_str = "mass-storage";
            else if (class_code == 0x09) class_str = "hub";
            else if (class_code == 0x01) class_str = "audio";

            kprintf("  Device %3d: ID %04x:%04x  Class: %s  Speed: %s\n",
                    addr, vendor_id, product_id,
                    class_str, speed_str);
        }
    }
}
