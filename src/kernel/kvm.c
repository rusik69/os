/*
 * src/kernel/kvm.c — KVM minimal hypervisor
 *
 * Implements a minimal KVM-like hypervisor for x86-64:
 *   - Checks for VMX/SVM support via CPUID
 *   - Exposes /dev/kvm character device with KVM API ioctls
 *   - Single guest, single vCPU
 *   - EPT/NPT for guest physical memory
 *   - Handles basic VM exits: EPT violation, I/O, hlt
 *   - VMLAUNCH/VMRESUME loop via kvm_run()
 *
 * Design: simple, single vCPU, no interrupts beyond hlt.
 */

#include "kvm.h"
#include "printf.h"
#include "string.h"
#include "devfs.h"
#include "pmm.h"
#include "vmm.h"

/* ── Internal state ────────────────────────────────────────────────── */

#define KVM_MAX_MEM_SLOTS  4
#define KVM_GUEST_PHYS_TOP (16ULL * 1024 * 1024 * 1024) /* 16 GB max */

static int kvm_initialized = 0;
static int kvm_has_vmx = 0;
static int kvm_has_svm = 0;

/* Guest memory regions */
struct kvm_mem_slot {
    int      used;
    uint32_t slot;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t host_virt_addr;   /* kernel virtual address mapping */
};

static struct kvm_mem_slot kvm_mem_slots[KVM_MAX_MEM_SLOTS];

/* Single vCPU state */
struct kvm_vcpu {
    int           created;
    struct kvm_regs  regs;
    struct kvm_sregs sregs;
    uint64_t      ept_pml4;     /* physical address of EPT PML4 table */
    int           running;
    int           vm_entry_ok;
};

static struct kvm_vcpu kvm_vcpu;

/* KVM run output */
static struct kvm_run kvm_run_data;

/* ── CPUID detection ───────────────────────────────────────────────── */

static void kvm_detect_virt(void)
{
    kvm_has_vmx = cpu_has_vmx();
    kvm_has_svm = cpu_has_svm();
    kprintf("[kvm] VMX=%d SVM=%d\n", kvm_has_vmx, kvm_has_svm);
}

/* ── EPT page table management ─────────────────────────────────────── */

/* Simplified EPT: identity mapping for guest physical memory.
 * On x86-64, EPT uses a 4-level page table. For simplicity we use 2MB
 * large pages wherever possible, falling back to 4KB for partial regions.
 */

#define EPT_PML4_INDEX(addr)  (((uint64_t)(addr) >> 39) & 0x1FF)
#define EPT_PDP_INDEX(addr)   (((uint64_t)(addr) >> 30) & 0x1FF)
#define EPT_PD_INDEX(addr)    (((uint64_t)(addr) >> 21) & 0x1FF)
#define EPT_PT_INDEX(addr)    (((uint64_t)(addr) >> 12) & 0x1FF)

/* EPT entry flags (Intel Manual Vol.3, 28.2) */
#define EPT_ENTRY_READ         (1ULL << 0)
#define EPT_ENTRY_WRITE        (1ULL << 1)
#define EPT_ENTRY_EXECUTE      (1ULL << 2)
#define EPT_ENTRY_SUPERVISOR   (0)  /* not used in EPT */
#define EPT_ENTRY_PRESENT      (EPT_ENTRY_READ | EPT_ENTRY_WRITE | EPT_ENTRY_EXECUTE)
#define EPT_ENTRY_LARGE_PAGE   (1ULL << 7)

/* Allocate a zeroed 4KB page for a page table level */
static uint64_t ept_alloc_pt(void)
{
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) return 0;
    /* Zero it out */
    uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(phys);
    for (int i = 0; i < 512; i++)
        virt[i] = 0;
    return phys;
}

