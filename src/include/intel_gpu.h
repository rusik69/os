#ifndef INTEL_GPU_H
#define INTEL_GPU_H

#include "types.h"

struct intel_gpu_info {
    uint8_t present;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t device_id;
    uint8_t irq;
    uint64_t mmio_base;      /* BAR0, MMIO registers */
    uint64_t aperture_base;  /* BAR2, graphics aperture if exposed */
    uint64_t stolen_mem_base;
    uint32_t mmio_size;
    uint16_t mode_width;
    uint16_t mode_height;
    uint8_t mmio_mapped;
    uint8_t pipe_a_active;
    uint8_t plane_a_active;
    uint8_t vga_disabled;
};

int intel_gpu_init(void);
int intel_gpu_is_present(void);
const struct intel_gpu_info *intel_gpu_get_info(void);

#endif
