#ifndef X2APIC_H
#define X2APIC_H

#include "types.h"

/* Initialize x2APIC: detect support and switch from xAPIC to x2APIC mode */
int x2apic_init(void);

/* Check if x2APIC is currently active */
int x2apic_is_active(void);

#endif /* X2APIC_H */