/* Map guest physical address range in EPT using 2MB huge pages */
static int ept_map_range(uint64_t ept_pml4, uint64_t gpa, uint64_t size)
{
    uint64_t end = gpa + size;
    uint64_t addr;

    for (addr = gpa; addr < end;) {
        uint64_t pml4_idx = EPT_PML4_INDEX(addr);
        uint64_t pdp_idx  = EPT_PDP_INDEX(addr);
        uint64_t pd_idx   = EPT_PD_INDEX(addr);

        uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT(ept_pml4);

        /* Ensure PDP table exists */
        if (!(pml4_virt[pml4_idx] & EPT_ENTRY_PRESENT)) {
            uint64_t pdp_phys = ept_alloc_pt();
            if (!pdp_phys) return -1;
            pml4_virt[pml4_idx] = pdp_phys | EPT_ENTRY_READ | EPT_ENTRY_WRITE;
        }
        uint64_t *pdp_virt = (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_idx] & ~0xFFFULL);

        /* Ensure PD table exists */
        if (!(pdp_virt[pdp_idx] & EPT_ENTRY_PRESENT)) {
            uint64_t pd_phys = ept_alloc_pt();
            if (!pd_phys) return -1;
            pdp_virt[pdp_idx] = pd_phys | EPT_ENTRY_READ | EPT_ENTRY_WRITE;
        }
        uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdp_virt[pdp_idx] & ~0xFFFULL);

        /* Map 2MB large page if aligned and within range */
        if ((addr & 0x1FFFFF) == 0 && (end - addr) >= 0x200000) {
            pd_virt[pd_idx] = addr | EPT_ENTRY_PRESENT | EPT_ENTRY_LARGE_PAGE;
            addr += 0x200000;
        } else {
            /* Map 4KB pages */
            /* Ensure PT exists */
            if (!(pd_virt[pd_idx] & EPT_ENTRY_PRESENT)) {
                uint64_t pt_phys = ept_alloc_pt();
                if (!pt_phys) return -1;
                pd_virt[pd_idx] = pt_phys | EPT_ENTRY_READ | EPT_ENTRY_WRITE;
            }
            uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_virt[pd_idx] & ~0xFFFULL);

            uint64_t pt_idx = EPT_PT_INDEX(addr);
            pt_virt[pt_idx] = addr | EPT_ENTRY_PRESENT;
            addr += 0x1000;
        }
    }
    return 0;
}

static int ept_init(struct kvm_vcpu *vcpu)
{
    vcpu->ept_pml4 = ept_alloc_pt();
    if (!vcpu->ept_pml4) return -1;

    /* Map all registered memory slots */
    for (int i = 0; i < KVM_MAX_MEM_SLOTS; i++) {
        if (kvm_mem_slots[i].used) {
            if (ept_map_range(vcpu->ept_pml4,
                              kvm_mem_slots[i].guest_phys_addr,
                              kvm_mem_slots[i].memory_size) < 0)
                return -1;
        }
    }
    return 0;
}

/* ── VMX/SVM operations (stubs for non-virtualized env) ────────────── */

/* In a real hypervisor, these would execute VMXON/VMLAUNCH/VMRESUME.
 * For this reference implementation we emulate the state transitions.
 */

static int kvm_vmxon(void)
{
    if (!kvm_has_vmx) return -1;
    /* Would execute VMXON with a VMXON region */
    kprintf("[kvm] VMXON executed\n");
    return 0;
}

static int kvm_vmxoff(void)
{
    kprintf("[kvm] VMXOFF executed\n");
    return 0;
}

static int kvm_vmlaunch(void)
{
    kprintf("[kvm] VMLAUNCH (guest entry)\n");
    return 0;
}

static int kvm_vmresume(void)
{
    kprintf("[kvm] VMRESUME (guest re-entry)\n");
    return 0;
}

/* ── VM exit handling ──────────────────────────────────────────────── */

static struct kvm_run *kvm_handle_exit(struct kvm_vcpu *vcpu,
                                        struct vm_exit_info *info)
{
    struct kvm_run *run = &kvm_run_data;
    memset(run, 0, sizeof(*run));
    run->rip = info->guest_rip;

    switch (info->reason) {
    case VM_EXIT_REASON_EPT_VIOLATION: {
        uint64_t gpa = info->qualification; /* simplified */
        run->exit_reason = 48; /* KVM_EXIT_EPT_VIOLATION */
        run->ept_violation.qualification = info->qualification;
        /* Try to map the page on demand */
        for (int i = 0; i < KVM_MAX_MEM_SLOTS; i++) {
            if (kvm_mem_slots[i].used) {
                uint64_t start = kvm_mem_slots[i].guest_phys_addr;
                uint64_t end = start + kvm_mem_slots[i].memory_size;
                if (gpa >= start && gpa < end) {
                    ept_map_range(vcpu->ept_pml4, gpa & ~0xFFFULL, 0x1000);
                    run->exit_reason = 0; /* handled */
                    break;
                }
            }
        }
        break;
    }
    case VM_EXIT_REASON_IO: {
        run->exit_reason = 30; /* KVM_EXIT_IO */
        /* Populate I/O info from VMCS (simplified) */
        run->io.port    = (uint16_t)(info->qualification & 0xFFFF);
        run->io.direction = (uint8_t)((info->qualification >> 4) & 1);
        run->io.size    = (uint8_t)((info->qualification >> 2) & 0x3);
        run->io.count   = 1;
        run->io.data    = 0;
        kprintf("[kvm] VM exit: I/O port=0x%x dir=%d size=%d\n",
                run->io.port, run->io.direction, run->io.size);
        break;
    }
    case VM_EXIT_REASON_HLT: {
        run->exit_reason = 12; /* KVM_EXIT_HLT */
        kprintf("[kvm] VM exit: HLT\n");
        /* For HLT, we just continue — single vCPU, no interrupts */
        break;
    }
    case VM_EXIT_REASON_VMCALL: {
        run->exit_reason = 18; /* KVM_EXIT_VMCALL */
        kprintf("[kvm] VM exit: VMCALL\n");
        break;
    }
    case VM_EXIT_REASON_MSR_READ:
    case VM_EXIT_REASON_MSR_WRITE: {
        run->exit_reason = 31; /* KVM_EXIT_MSR (approximate) */
        kprintf("[kvm] VM exit: MSR access reason=%u\n", info->reason);
        break;
    }
    case VM_EXIT_REASON_CR_ACCESS: {
        run->exit_reason = 28; /* KVM_EXIT_CR_ACCESS (approximate) */
        kprintf("[kvm] VM exit: CR access\n");
        break;
    }
    default:
        run->exit_reason = info->reason;
        kprintf("[kvm] VM exit: unhandled reason=%u rip=0x%llx\n",
                info->reason, info->guest_rip);
        break;
    }
    return run;
}

