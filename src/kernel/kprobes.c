/*
 * kprobes.c — Dynamic breakpoint insertion for kernel debugging (Item 203)
 *
 * Implements Kprobes: the ability to insert a breakpoint (INT3) at any
 * kernel instruction address, execute a pre-handler, single-step the
 * original instruction, and call a post-handler.
 *
 * Architecture:
 *   register_kprobe() patches the probed instruction's first byte to INT3
 *   (0xCC) using a safe text_poke mechanism.  When the INT3 fires:
 *     1. kprobe_int3_handler() is called by the IDT
 *     2. It saves a copy of the CPU registers
 *     3. Calls kp->pre_handler() if set
 *     4. If pre_handler returned CONTINUE, temporarily removes INT3,
 *        sets the Trap Flag (TF), and returns to the original instruction
 *     5. The CPU single-steps the original instruction
 *     6. kprobe_debug_handler() fires on the #DB exception
 *     7. Calls kp->post_handler() if set
 *     8. Re-patches INT3 at the probe address
 *
 * Text patching:
 *   We use a temporary writable mapping of the physical page containing
 *   the target instruction.  A small scratch region (TEXT_POKE_BASE) is
 *   reserved for this purpose.  This avoids modifying the read-only
 *   kernel text mapping directly.
 */

#define KERNEL_INTERNAL
#include "kprobes.h"
#include "idt.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "panic.h"

/* ── Scratch area for text_poke ──────────────────────────────────────
 *
 * We reserve a single page at TEXT_POKE_BASE in kernel virtual space.
 * To patch a kernel text byte:
 *   1. Phys = vmm_get_physaddr(target_page)
 *   2. Map phys → TEXT_POKE_BASE with WRITE permission
 *   3. Write the byte via TEXT_POKE_BASE + offset_in_page
 *   4. Unmap TEXT_POKE_BASE (or map it to a zero page to save phys)
 *   5. Flush TLB for both addresses
 *
 * Phys pages used for text_poke are NOT freed — they were already
 * allocated for the kernel text and are just temporarily aliased.
 *
 * NOTE: SMP safety — on multi-core, we must ensure no other CPU is
 *       executing the instruction being patched.  For now we assume
 *       that kprobe registration happens early or under a mutex that
 *       serialises with thread migration.  A full implementation would
 *       use stop_machine().
 */
#define TEXT_POKE_BASE  0xFFFFFF8000000000ULL  /* scratch VA in high half */
#define TEXT_POKE_PAGES 1

/* ── Kprobe table ──────────────────────────────────────────────────── */

static struct kprobe *g_kprobes[KPROBES_MAX];
static int g_kprobe_count = 0;
static int g_kprobes_initialized = 0;

/* Profiling counters */
static uint64_t g_kprobe_hits = 0;
static uint64_t g_kprobe_ss_hits = 0;

/* Temporary storage during single-step — the kprobe we're currently
 * stepping over.  Only one CPU can be single-stepping at a time via
 * this mechanism (global). */
static struct kprobe *g_current_stepping = NULL;

/* ── Text patching helpers ──────────────────────────────────────────── */

/* text_poke_read — safely read a byte from kernel text.
 * Returns the byte at @addr. */
static uint8_t text_poke_read(uint64_t addr) {
    return *(volatile uint8_t *)addr;
}

/* text_poke_write — safely write a byte to kernel text.
 * Uses a temporary writable mapping to avoid modifying read-only PTEs.
 * Returns 0 on success, -1 on failure. */
static int text_poke_write(uint64_t addr, uint8_t value) {
    uint64_t page_start = addr & ~(PAGE_SIZE - 1);
    uint64_t offset     = addr & (PAGE_SIZE - 1);

    /* Get the physical address of the target page */
    uint64_t phys = vmm_get_physaddr(page_start);
    if (!phys) {
        kprintf("[KPROBES] text_poke: no physical mapping for 0x%llX\n",
                (unsigned long long)addr);
        return -1;
    }

    /* Map the physical page at our scratch address with write permission */
    uint64_t poke_vaddr = TEXT_POKE_BASE;
    if (vmm_map_page(poke_vaddr, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE) < 0) {
        kprintf("[KPROBES] text_poke: vmm_map_page failed\n");
        return -1;
    }

    /* Write the byte */
    *(volatile uint8_t *)(poke_vaddr + offset) = value;

    /* Flush TLB for the scratch address and the original address */
    __asm__ volatile("invlpg (%0)" : : "r"(poke_vaddr) : "memory");
    __asm__ volatile("invlpg (%0)" : : "r"(page_start) : "memory");

    /* Unmap scratch — map to zero page or simply unmap */
    vmm_unmap_page(poke_vaddr);

    /* Memory barrier — ensure the write is visible before we continue */
    __sync_synchronize();

    return 0;
}

