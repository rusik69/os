/*
 * sys_module.c — Legacy module syscall implementations
 *
 * Provides legacy syscalls for Linux ABI compatibility:
 *   sys_create_module    — legacy module creation (Linux 2.4 era)
 *   sys_get_kernel_syms  — legacy kernel symbol query
 *
 * These are deprecated/legacy interfaces. The modern equivalents are:
 *   finit_module(2) / init_module(2) for loading modules
 *   /proc/kallsyms for kernel symbol lookup
 *
 * Modern module syscalls (finit_module, delete_module, query_module)
 * are implemented in syscall.c alongside the main dispatch table.
 */

#define KERNEL_INTERNAL
#include "syscall.h"
#include "module.h"
#include "uaccess.h"
#include "lockdown.h"
#include "caps.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Legacy module syscall implementations (create_module, get_kernel_syms)");
MODULE_AUTHOR("OS Kernel Team");

/*
 * sys_create_module — Legacy module creation syscall
 *
 *   @name_addr:  User-space pointer to module name string.
 *   @size:       Requested module size in bytes (unused, allocation is
 *                handled by the modern ELF module loader).
 *
 * This is a legacy syscall from the Linux 2.4 era, superseded by
 * finit_module(2) / init_module(2).  The modern ELF-based module
 * loader handles all memory allocation internally during the
 * validate-parse-finalize sequence.
 *
 * Returns:
 *   -ENOSYS    — always; this interface is not implemented.
 *   -EPERM     — caller lacks permission (not root, or lockdown active).
 *   -EFAULT    — invalid user-space pointer for name.
 *   -EINVAL    — empty name string.
 */
uint64_t sys_create_module(uint64_t name_addr, uint64_t size)
{
	(void)size;

	/* Lockdown: reject module operations at INTEGRITY level or above */
	if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
		return (uint64_t)-EPERM;

	/* CAP_SYS_MODULE check */
	{
		int cap_ret = cap_sys_module_check();
		if (cap_ret < 0)
			return (uint64_t)(int64_t)cap_ret;
	}

	/* Only root can load modules */
	if (process_get_current()) {
		struct process *p = process_get_current();
		if (p->is_user && p->euid != 0)
			return (uint64_t)-EPERM;
	}

	/* Validate user-space name pointer */
	if (name_addr != 0) {
		char buf[64];
		long ret = strncpy_from_user(buf, name_addr, sizeof(buf));
		if (ret < 0)
			return (uint64_t)-EFAULT;
		if (ret == 0 || buf[0] == '\0')
			return (uint64_t)-EINVAL;
		kprintf("[MOD] sys_create_module(\"%s\", %llu): deprecated, use finit_module instead\n",
			buf, (unsigned long long)size);
	} else {
		kprintf("[MOD] sys_create_module(NULL, %llu): deprecated, use finit_module instead\n",
			(unsigned long long)size);
	}

	return (uint64_t)(int64_t)-ENOSYS;
}

/*
 * sys_get_kernel_syms — Legacy kernel symbol query syscall
 *
 *   @table_addr:  User-space pointer to struct kernel_sym array (output).
 *                 In the legacy interface this pointed to an array of
 *                 struct kernel_sym entries filled by the kernel.
 *
 * This is a legacy syscall from the Linux 2.4 era, removed in Linux 2.6.
 * Modern kernels provide symbol information through /proc/kallsyms and
 * the module loader's internal ksym resolution (ksym.c).
 *
 * Returns:
 *   -ENOSYS    — always; this interface is not implemented.
 *   -EPERM     — caller lacks permission (not root, or lockdown active).
 */
uint64_t sys_get_kernel_syms(uint64_t table_addr)
{
	(void)table_addr;

	/* Lockdown CONFIDENTIALITY: block kernel symbol leak */
	if (lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY))
		return (uint64_t)-EPERM;

	/* Root-only access */
	struct process *p = process_get_current();
	if (p && p->is_user && p->euid != 0)
		return (uint64_t)-EPERM;

	kprintf("[MOD] sys_get_kernel_syms: deprecated interface, use /proc/kallsyms\n");

	return (uint64_t)(int64_t)-ENOSYS;
}