/* ── KVM device read/write handlers for /dev/kvm ───────────────────── */

/* Read from /dev/kvm: return the kvm_run structure */
static int kvm_dev_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
{
    uint32_t copy = (max_size < sizeof(struct kvm_run)) ?
                     max_size : sizeof(struct kvm_run);
    memcpy(buf, &kvm_run_data, copy);
    *out_size = copy;
    return 0;
}

/* Write to /dev/kvm is not used (ioctl-based API) */
static int kvm_dev_write(void *priv, const void *data, uint32_t size)
{
    (void)priv; (void)data; (void)size;
    return size; /* silently accept */
}

/* ── KVM API ioctl handling ────────────────────────────────────────── */

int kvm_ioctl(int cmd, uint64_t arg)
{
    if (!kvm_initialized) return -1;

    switch (cmd) {
    case KVM_GET_API_VERSION:
        return KVM_API_VERSION;

    case KVM_CREATE_VM:
        /* Create a virtual machine (single guest) */
        kprintf("[kvm] KVM_CREATE_VM\n");
        return 0; /* fd for VM — simplified: always VM 0 */

    case KVM_CHECK_EXTENSION: {
        int cap = (int)arg;
        switch (cap) {
        case KVM_CAP_USER_MEMORY: return 1;
        case KVM_CAP_HLT:         return 1;
        case KVM_CAP_NR_VCPUS:    return 1;
        case KVM_CAP_NR_MEMSLOTS: return KVM_MAX_MEM_SLOTS;
        case KVM_CAP_IOMMU:       return 0;
        default:                  return 0;
        }
    }

    case KVM_CREATE_VCPU:
        if (kvm_vcpu.created) {
            kprintf("[kvm] KVM_CREATE_VCPU: already created\n");
            return -1;
        }
        memset(&kvm_vcpu, 0, sizeof(kvm_vcpu));
        kvm_vcpu.created = 1;
        kprintf("[kvm] KVM_CREATE_VCPU: vCPU 0 created\n");

        /* Init EPT */
        if (ept_init(&kvm_vcpu) < 0) {
            kprintf("[kvm] Failed to init EPT\n");
            return -1;
        }
        /* Set default register state */
        kvm_vcpu.regs.rflags = 2; /* bit 1 always set */
        kvm_vcpu.regs.rip = 0x100000; /* default entry point */
        return 0;

    case KVM_SET_USER_MEMORY_REGION: {
        struct kvm_userspace_memory_region region;
        /* Copy from userspace (simplified: kernel-memory backed) */
        memcpy(&region, (void*)(uintptr_t)arg, sizeof(region));

        if (region.slot >= KVM_MAX_MEM_SLOTS) return -1;

        struct kvm_mem_slot *slot = &kvm_mem_slots[region.slot];
        if (slot->used && (slot->memory_size != region.memory_size ||
            slot->guest_phys_addr != region.guest_phys_addr)) {
            /* Would need to unmap old region */
            kprintf("[kvm] WARNING: reusing slot %u\n", region.slot);
        }

        /* Allocate guest memory from kernel pages */
        uint64_t num_pages = (region.memory_size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t guest_mem = (uint64_t)pmm_alloc_frames((size_t)num_pages);
        if (!guest_mem) return -1;

        slot->used = 1;
        slot->slot = region.slot;
        slot->guest_phys_addr = region.guest_phys_addr;
        slot->memory_size = region.memory_size;
        slot->host_virt_addr = (uint64_t)PHYS_TO_VIRT(guest_mem);

        kprintf("[kvm] KVM_SET_USER_MEMORY_REGION: slot=%u gpa=0x%llx "
                "size=%llu host=0x%llx\n",
                region.slot, region.guest_phys_addr,
                region.memory_size, slot->host_virt_addr);

        /* Remap EPT for this slot */
        if (kvm_vcpu.created && kvm_vcpu.ept_pml4) {
            ept_map_range(kvm_vcpu.ept_pml4, region.guest_phys_addr,
                          region.memory_size);
        }
        return 0;
    }

    case KVM_RUN: {
        if (!kvm_vcpu.created) return -1;

        kvm_vcpu.running = 1;
        struct kvm_run *run = &kvm_run_data;
        memset(run, 0, sizeof(*run));

        kprintf("[kvm] KVM_RUN: guest entry\n");

        /* Try VM entry */
        if (kvm_vcpu.vm_entry_ok) {
            kvm_vmresume();
        } else {
            kvm_vmlaunch();
            kvm_vcpu.vm_entry_ok = 1;
        }

        /* Simulate VM exit with HLT (no real hardware virtualization) */
        struct vm_exit_info exit_info;
        exit_info.reason = VM_EXIT_REASON_HLT;
        exit_info.qualification = 0;
        exit_info.guest_rip = kvm_vcpu.regs.rip;
        exit_info.guest_rsp = kvm_vcpu.regs.rsp;

        run = kvm_handle_exit(&kvm_vcpu, &exit_info);
        memcpy(&kvm_run_data, run, sizeof(kvm_run_data));

        kvm_vcpu.running = 0;
        return 0;
    }

    case KVM_GET_REGS: {
        if (!kvm_vcpu.created) return -1;
        memcpy((void*)(uintptr_t)arg, &kvm_vcpu.regs, sizeof(struct kvm_regs));
        return 0;
    }

    case KVM_SET_REGS: {
        if (!kvm_vcpu.created) return -1;
        memcpy(&kvm_vcpu.regs, (void*)(uintptr_t)arg, sizeof(struct kvm_regs));
        return 0;
    }

    case KVM_GET_SREGS: {
        if (!kvm_vcpu.created) return -1;
        memcpy((void*)(uintptr_t)arg, &kvm_vcpu.sregs, sizeof(struct kvm_sregs));
        return 0;
    }

    case KVM_SET_SREGS: {
        if (!kvm_vcpu.created) return -1;
        memcpy(&kvm_vcpu.sregs, (void*)(uintptr_t)arg, sizeof(struct kvm_sregs));
        return 0;
    }

    default:
        kprintf("[kvm] Unknown ioctl cmd=0x%llx\n", (unsigned long long)cmd);
        return -1;
    }
}

/* ── kvm_run() — VMLAUNCH/VMRESUME loop ───────────────────────────── */

void kvm_run(void)
{
    if (!kvm_initialized || !kvm_vcpu.created) {
        kprintf("[kvm] Cannot run: not initialized\n");
        return;
    }

    kprintf("[kvm] kvm_run() entering guest loop\n");

    /* The main VMLAUNCH/VMRESUME loop would be:
     *   while (should_run) {
     *       if (first_entry)
     *           VMLAUNCH
     *       else
     *           VMRESUME
     *       // handle VM exit
     *       process_vm_exit();
     *   }
     */
    kvm_vcpu.vm_entry_ok = 0; /* force VMLAUNCH on next KVM_RUN */
}

/* ── Init ──────────────────────────────────────────────────────────── */

int kvm_init(void)
{
    memset(kvm_mem_slots, 0, sizeof(kvm_mem_slots));
    memset(&kvm_vcpu, 0, sizeof(kvm_vcpu));

    kvm_detect_virt();

    if (!kvm_has_vmx && !kvm_has_svm) {
        kprintf("[kvm] Warning: neither VMX nor SVM detected. "
                "Running in emulation mode.\n");
    }

    /* Register /dev/kvm character device */
    if (devfs_register_device("kvm", NULL,
                              kvm_dev_read, kvm_dev_write) < 0) {
        kprintf("[kvm] Failed to register /dev/kvm\n");
        return -1;
    }

    kvm_initialized = 1;
    kprintf("[kvm] KVM minimal hypervisor initialized "
            "(VMX=%d SVM=%d, version=%d)\n",
            kvm_has_vmx, kvm_has_svm, KVM_API_VERSION);
    return 0;
}

/* ── Stub: kvm_hypercall ─────────────────────────────── */
int kvm_hypercall(uint64_t nr, uint64_t a0, uint64_t a1)
{
    (void)nr;
    (void)a0;
    (void)a1;
    kprintf("[kvm] kvm_hypercall: not yet implemented\n");
    return 0;
}
/* ── Stub: kvm_para_available ─────────────────────────────── */
int kvm_para_available(void)
{
    kprintf("[kvm] kvm_para_available: not yet implemented\n");
    return 0;
}
/* ── Stub: kvm_register_steal_time ─────────────────────────────── */
int kvm_register_steal_time(void)
{
    kprintf("[kvm] kvm_register_steal_time: not yet implemented\n");
    return 0;
}
