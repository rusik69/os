#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "firmware.h"
#include "vfs.h"
#include "heap.h"
#include "spinlock.h"

/* Built-in firmware table */
#define BUILTIN_FW_MAX 16
static struct builtin_fw builtin_fw_table[BUILTIN_FW_MAX];
static int num_builtin_fw = 0;
static spinlock_t fw_lock = SPINLOCK_INIT;

/* Forward declaration of built-in firmware blobs (if any) */
extern const uint8_t _binary_firmware_start[];
extern const uint8_t _binary_firmware_end[];

void firmware_init(void)
{
    memset(builtin_fw_table, 0, sizeof(builtin_fw_table));
    num_builtin_fw = 0;
    kprintf("[OK] firmware: firmware loading API initialized\n");
}

int firmware_register_builtin(const char *name, const uint8_t *data, size_t size)
{
    if (!name || !data || size == 0)
        return -EINVAL;

    spinlock_acquire(&fw_lock);

    if (num_builtin_fw >= BUILTIN_FW_MAX) {
        spinlock_release(&fw_lock);
        return -ENOMEM;
    }

    struct builtin_fw *entry = &builtin_fw_table[num_builtin_fw];
    entry->name = name;
    entry->data = data;
    entry->size = size;
    num_builtin_fw++;

    spinlock_release(&fw_lock);
    return 0;
}

int firmware_load(const char *name, struct firmware *fw)
{
    if (!name || !fw)
        return -EINVAL;

    /* Initialize output */
    fw->data = NULL;
    fw->size = 0;

    /* 1. Check built-in table first */
    spinlock_acquire(&fw_lock);
    for (int i = 0; i < num_builtin_fw; i++) {
        if (strcmp(builtin_fw_table[i].name, name) == 0) {
            fw->data = builtin_fw_table[i].data;
            fw->size = builtin_fw_table[i].size;
            spinlock_release(&fw_lock);
            return 0;
        }
    }
    spinlock_release(&fw_lock);

    /* 2. Try loading from /lib/firmware/<name> via VFS */
    char path[128];
    int ret = snprintf(path, sizeof(path), "/lib/firmware/%s", name);
    if (ret < 0 || ret >= (int)sizeof(path))
        return -ENAMETOOLONG;

    /* Stat the file first to get size */
    struct vfs_stat st;
    ret = vfs_stat(path, &st);
    if (ret != 0)
        return -ENOENT;

    size_t fw_size = st.size;
    if (fw_size == 0)
        return -ENODATA;

    /* Allocate memory for firmware data */
    uint8_t *fw_data = (uint8_t *)kmalloc(fw_size);
    if (!fw_data)
        return -ENOMEM;

    /* Read the file */
    uint32_t bytes_read = 0;
    ret = vfs_read(path, fw_data, (uint32_t)fw_size, &bytes_read);
    if (ret != 0 || bytes_read != fw_size) {
        kfree(fw_data);
        return -EIO;
    }

    fw->data = fw_data;
    fw->size = fw_size;
    return 0;
}

void firmware_release(struct firmware *fw)
{
    if (!fw)
        return;

    /* Only free if dynamically allocated (not in built-in table) */
    spinlock_acquire(&fw_lock);
    int is_builtin = 0;
    for (int i = 0; i < num_builtin_fw; i++) {
        if (builtin_fw_table[i].data == fw->data) {
            is_builtin = 1;
            break;
        }
    }
    spinlock_release(&fw_lock);

    if (!is_builtin && fw->data) {
        kfree((void *)fw->data);
    }

    fw->data = NULL;
    fw->size = 0;
}
