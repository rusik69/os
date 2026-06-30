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
 * 1. If header->version == _LINUX_CAPABILITY_VERSION_x, accept and return data.
 * 2. If header->version == some other value, the kernel overwrites
 *    header->version with LINUX_CAPABILITY_VERSION and returns -EINVAL.
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
 * If header->pid == 0, returns capabilities of the calling process.
 * Otherwise, returns capabilities of the specified process (requires
 * the caller to have permission to see that process).
 *
 * Version negotiation: if header->version is not a known capability
 * version, the kernel overwrites header->version with the latest
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

/* ── sys_capset — set process capabilities ────────────────────────────
 *
 * int capset(cap_user_header_t header, const cap_user_data_t data);
 *
 * Set the capability sets of a process. The header specifies the
 * capability version and target PID.
 *
 * If header->pid == 0, sets capabilities of the calling process.
 * Otherwise, sets capabilities of the specified process (requires
 * CAP_SETPCAP in the caller's effective set).
 *
 * Permission rules (Linux-compatible):
 *   1. Setting caps on another process always requires CAP_SETPCAP.
 *   2. Setting caps on self:
 *      a. New permitted set must be a subset of the intersection of
 *         the caller's current permitted set and the bounding set.
 *      b. New inheritable set must be a subset of the caller's
 *         current inheritable set.
 *      c. New effective set must be a subset of the new permitted set.
 *
 * Returns 0 on success, -EINVAL / -EPERM / -EFAULT / -ESRCH on error.
 */
uint64_t sys_capset(uint64_t header_addr, uint64_t data_addr)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct kdata[2];
	struct process *target, *caller;
	uint64_t new_eff, new_perm, new_inh;
	uint64_t cur_perm, cur_inh;
	int ret, ndata;

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

	/* Number of data structs: V1 = 1, V2/V3 = 2 */
	ndata = (hdr.version == _LINUX_CAPABILITY_VERSION_1) ? 1 : 2;

	/* Copy capability data from userspace */
	if (copy_from_user(kdata, data_addr,
			   (size_t)ndata * sizeof(struct __user_cap_data_struct)) < 0)
		return (uint64_t)(int64_t)-EFAULT;

	/* Reconstruct full 64-bit capability masks from the data structs */
	new_eff   = (uint64_t)kdata[0].effective;
	new_perm  = (uint64_t)kdata[0].permitted;
	new_inh   = (uint64_t)kdata[0].inheritable;

	if (ndata > 1) {
		new_eff   |= (uint64_t)kdata[1].effective   << 32;
		new_perm  |= (uint64_t)kdata[1].permitted   << 32;
		new_inh   |= (uint64_t)kdata[1].inheritable << 32;
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
	}

	caller = process_get_current();

	/* Permission checks */
	if (hdr.pid != 0 && caller) {
		/* Setting caps on another process needs CAP_SETPCAP */
		int cword = CAP_SETPCAP / 64;
		int cbit  = CAP_SETPCAP % 64;
		if (cword < PROCESS_SYSCALL_CAP_WORDS &&
		    !(caller->syscall_caps[cword] & (1ULL << cbit)))
			return (uint64_t)(int64_t)-EPERM;
	}

	if (!caller)
		return (uint64_t)(int64_t)-EPERM;

	/* Save current permitted and inheritable sets for validation */
	cur_perm = caller->cap_permitted[0];
	cur_inh  = caller->cap_inheritable[0];

	/* ── Validate and apply ────────────────────────────────────── */

	/* If targeting another process, skip the self-capability
	 * restrictions — CAP_SETPCAP was already checked above.
	 * The kernel still applies the bounding set. */
	if (hdr.pid != 0) {
		/* Intersect with bounding set */
		new_perm &= sys_cap_bset_get_word(0);
		if (PROCESS_SYSCALL_CAP_WORDS > 1) {
			uint64_t new_perm_hi = new_perm >> 32;
			(void)new_perm_hi;
		}
		target->cap_permitted[0]   = new_perm;
		target->cap_inheritable[0] = new_inh;
		target->cap_effective[0]   = new_eff & new_perm;
		return 0;
	}

	/* ── Self-capset rules (pid == 0) ──────────────────────────── */
	/* Following Linux kernel cap_set_user() semantics:
	 *
	 * 1. New permitted set: caps added to permitted must be in
	 *    the caller's current permitted set AND the bounding set.
	 *
	 *    i.e. new_perm cannot contain bits outside
	 *         (cur_perm & sys_cap_bset[0]).
	 */
	{
		uint64_t source_perm = cur_perm & sys_cap_bset_get_word(0);
		if (new_perm & ~source_perm)
			return (uint64_t)(int64_t)-EPERM;
	}

	/*
	 * 2. New inheritable set: caps added to inheritable must be
	 *    in the caller's current inheritable set.
	 *
	 *    i.e. new_inh cannot contain bits outside cur_inh.
	 */
	if (new_inh & ~cur_inh)
		return (uint64_t)(int64_t)-EPERM;

	/*
	 * 3. New effective set must be a subset of new permitted set.
	 */
	if (new_eff & ~new_perm)
		return (uint64_t)(int64_t)-EPERM;

	/* Apply the new capability sets to the target (self) */
	target->cap_permitted[0]   = new_perm;
	target->cap_inheritable[0] = new_inh;
	target->cap_effective[0]   = new_eff;

	/* Apply bounding set to all cap sets */
	sys_cap_bset_apply(target);

	return 0;
}

/* ── sys_setsecurebits — set securebits for current process ────────────
 *
 * int setsecurebits(unsigned int bits);
 *
 * Set the securebits flags for the calling process. The bits argument
 * specifies which securebits to set. Locked bits cannot be changed once
 * set, and setting a locked bit requires the corresponding non-locked bit
 * to also be set.
 *
 * Returns 0 on success, -EINVAL on invalid bits, -EPERM on locked bits,
 * or -ESRCH if the current process cannot be determined.
 */
uint64_t sys_setsecurebits(uint64_t bits)
{
	struct process *p = process_get_current();

	if (!p)
		return (uint64_t)(int64_t)-ESRCH;

	/* Validate: only accept the low byte (securebits is uint8_t) */
	if (bits & ~0xFFULL)
		return (uint64_t)(int64_t)-EINVAL;

	int ret = securebits_set(p, (uint8_t)bits);
	if (ret < 0)
		return (uint64_t)(int64_t)ret;

	return 0;
}

/* ── sys_getsecurebits — get securebits for current process ────────────
 *
 * int getsecurebits(void);
 *
 * Returns the current securebits flags for the calling process,
 * or -ESRCH if the current process cannot be determined.
 */
uint64_t sys_getsecurebits(void)
{
	struct process *p = process_get_current();

	if (!p)
		return (uint64_t)(int64_t)-ESRCH;

	return (uint64_t)securebits_get(p);
}
