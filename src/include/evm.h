#ifndef EVM_H
#define EVM_H

/* Extended Verification Module (EVM) — verify integrity of protected
 * extended attributes on access.  EVM signs the security.* xattrs of a
 * file with an HMAC-SHA256 (stored in security.evm); reads of protected
 * xattrs re-verify the HMAC to detect offline tampering.
 *
 * evm_verify_get() is invoked from vfs_getxattr() before a protected
 * xattr value is returned to a caller.
 *
 * @path:       absolute path of the file.
 * @xattr_name: name of the xattr being read.
 *
 * Returns 0 to allow the read, or a negative errno (-EACCES) to deny a
 * tampered file when EVM enforcement is enabled.  Files with no
 * security.evm xattr (un-protected) are always allowed through.
 */
int evm_verify_get(const char *path, const char *xattr_name);

/* Protect the security.evm xattr during setxattr.  Called before a
 * setxattr takes effect.  Returns 0 to allow the write, or negative
 * errno to deny a direct modification of security.evm by a non-EVM
 * writer.  EVM's own maintenance (evm_update_after_set) is exempt. */
int evm_setxattr_check(const char *path, const char *name);

/* Return 1 if the named xattr is a security.* xattr (other than
 * security.evm itself) whose change invalidates the stored EVM HMAC
 * and therefore requires an EVM re-calculation after the set. */
int evm_setxattr_must_update(const char *name);

/* Recompute and rewrite the security.evm xattr after a protected xattr
 * changed, keeping the file's EVM HMAC consistent.  Called after a
 * successful setxattr of a protected xattr.  Returns 0 on success,
 * negative on error. */
int evm_update_after_set(const char *path);

#endif /* EVM_H */