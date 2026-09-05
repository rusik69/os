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

#endif /* EVM_H */