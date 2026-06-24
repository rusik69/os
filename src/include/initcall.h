#ifndef INITCALL_H
#define INITCALL_H

/*
 * Initcall system — linker section based initialization ordering.
 *
 * Usage:
 *   static void my_init(void) { ... }
 *   device_initcall(my_init);
 *
 * Linker sections are emitted in order: pure, core, postcore, arch, subsys, device.
 * The linker.ld must include the .initcall section.
 */

typedef void (*initcall_t)(void);

/* Section names: each level gets its own section for ordering */
#define __define_initcall(fn, id) \
    static initcall_t __initcall_##fn##id \
    __attribute__((__used__, __section__(".initcall." #id))) = (initcall_t)fn

#define pure_initcall(fn)       __define_initcall(fn, 0)
#define core_initcall(fn)       __define_initcall(fn, 1)
#define postcore_initcall(fn)   __define_initcall(fn, 2)
#define arch_initcall(fn)       __define_initcall(fn, 3)
#define subsys_initcall(fn)     __define_initcall(fn, 4)
#define device_initcall(fn)     __define_initcall(fn, 5)

/* Documentation aliases — same behaviour, clarify intended ordering.
 * These don't create new linker sections; they use the existing level
 * that matches their documented role:
 *   fs_initcall   → subsys level (after arch, before device)
 *   late_initcall  → device level (last among built-in initcalls)
 */
#define fs_initcall(fn)         __define_initcall(fn, 4)
#define late_initcall(fn)       __define_initcall(fn, 5)

/* Called from kernel_main to run all initcalls in order */
void do_initcalls(void);

/* Linker symbols for initcall section boundaries */
extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

#endif /* INITCALL_H */
