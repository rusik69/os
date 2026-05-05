#include "intel_gpu.h"
#include "pci.h"
#include "printf.h"
#include "vmm.h"

#define INTEL_VENDOR_ID 0x8086
#define PCI_CLASS_DISPLAY 0x03

#define INTEL_GPU_MMIO_SIZE 0x200000ULL

/* Common Intel display registers present across several generations. */
#define INTEL_VGACNTRL 0x71400
#define INTEL_PIPEACONF 0x70008
#define INTEL_DSPACNTR  0x70180
#define INTEL_PIPEASRC  0x6001C

static struct intel_gpu_info g_gpu;
static volatile uint8_t *g_mmio;

static uint32_t intel_gpu_read32(uint32_t reg) {
    if (!g_mmio) return 0;
    return *(volatile uint32_t *)(g_mmio + reg);
}

static void intel_gpu_map_mmio(struct intel_gpu_info *gpu) {
    if (!gpu->mmio_base) return;
    for (uint64_t off = 0; off < gpu->mmio_size; off += PAGE_SIZE)
        vmm_map_page(gpu->mmio_base + off, gpu->mmio_base + off,
                     VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    g_mmio = (volatile uint8_t *)(uintptr_t)gpu->mmio_base;
    gpu->mmio_mapped = 1;
}

static void intel_gpu_read_state(struct intel_gpu_info *gpu) {
    if (!gpu->mmio_mapped) return;

    uint32_t vga = intel_gpu_read32(INTEL_VGACNTRL);
    uint32_t pipeconf = intel_gpu_read32(INTEL_PIPEACONF);
    uint32_t dspcntr = intel_gpu_read32(INTEL_DSPACNTR);
    uint32_t pipesrc = intel_gpu_read32(INTEL_PIPEASRC);

    gpu->vga_disabled = (vga >> 31) & 1;
    gpu->pipe_a_active = (pipeconf >> 31) & 1;
    gpu->plane_a_active = (dspcntr >> 31) & 1;

    if (pipesrc) {
        gpu->mode_width = (uint16_t)((pipesrc & 0x0FFFu) + 1);
        gpu->mode_height = (uint16_t)(((pipesrc >> 16) & 0x0FFFu) + 1);
    }
}

static int scan_intel_display(struct intel_gpu_info *out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
            if (vid == 0xFFFF) continue;

            uint32_t reg0c = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            uint8_t hdr = (uint8_t)((reg0c >> 16) & 0xFF);
            int nfunc = (hdr & 0x80) ? 8 : 1;

            for (int func = 0; func < nfunc; func++) {
                reg0 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x00);
                vid = (uint16_t)(reg0 & 0xFFFF);
                if (vid == 0xFFFF) continue;
                if (vid != INTEL_VENDOR_ID) continue;

                uint16_t did = (uint16_t)((reg0 >> 16) & 0xFFFF);
                uint32_t reg08 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                uint8_t cls = (uint8_t)((reg08 >> 24) & 0xFF);
                if (cls != PCI_CLASS_DISPLAY) continue;

                uint32_t irqr = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x3C);
                uint32_t bar0 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x10);
                uint32_t bar2 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x18);

                out->present = 1;
                out->bus = (uint8_t)bus;
                out->slot = (uint8_t)slot;
                out->func = (uint8_t)func;
                out->device_id = did;
                out->irq = (uint8_t)(irqr & 0xFF);
                out->mmio_base = (uint64_t)(bar0 & ~0xFULL);
                out->aperture_base = (uint64_t)(bar2 & ~0xFULL);
                out->stolen_mem_base = (uint64_t)(pci_read((uint8_t)bus, (uint8_t)slot,
                                                            (uint8_t)func, 0x5C) & ~0xFFFFFULL);
                out->mmio_size = (uint32_t)INTEL_GPU_MMIO_SIZE;
                return 0;
            }
        }
    }
    return -1;
}

int intel_gpu_init(void) {
    g_gpu.present = 0;
    g_mmio = 0;

    if (scan_intel_display(&g_gpu) < 0)
        return -1;

    /* Enable I/O, MMIO and bus mastering for the display device. */
    uint32_t cmd = pci_read(g_gpu.bus, g_gpu.slot, g_gpu.func, 0x04);
    cmd |= (1u << 0) | (1u << 1) | (1u << 2);
    pci_write(g_gpu.bus, g_gpu.slot, g_gpu.func, 0x04, cmd);

        intel_gpu_map_mmio(&g_gpu);
        intel_gpu_read_state(&g_gpu);

        kprintf("  Intel GPU %04x at %02x:%02x.%u mmio=0x%x aper=0x%x stolen=0x%x irq=%u\n",
            (uint64_t)g_gpu.device_id,
            (uint64_t)g_gpu.bus, (uint64_t)g_gpu.slot, (uint64_t)g_gpu.func,
            g_gpu.mmio_base, g_gpu.aperture_base, g_gpu.stolen_mem_base,
            (uint64_t)g_gpu.irq);
        if (g_gpu.mode_width && g_gpu.mode_height) {
        kprintf("  Intel GPU state: pipeA=%u planeA=%u vga=%s mode=%ux%u\n",
            (uint64_t)g_gpu.pipe_a_active,
            (uint64_t)g_gpu.plane_a_active,
            g_gpu.vga_disabled ? "disabled" : "enabled",
            (uint64_t)g_gpu.mode_width, (uint64_t)g_gpu.mode_height);
        } else {
        kprintf("  Intel GPU state: pipeA=%u planeA=%u vga=%s\n",
            (uint64_t)g_gpu.pipe_a_active,
            (uint64_t)g_gpu.plane_a_active,
            g_gpu.vga_disabled ? "disabled" : "enabled");
        }
    return 0;
}

int intel_gpu_is_present(void) {
    return g_gpu.present != 0;
}

const struct intel_gpu_info *intel_gpu_get_info(void) {
    return &g_gpu;
}