/* ── Kprobe table helpers ──────────────────────────────────────────── */

/* Find a kprobe by address.  Returns NULL if not found. */
static struct kprobe *kprobe_find(uint64_t addr) {
    for (int i = 0; i < KPROBES_MAX; i++) {
        if (g_kprobes[i] && (g_kprobes[i]->flags & KPROBE_FLAG_ACTIVE) &&
            g_kprobes[i]->addr == addr) {
            return g_kprobes[i];
        }
    }
    return NULL;
}

/* Find a free slot in the kprobe table.  Returns -1 if full. */
static int kprobe_find_free_slot(void) {
    for (int i = 0; i < KPROBES_MAX; i++) {
        if (!g_kprobes[i] || !(g_kprobes[i]->flags & KPROBE_FLAG_ACTIVE)) {
            return i;
        }
    }
    return -1;
}

/* Estimate the length of the instruction at @addr by reading opcode bytes.
 * This is a best-effort heuristic for single-step — we walk the REX/opcode
 * prefix chain and estimate based on common patterns.  A full
 * disassembler (like the one in the in-kernel C compiler) would be more
 * accurate.
 *
 * Returns 1..15 (x86-64 max instruction length), defaulting to 1.
 *
 * NOTE: This is a simplified instruction length decoder.  Real kprobes
 *       use a proper x86 instruction decoder (e.g., Linux's insn.c).
 *       For production use, integrate the kernel's existing decoder or
 *       the one from src/compiler/cc_parse.c.
 */
static int kprobe_estimate_insn_len(uint64_t addr) {
    /* Read the first byte of the instruction.  We assume it's safe to
     * read the original opcode since it's kernel .text. */
    uint8_t b0 = text_poke_read(addr);
    int len = 1;

    /* REX prefix (0x40-0x4F) */
    if (b0 >= 0x40 && b0 <= 0x4F) {
        len = 2;
        uint8_t b1 = text_poke_read(addr + 1);
        /* Two-byte opcode (0F xx) */
        if (b1 == 0x0F) {
            len = 3;
        }
        /* VEX3 prefix (0x0F 0x38/0x3A) */
        if (addr + 2 < addr + 15) {
            uint8_t b2 = text_poke_read(addr + 2);
            if (b2 == 0x38 || b2 == 0x3A) len = 4;
        }
        return len;
    }

    /* Two-byte opcode starting with 0x0F */
    if (b0 == 0x0F) {
        len = 2;
        uint8_t b1 = text_poke_read(addr + 1);
        /* VEX3 */
        if (b1 == 0x38 || b1 == 0x3A) {
            len = 3;
            if (addr + 3 < addr + 15) {
                uint8_t b2 = text_poke_read(addr + 3);
                if (b2 == 0x38 || b2 == 0x3A) len = 4;
            }
        }
        return len;
    }

    /* REP/REPNE prefixes (0xF2, 0xF3) */
    if (b0 == 0xF2 || b0 == 0xF3) {
        /* Likely a two-byte opcode after prefix */
        len = 3;
        return len;
    }

    /* Lock prefix (0xF0) */
    if (b0 == 0xF0) {
        len = 2;
        return len;
    }

    /* Operand-size override (0x66) */
    if (b0 == 0x66) {
        len = 2;
        return len;
    }

    /* Address-size override (0x67) */
    if (b0 == 0x67) {
        len = 2;
        return len;
    }

    /* CS/DS/ES/SS/FS/GS segment overrides (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65) */
    if (b0 == 0x26 || b0 == 0x2E || b0 == 0x36 || b0 == 0x3E ||
        b0 == 0x64 || b0 == 0x65) {
        len = 2;
        return len;
    }

    /* NOP (0x90) — single byte */
    if (b0 == 0x90)
        return 1;

    /* RET (0xC3), IRET (0xCF), SYSCALL (0x0F 0x05) already handled by 0x0F case */
    if (b0 == 0xC3 || b0 == 0xCB || b0 == 0xCA)
        return 1;

    /* INT3 (0xCC) */
    if (b0 == 0xCC)
        return 1;

    /* Near CALL (0xE8) — 5 bytes: E8 rel32 */
    if (b0 == 0xE8) return 5;

    /* Near JMP (0xE9) — 5 bytes */
    if (b0 == 0xE9) return 5;

    /* Short JMP (0xEB) — 2 bytes */
    if (b0 == 0xEB) return 2;

    /* Jcc rel8 (0x70-0x7F) — 2 bytes */
    if (b0 >= 0x70 && b0 <= 0x7F) return 2;

    /* Jcc rel32 (0x0F 0x8x) — 6 bytes, handled by 0x0F case above */

    /* MOV AL, imm8 (0xB0-0xB7), MOV reg, imm64 (0xB8-0xBF) */
    if (b0 >= 0xB0 && b0 <= 0xB7) return 2;  /* MOV r8, imm8 */
    if (b0 >= 0xB8 && b0 <= 0xBF) return 10; /* MOV r64, imm64 (REX.W + opcode + imm64) */

    /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP AL, imm8 — 2 bytes */
    if (b0 >= 0x04 && b0 <= 0x0D && b0 != 0x0A && b0 != 0x0B)
        return (b0 == 0x0C || b0 == 0x0D) ? 2 : 2;

    /* PUSH/POP reg (0x50-0x5F) */
    if (b0 >= 0x50 && b0 <= 0x5F) return 1;

    /* INC/DEC reg (0x40-0x4F without REX) — if we got here, treat as REX */
    /* Already handled as REX above */

    /* Default to a conservative estimate for common ALU ops:
     * Most single-byte opcodes with ModR/M are 2-4 bytes total.
     * We read a few bytes to guess the instruction format. */
    uint8_t b1 = 0;
    if (addr + 1 < addr + 15)
        b1 = text_poke_read(addr + 1);

    /* Check for ModR/M byte: b1 >= 0xC0 means register operand (no SIB/disp),
     * b1 < 0xC0 means memory operand (may have SIB + displacement). */
    if (b1 >= 0xC0) {
        /* Register form — just opcode + ModR/M */
        len = 2;
    } else {
        /* Memory form — at least opcode + ModR/M. Check ModR/M for SIB/disp.
         * Mod bits (top 2 bits of b1): 00 = no disp (unless r/m=101),
         * 01 = disp8, 10 = disp32, 11 = register. */
        int mod = (b1 >> 6) & 3;
        int rm  = b1 & 7;
        len = 2; /* opcode + ModR/M */

        if (mod == 0 && rm == 5)
            len += 4; /* RIP-relative: disp32 */
        else if (mod == 1)
            len += 1; /* disp8 */
        else if (mod == 2)
            len += 4; /* disp32 */

        /* SIB byte present when Mod != 11 and R/M == 4 */
        if (mod != 3 && rm == 4)
            len += 1; /* SIB byte */
    }

    /* Cap at 15 bytes (x86-64 max instruction length) */
    if (len > 15) len = 15;
    return len;
}

