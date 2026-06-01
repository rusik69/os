#ifndef DMI_H
#define DMI_H
#include "types.h"
void dmi_init(void);
const char *dmi_get_bios_vendor(void);
const char *dmi_get_sys_vendor(void);
#endif
