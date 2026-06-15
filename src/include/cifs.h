#ifndef CIFS_H
#define CIFS_H

#include "types.h"
#include "vfs.h"
#include "errno.h"

/* CIFS/SMB client constants */
#define CIFS_MAX_PATH     1024
#define CIFS_MAX_DATA     8192
#define CIFS_MAX_MOUNTS   8
#define CIFS_SERVER_NAME  64
#define CIFS_MAX_SHARE    128
#define CIFS_MAX_USER     64
#define CIFS_MAX_PASS     64

/* SMB 2.0.2 dialect revision */
#define SMB2_DIALECT_REVISION_202 0x0202

/* SMB2 command codes */
#define SMB2_NEGOTIATE    0x0000
#define SMB2_SESSION_SETUP 0x0001
#define SMB2_LOGOFF       0x0002
#define SMB2_TREE_CONNECT 0x0003
#define SMB2_TREE_DISCONNECT 0x0004
#define SMB2_CREATE       0x0005
#define SMB2_CLOSE        0x0006
#define SMB2_FLUSH        0x0007
#define SMB2_READ         0x0008
#define SMB2_WRITE        0x0009
#define SMB2_LOCK         0x000A
#define SMB2_IOCTL        0x000B
#define SMB2_CANCEL       0x000C
#define SMB2_QUERY_INFO   0x0010
#define SMB2_QUERY_DIRECTORY 0x0011

/* SMB2 tree connect flags */
#define SMB2_TREE_FLAG_ENCRYPT_DATA 0x01

/* SMB2 create file access masks */
#define SMB2_FILE_READ_DATA        0x00000001
#define SMB2_FILE_WRITE_DATA       0x00000002
#define SMB2_FILE_APPEND_DATA      0x00000004
#define SMB2_FILE_READ_EA          0x00000008
#define SMB2_FILE_WRITE_EA         0x00000010
#define SMB2_FILE_EXECUTE          0x00000020
#define SMB2_FILE_DELETE_CHILD     0x00000040
#define SMB2_FILE_READ_ATTRIBUTES  0x00000080
#define SMB2_FILE_WRITE_ATTRIBUTES 0x00000100
#define SMB2_DELETE                0x00010000
#define SMB2_READ_CONTROL          0x00020000
#define SMB2_WRITE_DAC             0x00040000
#define SMB2_WRITE_OWNER           0x00080000
#define SMB2_SYNCHRONIZE           0x00100000
#define SMB2_ACCESS_SYSTEM_SECURITY 0x01000000
#define SMB2_MAXIMUM_ALLOWED       0x02000000
#define SMB2_GENERIC_ALL           0x10000000
#define SMB2_GENERIC_EXECUTE       0x20000000
#define SMB2_GENERIC_WRITE         0x40000000
#define SMB2_GENERIC_READ          0x80000000

/* SMB2 create options */
#define SMB2_FILE_DIRECTORY_FILE   0x00000001
#define SMB2_FILE_NON_DIRECTORY_FILE 0x00000040

/* SMB2 create disposition */
#define SMB2_FILE_OPEN             0x00000001
#define SMB2_FILE_OPEN_IF          0x00000003

/* SMB2 info types */
#define SMB2_QUERY_INFO_FILE       0x01
#define SMB2_QUERY_INFO_FILESYSTEM 0x02
#define SMB2_QUERY_INFO_SECURITY   0x03

/* SMB2 file info classes */
#define SMB2_FILE_BASIC_INFO       0x04
#define SMB2_FILE_STANDARD_INFO    0x05
#define SMB2_FILE_NAMES_INFO       0x0C
#define SMB2_FILE_ALL_INFO         0x12
#define SMB2_FILE_ID_INFO          0x3C

/* SMB2 share types */
#define SMB2_SHARE_TYPE_DISK       0x01

/* SMB2 error codes */
#define SMB2_SUCCESS               0x00000000
#define SMB2_STATUS_NO_MORE_FILES  0x80000006
#define SMB2_STATUS_NO_SUCH_FILE   0xC000000F
#define SMB2_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034
#define SMB2_STATUS_END_OF_FILE    0xC0000011

/* SMB2 header size (64 bytes) */
#define SMB2_HEADER_SIZE 64

/* SMB2 negotiate context types */
#define SMB2_PREAUTH_INTEGRITY_CAPABILITIES 0x0001
#define SMB2_ENCRYPTION_CAPABILITIES        0x0002

/* CIFS mount info */
struct cifs_mount_info {
    int mounted;
    char server[CIFS_SERVER_NAME];
    uint32_t server_ip;
    uint16_t server_port;         /* 445 */
    char share[CIFS_MAX_SHARE];
    char username[CIFS_MAX_USER];
    char password[CIFS_MAX_PASS];

    /* TCP connection */
    int conn_id;

    /* SMB2 session */
    uint64_t session_id;
    uint64_t tree_id;
    uint32_t max_transact_size;
    uint32_t max_read_size;
    uint16_t dialect_revision;

    /* Authentication */
    uint8_t session_key[16];

    /* Mount ID for VFS dispatch */
    int mount_id;
};

/* SMB2 file handle */
struct cifs_fh {
    uint64_t volatile_file_id;
    uint64_t persistent_file_id;
};

/* Public API */
int cifs_mount(const char *server, const char *share,
               const char *user, const char *pass);
int cifs_open(int mount_id, const char *path, struct cifs_fh *fh);
int cifs_close(int mount_id, struct cifs_fh *fh);
int cifs_read(int mount_id, struct cifs_fh *fh,
              uint64_t offset, uint32_t count, uint8_t *buf);
int cifs_readdir(int mount_id, const char *dir_path);
int cifs_stat(int mount_id, const char *path, struct vfs_stat *st);
int cifs_umount(int mount_id);
int cifs_init(void);

#endif /* CIFS_H */
