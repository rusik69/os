#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "dmi.h"
#include "string.h"
#include "io.h"
static __attribute__((unused)) char dmi_bios_vendor[64] = "QEMU";
static __attribute__((unused)) char dmi_sys_vendor[64] = "QEMU";
static __attribute__((unused)) char dmi_product[64] = "Standard PC";
void dmi_init(void) {
    /* Scan BIOS area for DMI table signature */
    const char *sig = "_SM_";
    uint8_t *ptr = (uint8_t*)(uintptr_t)0xF0000;
    int found = 0;
    for (int off = 0; off < 0x10000 - 4; off += 16) {
        if (ptr[off] == '_' && memcmp(&ptr[off], sig, 4) == 0) {
            kprintf("[OK] DMI table found at 0xF%05x\n", (int)((uintptr_t)&ptr[off] & 0xFFFFF));
            found = 1;
            break;
        }
    }
    if (!found) kprintf("[--] No SMBIOS table found\n");
}
const char *dmi_get_bios_vendor(void) { return dmi_bios_vendor; }
const char *dmi_get_sys_vendor(void) { return dmi_sys_vendor; }

/* ── Stub: dmi_match ─────────────────────────────── */
int dmi_match(const char *slot, const char *value)
{
    (void)slot;
    (void)value;
    kprintf("[dmi] dmi_match: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dmi_get_system_info ─────────────────────────────── */
const char* dmi_get_system_info(int field)
{
    (void)field;
    kprintf("[dmi] dmi_get_system_info: not yet implemented\n");
    return -ENOSYS;
}
