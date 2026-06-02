#ifndef VERMAGIC_H
#define VERMAGIC_H

/*
 * vermagic.h — Module version magic strings.
 *
 * The vermagic (version magic) is a string embedded into every .ko
 * module file that records the kernel version and build configuration
 * flags under which the module was compiled.  At load time, the module
 * loader compares the module's vermagic against the running kernel's
 * vermagic.  A mismatch causes the load to be rejected, preventing
 * subtle corruption from ABI incompatibilities (e.g., SMP vs UP,
 * preemption model, compiler version, architecture options).
 *
 * The canonical format (inspired by Linux) is:
 *
 *   <version> SMP [preempt|preempt_voluntary|no_preempt] <arch>
 *
 * Components:
 *   version          — kernel version string (e.g., "6.1.0-osdev")
 *   SMP              — present if CONFIG_SMP is set
 *   preempt/...      — preemption model indicator
 *   arch             — architecture name (e.g., "x86_64")
 *
 * Additional flags can be appended as needed (e.g., "gcc-13", "lto").
 */

/* ── Build-time version string (set via Makefile -DKVERSION="...") ── */
#ifndef KVERSION
#define KVERSION  "6.1.0-osdev"
#endif

/* Architecture string */
#define VERMAGIC_ARCH  "x86_64"

/*
 * Construct the base vermagic string.
 *
 * Format:  <KVERSION> SMP preempt:<model> <ARCH>
 *
 * CONFIG_SMP, CONFIG_PREEMPT, etc. are set via Makefile -D flags and
 * reflect the running kernel's configuration.  Modules built with
 * different settings will have a different vermagic and be rejected.
 */
#ifdef CONFIG_SMP
#define VERMAGIC_SMP  "SMP "
#else
#define VERMAGIC_SMP  ""
#endif

#ifdef CONFIG_PREEMPT
#define VERMAGIC_PREEMPT  "preempt "
#elif defined(CONFIG_PREEMPT_VOLUNTARY)
#define VERMAGIC_PREEMPT  "preempt_voluntary "
#else
#define VERMAGIC_PREEMPT  "no_preempt "
#endif

/*
 * VERMAGIC_STRING — the full version magic string for this kernel build.
 *
 * This is embedded in the kernel image itself (via module_vermagic[])
 * and also passed to modules during compilation so they embed the same
 * string in their .modinfo section.
 */
#define VERMAGIC_STRING  KVERSION " " VERMAGIC_SMP VERMAGIC_PREEMPT VERMAGIC_ARCH

/*
 * MODULE_VERMAGIC — embed vermagic info in a module's .modinfo section.
 *
 * Modules should use this exactly once in their main source file.
 * When the module is loaded, the kernel compares this string against
 * its own VERMAGIC_STRING and rejects the module on mismatch.
 *
 * Usage:  MODULE_VERMAGIC(VERMAGIC_STRING);
 */
#define MODULE_VERMAGIC(v)  static const char __mod_vermagic[]  \
    __attribute__((section(".modinfo"), used)) = "vermagic=" v

/* ── Module name helper (used by ELF loader) ────────────────────── */
#define MODULE_NAME(name)    static const char __mod_name[]  \
    __attribute__((section(".modinfo"), used)) = "name=" name

#endif /* VERMAGIC_H */
