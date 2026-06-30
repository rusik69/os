/*
 * Linux-compatible capability syscalls.
 *
 * Provides capget, capset, and related capability management syscalls
 * matching the Linux x86-64 ABI conventions.
 *
 * Each function returns (uint64_t)(int64_t)-errno on error, or a
 * non-negative value on success.
 */
#include "syscall.h"
#include "module.h"
#include "process.h"
#include "errno.h"
#include "uaccess.h"
#include "caps.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux-compatible capability syscalls (capget, capset, etc.)");
MODULE_AUTHOR("Ruslan Gustomiasov");

/*
 * Capability version negotiation helper.
 *
 * Linux capget() version negotiation:
 * 1. If header→version == _LINUX_CAPABILITY_VERSION_x, accept and return data.
 * 2. If header→version == some other value, the kernel overwrites
 *    header→version with LINUX_CAPABILITY_VERSION and returns -EINVAL.
 *    The caller must retry with the reported version.
 */
static int cap_version_negotiate(struct __user_cap_header_struct *hdr)
{
	uint32_t ver = hdr->version;

	if (ver == _LINUX_CAPABILITY_VERSION_1 ||
	    ver == _LINUX_CAPABILITY_VERSION_2 ||
	    ver == _LINUX_CAPABILITY_VERSION_3)
		return 0;

	/* Negotiate: tell caller what version we support */
	hdr->version = LINUX_CAPABILITY_VERSION;
	return -EINVAL;
}

/*
 * Fill capability data for a given process.
 *
 * For V1: return only the low 32 bits of each mask (legacy).
 * For V2/V3: return the full 64-bit capability set as two data structs.
 *
 * 'data' points to an array of struct __user_cap_data_struct[].
 *   data[0] = flags for caps  0-31
 *   data[1] = flags for caps 32-63  (V2/V3 only)
 *
 * Returns 0 on success, -EFAULT on copy_to_user failure.
 */
static int cap_fill_data(struct process *proc,
			 uint64_t data_addr,
			 uint32_t version)
{
	struct __user_cap_data_struct kdata[2];
	int ndata = (version == _LINUX_CAPABILITY_VERSION_1) ? 1 : 2;

	/* data[0]: low 32 bits of each mask (caps 0-31) */
	kdata[0].effective   = (uint32_t)(proc->cap_effective[0] & 0xFFFFFFFFULL);
	kdata[0].permitted   = (uint32_t)(proc->cap_permitted[0] & 0xFFFFFFFFULL);
	kdata[0].inheritable = (uint32_t)(proc->cap_inheritable[0] & 0xFFFFFFFFULL);

	if (ndata > 1) {
		/* data[1]: next 32 bits (caps 32-63) */
		uint64_t eff_hi   = (PROCESS_SYSCALL_CAP_WORDS > 1)
				    ? proc->cap_effective[1] : 0;
		uint64_t perm_hi = (PROCESS_SYSCALL_CAP_WORDS > 1)
				    ? proc->cap_permitted[1] : 0;
		uint64_t inh_hi  = (PROCESS_SYSCALL_CAP_WORDS > 1)
				    ? proc->cap_inheritable[1] : 0;
		kdata[1].effective   = (uint32_t)(eff_hi & 0xFFFFFFFFULL);
		kdata[1].permitted   = (uint32_t)(perm_hi & 0xFFFFFFFFULL);
		kdata[1].inheritable = (uint32_t)(inh_hi & 0xFFFFFFFFULL);
	}

	if (copy_to_user(data_addr, kdata,
			 (size_t)ndata * sizeof(struct __user_cap_data_struct)) < 0)
		return -EFAULT;

	return 0;
}

/* ── sys_capget — get process capabilities ────────────────────────────
 *
 * int capget(cap_user_header_t header, cap_user_data_t data);
 *
 * Get the capability sets of a process. The header specifies the
 * capability version and target PID.
 *
 * If header→pid == 0, returns capabilities of the calling process.
 * Otherwise, returns capabilities of the specified process (requires
 * the caller to have permission to see that process).
 *
 * Version negotiation: if header→version is not a known capability
 * version, the kernel overwrites header→version with the latest
 * supported version and returns -EINVAL. The caller should retry
 * with the reported version.
 *
 * Returns 0 on success, -EINVAL / -EPERM / -EFAULT / -ESRCH on error.
 */
uint64_t sys_capget(uint64_t header_addr, uint64_t data_addr)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct kdata[2];
	struct process *target, *caller;
	int ret;

	/* Validate and copy header from userspace */
	if (!header_addr || !data_addr)
		return (uint64_t)(int64_t)-EINVAL;

	if (copy_from_user(&hdr, header_addr, sizeof(hdr)) < 0)
		return (uint64_t)(int64_t)-EFAULT;

	/* Version negotiation */
	ret = cap_version_negotiate(&hdr);
	if (ret < 0) {
		/* Write back the negotiated version */
		if (copy_to_user(header_addr, &hdr, sizeof(hdr)) < 0)
			return (uint64_t)(int64_t)-EFAULT;
		return (uint64_t)(int64_t)ret;
	}

	/* Find target process */
	if (hdr.pid == 0) {
		target = process_get_current();
		if (!target)
			return (uint64_t)(int64_t)-ESRCH;
	} else {
		target = process_get_by_pid((uint32_t)hdr.pid);
		if (!target || target->state == PROCESS_UNUSED)
			return (uint64_t)(int64_t)-ESRCH;

		/* Check visibility: non-root callers cannot get caps
		 * of other processes they cannot see */
		caller = process_get_current();
		if (caller && caller->euid != 0) {
			if (!process_can_see(caller, target))
				return (uint64_t)(int64_t)-EPERM;
		}
	}

	/* Fill and write capability data */
	ret = cap_fill_data(target, data_addr, hdr.version);
	if (ret < 0)
		return (uint64_t)(int64_t)ret;

	(void)kdata; /* used in future extension — data array on stack for V2+ */
	return 0;
}
