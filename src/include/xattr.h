#ifndef XATTR_H
#define XATTR_H
#include "types.h"

/* Extended attribute limits */
#define XATTR_NAME_MAX 64
#define XATTR_VALUE_MAX 256
#define XATTR_MAX_ENTRIES 8

/* Namespace prefixes for extended attributes */
#define XATTR_SYSTEM_PREFIX    "system."
#define XATTR_SECURITY_PREFIX  "security."
#define XATTR_TRUSTED_PREFIX   "trusted."
#define XATTR_USER_PREFIX      "user."
#define XATTR_POSIX_ACL_ACCESS "system.posix_acl_access"
#define XATTR_POSIX_ACL_DEFAULT "system.posix_acl_default"

/* Validate that an xattr name has a valid namespace prefix.
 * Returns 0 on success, -EINVAL if invalid. */
int xattr_validate_namespace(const char *name);

void xattr_init(void);
int xattr_set(const char *path, const char *name, const void *value, size_t size);
int xattr_get(const char *path, const char *name, void *value, size_t size);
int xattr_list(const char *path, char *buf, size_t size);
int xattr_remove(const char *path, const char *name);

/* VFS-level wrappers */
int vfs_setxattr(const char *path, const char *name, const void *value, int size);
int vfs_getxattr(const char *path, const char *name, void *value, int size);
int vfs_listxattr(const char *path, char *buf, int size);
int vfs_removexattr(const char *path, const char *name);

#endif