/* ── API implementation ────────────────────────────────────────────── */

void kprobes_init(void) {
    memset(g_kprobes, 0, sizeof(g_kprobes));
    g_kprobe_count = 0;
    g_current_stepping = NULL;
    g_kprobe_hits = 0;
    g_kprobe_ss_hits = 0;

    /* Verify the scratch page is unmapped */
    uint64_t p = vmm_get_physaddr(TEXT_POKE_BASE);
    if (p) {
        /* It already exists — unlikely in practice but handle it */
        kprintf("[KPROBES] Warning: TEXT_POKE_BASE already mapped (phys 0x%llX)\n",
                (unsigned long long)p);
    }

    kprintf("[OK] Kprobes initialized (%d max probes, scratch at 0x%llX)\n",
            KPROBES_MAX, (unsigned long long)TEXT_POKE_BASE);

    g_kprobes_initialized = 1;
}

int register_kprobe(struct kprobe *kp) {
    if (!kp || !kp->addr) {
        kprintf("[KPROBES] register_kprobe: NULL probe or addr\n");
        return -1;
    }

    if (!kp->pre_handler && !kp->post_handler) {
        kprintf("[KPROBES] register_kprobe: no handlers set\n");
        return -1;  /* at least one handler required */
    }

    if (!g_kprobes_initialized) {
        kprintf("[KPROBES] register_kprobe: kprobes not initialized\n");
        return -1;
    }

    /* Check if already registered */
    if (kprobe_find(kp->addr)) {
        kprintf("[KPROBES] register_kprobe: already probed at 0x%llX\n",
                (unsigned long long)kp->addr);
        return -1;
    }

    /* Find a free slot */
    int slot = kprobe_find_free_slot();
    if (slot < 0) {
        kprintf("[KPROBES] register_kprobe: table full (%d max)\n", KPROBES_MAX);
        return -1;
    }

    /* Save the original opcode */
    kp->orig_opcode = text_poke_read(kp->addr);
    kp->flags = KPROBE_FLAG_ACTIVE;

    /* Estimate the instruction length for single-stepping */
    kp->insn_len = kprobe_estimate_insn_len(kp->addr);

    /* Write INT3 (0xCC) at the probe address */
    if (text_poke_write(kp->addr, 0xCC) < 0) {
        kp->flags = 0;
        kprintf("[KPROBES] register_kprobe: text_poke_write failed at 0x%llX\n",
                (unsigned long long)kp->addr);
        return -1;
    }

    g_kprobes[slot] = kp;
    g_kprobe_count++;

    kprintf("[KPROBES] Registered probe at 0x%llX (opcode 0x%02X, insn_len %d, slot %d)\n",
            (unsigned long long)kp->addr, kp->orig_opcode, kp->insn_len, slot);

    return 0;
}

