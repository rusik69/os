/* cmd_hwinfo.c — comprehensive hardware summary */
#include "shell_cmds.h"
#include "printf.h"
#include "pci.h"
#include "intel_gpu.h"
#include "io.h"
#include "string.h"

static uint8_t hw_cmos_read(uint8_t reg) {
    outb(0x70, reg & 0x7F);
    return inb(0x71);
}

void cmd_hwinfo(void) {
    uint32_t eax, ebx, ecx, edx;

    kprintf("=== Hardware Information ===\n");

    /* --- CPU --- */
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;
    kprintf("CPU vendor: %s\n", vendor);

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint8_t family   = (eax >> 8) & 0xF;
    uint8_t model    = (eax >> 4) & 0xF;
    uint8_t stepping = eax & 0xF;
    kprintf("CPU family/model/stepping: %u/%u/%u\n",
            (uint64_t)family, (uint64_t)model, (uint64_t)stepping);

    kprintf("CPU features:");
    if (edx & (1u<< 0)) kprintf(" FPU");
    if (edx & (1u<<25)) kprintf(" SSE");
    if (edx & (1u<<26)) kprintf(" SSE2");
    if (ecx & (1u<<19)) kprintf(" SSE4.1");
    if (ecx & (1u<<20)) kprintf(" SSE4.2");
    if (ecx & (1u<<25)) kprintf(" AES");
    if (ecx & (1u<<28)) kprintf(" AVX");
    if (edx & (1u<<28)) kprintf(" HTT");
    kprintf("\n");

    /* --- Memory (from CMOS) --- */
    uint16_t base_kb = ((uint16_t)hw_cmos_read(0x16) << 8) | hw_cmos_read(0x15);
    uint16_t ext_kb  = ((uint16_t)hw_cmos_read(0x18) << 8) | hw_cmos_read(0x17);
    kprintf("Base memory: %u KB\n", (uint64_t)base_kb);
    kprintf("Ext memory:  %u KB (%u MB)\n",
            (uint64_t)ext_kb, (uint64_t)(ext_kb / 1024));

    /* --- Serial ports --- */
    /* BDA (BIOS Data Area) holds COM port base addresses at 0x400 */
    uint16_t com1 = *(volatile uint16_t *)0x400;
    uint16_t com2 = *(volatile uint16_t *)0x402;
    kprintf("COM1: 0x%x  COM2: 0x%x\n",
            (uint64_t)com1, (uint64_t)com2);

        /* --- Intel GPU --- */
        if (intel_gpu_is_present()) {
                const struct intel_gpu_info *gpu = intel_gpu_get_info();
                kprintf("Intel GPU: %04x at %02x:%02x.%u (MMIO=0x%x, APER=0x%x, STOLEN=0x%x, IRQ=%u)\n",
                                (uint64_t)gpu->device_id,
                                (uint64_t)gpu->bus, (uint64_t)gpu->slot, (uint64_t)gpu->func,
                                gpu->mmio_base, gpu->aperture_base, gpu->stolen_mem_base,
                                (uint64_t)gpu->irq);
                kprintf("Intel GPU state: MMIO=%s, pipeA=%u, planeA=%u, VGA=%s",
                                gpu->mmio_mapped ? "mapped" : "off",
                                (uint64_t)gpu->pipe_a_active,
                                (uint64_t)gpu->plane_a_active,
                                gpu->vga_disabled ? "disabled" : "enabled");
                if (gpu->mode_width && gpu->mode_height)
                        kprintf(", mode=%ux%u", (uint64_t)gpu->mode_width, (uint64_t)gpu->mode_height);
                kprintf("\n");
        } else {
                kprintf("Intel GPU: not detected\n");
        }

    /* --- PCI devices --- */
    kprintf("PCI devices:\n");
    pci_list();
}
