#ifndef POSIX_ACL_H
#define POSIX_ACL_H

#include "vfs.h"

/* Serialize ACL to/from system.posix_acl_access xattr */
int posix_acl_set(const char *path, struct posix_acl *acl);
int posix_acl_get(const char *path, struct posix_acl *acl);

/* Check ACL for a given operation */
int posix_acl_permission(const char *path, uint16_t uid, uint16_t gid,
                          uint16_t req_perm);

#endif