int unregister_kprobe(struct kprobe *kp) {
    if (!kp || !(kp->flags & KPROBE_FLAG_ACTIVE))
        return -1;

    /* Restore the original opcode */
    if (text_poke_write(kp->addr, kp->orig_opcode) < 0) {
        kprintf("[KPROBES] unregister_kprobe: text_poke_write failed at 0x%llX\n",
                (unsigned long long)kp->addr);
        return -1;
    }

    kp->flags = 0;

    /* Remove from the table */
    for (int i = 0; i < KPROBES_MAX; i++) {
        if (g_kprobes[i] == kp) {
            g_kprobes[i] = NULL;
            g_kprobe_count--;
            break;
        }
    }

    kprintf("[KPROBES] Unregistered probe at 0x%llX\n",
            (unsigned long long)kp->addr);

    return 0;
}

int kprobe_is_probed(uint64_t addr) {
    return (kprobe_find(addr) != NULL) ? 1 : 0;
}

/* ── Interrupt handlers ────────────────────────────────────────────── */

void kprobe_int3_handler(struct interrupt_frame *frame) {
    uint64_t rip = frame->rip;
    g_kprobe_hits++;

    /* The CPU pushes RIP pointing to the NEXT instruction after INT3.
     * Since INT3 is 1 byte, rip points to addr+1.  Find the probe by
     * checking rip - 1 (the address containing INT3). */
    uint64_t probe_addr = rip - 1;
    struct kprobe *kp = kprobe_find(probe_addr);

    if (!kp) {
        /* Unhandled INT3 — could be a debugger.  We treat it as a
         * non-kprobe INT3 and fall through to the default handler
         * (which panics). */
        kprintf("[KPROBES] Unhandled INT3 at 0x%llX (RIP=0x%llX)\n",
                (unsigned long long)probe_addr, (unsigned long long)rip);
        panic("Unhandled INT3 in kernel");
        return;
    }

    /* Call the pre-handler if set */
    if (kp->pre_handler) {
        int action = kp->pre_handler(kp, frame);
        if (action == KPROBE_ACTION_CALLBACK) {
            /* Skip single-stepping — just continue execution.
             * We need to advance RIP past the INT3 and restore
             * the original instruction.  Since we already patched
             * INT3, just advance RIP and return. */
            frame->rip = rip; /* RIP already points past INT3 */
            return;
        }
    }

    /* Single-step the original instruction:
     * 1. Remove INT3 (restore original opcode)
     * 2. Set RIP back to the original instruction address
     * 3. Set the Trap Flag (TF) in RFLAGS
     * 4. Record that we're single-stepping */

    /* Restore original opcode */
    text_poke_write(kp->addr, kp->orig_opcode);

    /* Set RIP back to the original instruction */
    frame->rip = kp->addr;

    /* Set Trap Flag (bit 8 in RFLAGS) */
    frame->rflags |= (1ULL << 8);

    /* Record which kprobe we're stepping */
    g_current_stepping = kp;

    /* Return from interrupt — the CPU will execute the original
     * instruction and then fire #DB due to TF. */
}

void kprobe_debug_handler(struct interrupt_frame *frame) {
    /* Check if we're single-stepping for a kprobe */
    if (!g_current_stepping) {
        /* Non-kprobe debug exception — ignore (debugger not supported) */
        return;
    }

    g_kprobe_ss_hits++;

    /* The single-step completed.  The probed instruction at
     * g_current_stepping->addr was executed.  Call post_handler. */
    struct kprobe *kp = g_current_stepping;
    g_current_stepping = NULL;

    /* Clear the Trap Flag so we don't keep single-stepping */
    frame->rflags &= ~(1ULL << 8);

    /* Call post-handler */
    if (kp->post_handler) {
        kp->post_handler(kp, frame);
    }

    /* Re-patch INT3 — the probe stays active.  The original opcode was
     * restored before single-stepping and the instruction executed.
     * Re-insert INT3 for future hits. */
    text_poke_write(kp->addr, 0xCC);
}
