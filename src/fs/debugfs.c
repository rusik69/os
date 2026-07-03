#include "debugfs.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"

static struct debugfs_entry debugfs_entries[DEBUGFS_MAX_ENTRIES];
static int debugfs_mounted = 0;

/* debugfs uses a hierarchical namespace under /sys/kernel/debug/<path> */

/* ── Internal helpers ──────────────────────────────────────────── */

static int alloc_entry(void)
{
	for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
		if (!debugfs_entries[i].in_use) {
			debugfs_entries[i].in_use = 1;
			return i;
		}
	}
	return -ENOMEM;
}

/* Split a '/' separated path into its first component and the remainder.
 * Returns length of first component (could be 0 if no more components).
 * Sets *remaining to the rest of the path (or NULL if none). */
static int split_path(const char *path, const char **remaining)
{
	if (!path || *path == '\0') {
		*remaining = NULL;
		return 0;
	}
	/* Skip leading '/' */
	if (*path == '/')
		path++;
	if (*path == '\0') {
		*remaining = NULL;
		return 0;
	}
	const char *slash = strchr(path, '/');
	if (slash) {
		*remaining = slash + 1;
		return (int)(slash - path);
	}
	*remaining = NULL;
	return (int)strlen(path);
}

/* Find an entry by hierarchical path.
 * Path can be:
 *   - "/sys/kernel/debug/<subdir>/<name>" (full path)
 *   - "<subdir>/<name>" (relative to debugfs root)
 * Returns index on success, negative errno on failure. */
static int find_entry(const char *path)
{
	if (!path || path[0] != '/')
		return -EINVAL;

	/* Strip "/sys/kernel/debug" prefix if present */
	const char *rel = path;
	if (strncmp(path, "/sys/kernel/debug", 17) == 0) {
		rel = path + 17;
		if (*rel == '\0')
			rel = "/";
	}
	/* Also strip just "/sys/kernel" for backward compat in find */
	if (rel == path && strncmp(path, "/sys/kernel", 11) == 0) {
		/* Only strip if path continues with /debug */
		if (path[11] == '/' && strncmp(path + 12, "debug", 5) == 0) {
			rel = path + 17;
			if (*rel == '\0')
				rel = "/";
		}
	}

	/* Root path? */
	if (rel[0] == '/' && rel[1] == '\0')
		return 0; /* root entry is always at index 0 */

	/* Walk the hierarchy component by component */
	int current = 0; /* start at root */
	const char *rem = rel + 1; /* skip leading '/' */
	int comp_len;

	do {
		const char *next_rem = NULL;
		comp_len = split_path(rem, &next_rem);

		if (comp_len == 0) {
			if (next_rem == NULL)
				return current; /* trailing slash — return current dir */
			rem = next_rem;
			continue;
		}

		/* Find child of 'current' with matching name */
		int found = -ENOENT;
		for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
			if (!debugfs_entries[i].in_use)
				continue;
			if (debugfs_entries[i].parent != current)
				continue;
			if (i == current)
				continue;
			if ((int)strlen(debugfs_entries[i].name) == comp_len &&
			    memcmp(debugfs_entries[i].name, rem, (size_t)comp_len) == 0) {
				found = i;
				break;
			}
		}

		if (found < 0)
			return found; /* -ENOENT */

		current = found;
		rem = next_rem;
	} while (rem != NULL);

	return current;
}

/* Create a new entry with given basename, type, and parent.
 * Returns index on success, negative errno on failure. */
static int create_entry(const char *name, uint8_t type, int parent,
			debugfs_read_fn read_fn, debugfs_write_fn write_fn,
			uint32_t *u32_val, void *priv)
{
	if (!name || name[0] == '\0')
		return -EINVAL;

	int idx = alloc_entry();
	if (idx < 0)
		return idx;

	int nlen = (int)strlen(name);
	if (nlen >= DEBUGFS_MAX_NAME)
		nlen = DEBUGFS_MAX_NAME - 1;
	memcpy(debugfs_entries[idx].name, name, (size_t)nlen);
	debugfs_entries[idx].name[nlen] = '\0';
	debugfs_entries[idx].type = type;
	debugfs_entries[idx].parent = parent;
	debugfs_entries[idx].read_fn = read_fn;
	debugfs_entries[idx].write_fn = write_fn;
	debugfs_entries[idx].u32_val = u32_val;
	debugfs_entries[idx].priv = priv;
	debugfs_entries[idx].release_cb = NULL;
	return idx;
}

/* Ensure that all intermediate directories in a path exist.
 * Path should be relative to debugfs root (no leading / or /sys/kernel/debug).
 * Creates directory entries for each component.
 * Returns the index of the final directory (the parent for the file). */
static int ensure_dirs(const char *path)
{
	if (!path || *path == '\0')
		return 0; /* root */

	/* Skip leading / */
	if (*path == '/')
		path++;
	if (*path == '\0')
		return 0;

	/* Count components to pre-validate */
	int components = 1;
	for (const char *p = path; *p; p++) {
		if (*p == '/')
			components++;
	}

	/* Walk components, creating directories as needed */
	int current = 0; /* root */
	const char *rem = path;
	const char *comp_start = path;

	while (*rem) {
		/* Skip to next '/' or end */
		const char *slash = strchr(rem, '/');
		int comp_len;
		if (slash) {
			comp_len = (int)(slash - rem);
		} else {
			comp_len = (int)strlen(rem);
		}

		if (comp_len == 0) {
			rem++;
			continue;
		}

		/* Check if this component already exists as a child of current */
		int found = -ENOENT;
		for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
			if (!debugfs_entries[i].in_use)
				continue;
			if (debugfs_entries[i].parent != current)
				continue;
			if (i == current)
				continue;
			if ((int)strlen(debugfs_entries[i].name) == comp_len &&
			    memcmp(debugfs_entries[i].name, rem, (size_t)comp_len) == 0) {
				found = i;
				break;
			}
		}

		if (found >= 0) {
			current = found;
		} else {
			/* Create the directory */
			char dirname[DEBUGFS_MAX_NAME];
			int copy = comp_len < DEBUGFS_MAX_NAME - 1 ? comp_len : DEBUGFS_MAX_NAME - 1;
			memcpy(dirname, rem, (size_t)copy);
			dirname[copy] = '\0';

			int ret = create_entry(dirname, DEBUGFS_TYPE_DIR, current,
					      NULL, NULL, NULL, NULL);
			if (ret < 0)
				return ret;
			current = ret;
		}

		if (slash) {
			rem = slash + 1;
		} else {
			break;
		}
	}

	return current;
}

/* ── Public API ────────────────────────────────────────────────── */

int debugfs_create_file(const char *name,
			void (*read_fn)(char *buf, int *len))
{
	return debugfs_create_rw_file(name, read_fn, NULL);
}

int debugfs_create_rw_file(const char *name,
			   void (*read_fn)(char *buf, int *len),
			   int (*write_fn)(const char *buf, int len))
{
	if (!name || name[0] == '\0')
		return -EINVAL;

	/* Check if the name contains a path separator — create dirs if so */
	const char *slash = strchr(name, '/');
	int parent = 0; /* root by default */

	if (slash) {
		/* Extract directory path */
		int dirlen = (int)(slash - name);
		if (dirlen > 0) {
			char dirpath[DEBUGFS_MAX_NAME];
			int copy = dirlen < DEBUGFS_MAX_NAME - 1 ? dirlen : DEBUGFS_MAX_NAME - 1;
			memcpy(dirpath, name, (size_t)copy);
			dirpath[copy] = '\0';
			parent = ensure_dirs(dirpath);
			if (parent < 0)
				return -1;
		}
		name = slash + 1;
	}

	if (find_entry(name) >= 0)
		return -1; /* already exists at this level */

	int ret = create_entry(name, DEBUGFS_TYPE_FILE, parent,
			       read_fn, write_fn, NULL, NULL);
	return (ret < 0) ? -1 : 0;
}

int debugfs_create_u32(const char *name, uint32_t *val)
{
	if (!name || name[0] == '\0')
		return -EINVAL;

	/* Handle path separators like other create functions */
	const char *slash = strchr(name, '/');
	int parent = 0; /* root by default */

	if (slash) {
		int dirlen = (int)(slash - name);
		if (dirlen > 0) {
			char dirpath[DEBUGFS_MAX_NAME];
			int copy = dirlen < DEBUGFS_MAX_NAME - 1 ? dirlen : DEBUGFS_MAX_NAME - 1;
			memcpy(dirpath, name, (size_t)copy);
			dirpath[copy] = '\0';
			parent = ensure_dirs(dirpath);
			if (parent < 0)
				return -1;
		}
		name = slash + 1;
	}

	if (find_entry(name) >= 0)
		return -1;

	int ret = create_entry(name, DEBUGFS_TYPE_FILE, parent,
			       NULL, NULL, val, NULL);
	return (ret < 0) ? -1 : 0;
}

int debugfs_create_file_in_dir(const char *name,
			       void (*read_fn)(char *buf, int *len),
			       int (*write_fn)(const char *buf, int len),
			       void *parent)
{
	if (!name || name[0] == '\0')
		return -EINVAL;

	int parent_idx = 0;
	if (parent) {
		/* parent is a pointer to the debugfs_entry — compute its index */
		parent_idx = (int)((struct debugfs_entry *)parent - debugfs_entries);
		if (parent_idx < 0 || parent_idx >= DEBUGFS_MAX_ENTRIES)
			return -EINVAL;
		if (!debugfs_entries[parent_idx].in_use ||
		    debugfs_entries[parent_idx].type != DEBUGFS_TYPE_DIR)
			return -EINVAL;
	}

	/* Check no '/' in name (caller should use path-based API for hierarchies) */
	if (strchr(name, '/'))
		return -EINVAL;

	/* Check uniqueness under parent */
	for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
		if (!debugfs_entries[i].in_use)
			continue;
		if (debugfs_entries[i].parent != parent_idx)
			continue;
		if (strcmp(debugfs_entries[i].name, name) == 0)
			return -1;
	}

	int ret = create_entry(name, DEBUGFS_TYPE_FILE, parent_idx,
			       read_fn, write_fn, NULL, NULL);
	return (ret < 0) ? -1 : 0;
}

void *debugfs_create_dir(const char *name, void *parent)
{
	if (!name || name[0] == '\0')
		return NULL;

	int parent_idx = 0;
	if (parent) {
		parent_idx = (int)((struct debugfs_entry *)parent - debugfs_entries);
		if (parent_idx < 0 || parent_idx >= DEBUGFS_MAX_ENTRIES)
			return NULL;
		if (!debugfs_entries[parent_idx].in_use ||
		    debugfs_entries[parent_idx].type != DEBUGFS_TYPE_DIR)
			return NULL;
	}

	/* Handle path separators — create intermediate directories */
	const char *slash = strchr(name, '/');
	if (slash) {
		/* Create all but the last component as intermediate dirs */
		int dirlen = (int)(slash - name);
		if (dirlen > 0) {
			char dirpath[DEBUGFS_MAX_NAME];
			int copy = dirlen < DEBUGFS_MAX_NAME - 1 ? dirlen : DEBUGFS_MAX_NAME - 1;
			memcpy(dirpath, name, (size_t)copy);
			dirpath[copy] = '\0';
			int dir_idx = ensure_dirs(dirpath);
			if (dir_idx < 0)
				return NULL;
			parent_idx = dir_idx;
		}
		name = slash + 1;
		if (*name == '\0')
			return &debugfs_entries[parent_idx]; /* trailing slash */
	}

	/* Check no '/' in remaining name (should have been handled above) */
	if (strchr(name, '/'))
		return NULL;

	/* Check uniqueness under parent */
	for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
		if (!debugfs_entries[i].in_use)
			continue;
		if (debugfs_entries[i].parent != parent_idx)
			continue;
		if (strcmp(debugfs_entries[i].name, name) == 0)
			return &debugfs_entries[i]; /* already exists */
	}

	int ret = create_entry(name, DEBUGFS_TYPE_DIR, parent_idx,
			       NULL, NULL, NULL, NULL);
	if (ret < 0)
		return NULL;

	return &debugfs_entries[ret];
}

int debugfs_remove(void *entry)
{
	if (!entry)
		return -EINVAL;

	int idx = (int)((struct debugfs_entry *)entry - debugfs_entries);
	if (idx < 0 || idx >= DEBUGFS_MAX_ENTRIES)
		return -EINVAL;
	if (!debugfs_entries[idx].in_use)
		return -ENOENT;
	if (idx == 0)
		return -EINVAL; /* cannot remove root */

	/* If it's a directory, check it's empty */
	if (debugfs_entries[idx].type == DEBUGFS_TYPE_DIR) {
		for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
			if (!debugfs_entries[i].in_use)
				continue;
			if (debugfs_entries[i].parent == idx && i != idx)
				return -ENOTEMPTY;
		}
	}

	/* Invoke release callback */
	if (debugfs_entries[idx].release_cb)
		debugfs_entries[idx].release_cb(debugfs_entries[idx].priv);

	/* Clear the entry */
	memset(&debugfs_entries[idx], 0, sizeof(debugfs_entries[idx]));
	return 0;
}

int debugfs_remove_recursive(void *entry)
{
	if (!entry)
		return -EINVAL;

	int idx = (int)((struct debugfs_entry *)entry - debugfs_entries);
	if (idx < 0 || idx >= DEBUGFS_MAX_ENTRIES)
		return -EINVAL;
	if (!debugfs_entries[idx].in_use)
		return -ENOENT;
	if (idx == 0)
		return -EINVAL; /* cannot remove root */

	/* Recursively remove children first (reverse order to avoid index shift) */
	for (int i = DEBUGFS_MAX_ENTRIES - 1; i >= 0; i--) {
		if (!debugfs_entries[i].in_use)
			continue;
		if (debugfs_entries[i].parent == idx && i != idx)
			debugfs_remove_recursive(&debugfs_entries[i]);
	}

	/* Now remove the entry itself */
	return debugfs_remove(entry);
}

int debugfs_set_release_cb(void *entry, debugfs_release_cb cb)
{
	if (!entry)
		return -EINVAL;

	int idx = (int)((struct debugfs_entry *)entry - debugfs_entries);
	if (idx < 0 || idx >= DEBUGFS_MAX_ENTRIES)
		return -EINVAL;
	if (!debugfs_entries[idx].in_use)
		return -ENOENT;

	debugfs_entries[idx].release_cb = cb;
	return 0;
}

/* ── VFS operations ────────────────────────────────────────────── */

static int debugfs_vfs_read(void *priv, const char *path, void *buf,
			    uint32_t max_size, uint32_t *out_size)
{
	(void)priv;
	int idx = find_entry(path);
	if (idx < 0 || debugfs_entries[idx].type != DEBUGFS_TYPE_FILE)
		return -EINVAL;

	char *cbuf = (char *)buf;

	/* Check for u32-type files (stored in u32_val) */
	if (debugfs_entries[idx].u32_val) {
		uint32_t v = *debugfs_entries[idx].u32_val;
		char tmp[16];
		int pos = 0;
		if (v == 0) {
			tmp[pos++] = '0';
		} else {
			char rev[12]; int ri = 0;
			while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
			while (ri > 0) tmp[pos++] = rev[--ri];
		}
		tmp[pos++] = '\n';
		uint32_t copy = (uint32_t)pos < max_size ? (uint32_t)pos : max_size;
		memcpy(cbuf, tmp, copy);
		*out_size = copy;
		return 0;
	}

	/* Use read callback if available */
	if (debugfs_entries[idx].read_fn) {
		int len = 0;
		debugfs_entries[idx].read_fn(cbuf, &len);
		if ((size_t)len > max_size) len = (int)max_size;
		if (len < 0) len = 0;
		if ((size_t)len < max_size)
			cbuf[len] = '\0';
		*out_size = (uint32_t)len;
		return 0;
	}

	/* No data source */
	*out_size = 0;
	return 0;
}

static int debugfs_vfs_write(void *priv, const char *path, const void *data,
			     uint32_t size)
{
	(void)priv;
	int idx = find_entry(path);
	if (idx < 0 || debugfs_entries[idx].type != DEBUGFS_TYPE_FILE)
		return -EINVAL;

	/* Use write callback if available */
	if (debugfs_entries[idx].write_fn) {
		int ret = debugfs_entries[idx].write_fn((const char *)data, (int)size);
		if (ret < 0) return ret;
		return (int)size;
	}

	/* For u32 files, parse the input */
	if (debugfs_entries[idx].u32_val) {
		const char *s = (const char *)data;
		uint32_t v = 0;
		for (uint32_t i = 0; i < size; i++) {
			if (s[i] >= '0' && s[i] <= '9')
				v = v * 10 + (uint32_t)(s[i] - '0');
			else if (s[i] == '\n' || s[i] == '\0')
				break;
		}
		*debugfs_entries[idx].u32_val = v;
		return (int)size;
	}

	return -EROFS;
}

static int debugfs_vfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
	(void)priv;
	int idx = find_entry(path);
	if (idx < 0)
		return -EINVAL;

	if (debugfs_entries[idx].type == DEBUGFS_TYPE_DIR) {
		st->size = 0;
		st->type = VFS_TYPE_DIR;
		st->mode = 0555;
	} else {
		st->size = 64;
		st->type = VFS_TYPE_FILE;
		st->mode = 0644;
	}
	st->uid = 0;
	st->gid = 0;
	st->nlink = 1;
	st->ino = (uint32_t)(idx + 1);
	st->atime = 0;
	st->mtime = 0;
	st->dev_major = 0;
	st->dev_minor = 0;
	return 0;
}

static int debugfs_vfs_create(void *priv, const char *path, uint8_t type)
{
	(void)priv;
	(void)path;
	(void)type;
	return -EROFS; /* debugfs entries are created via the API, not VFS */
}

static int debugfs_vfs_unlink(void *priv, const char *path)
{
	(void)priv;
	(void)path;
	return -EROFS; /* debugfs entries are removed via the API, not VFS */
}

static int debugfs_vfs_readdir(void *priv, const char *path)
{
	(void)priv;
	int idx = find_entry(path);
	if (idx < 0 || debugfs_entries[idx].type != DEBUGFS_TYPE_DIR)
		return -EINVAL;

	for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
		if (!debugfs_entries[i].in_use)
			continue;
		if (debugfs_entries[i].parent != idx)
			continue;
		if (i == idx)
			continue;
		const char *t = (debugfs_entries[i].type == DEBUGFS_TYPE_DIR) ? "D" : "F";
		if (debugfs_entries[i].u32_val)
			kprintf("  [%s] %s (u32)\n", t, debugfs_entries[i].name);
		else if (debugfs_entries[i].read_fn || debugfs_entries[i].write_fn)
			kprintf("  [%s] %s (callback)\n", t, debugfs_entries[i].name);
		else if (debugfs_entries[i].type == DEBUGFS_TYPE_DIR)
			kprintf("  [%s] %s\n", t, debugfs_entries[i].name);
		else
			kprintf("  [%s] %s\n", t, debugfs_entries[i].name);
	}
	return 0;
}

static int debugfs_vfs_readdir_names(void *priv, const char *path,
				     char names[][64], int max)
{
	(void)priv;
	int idx = find_entry(path);
	if (idx < 0 || debugfs_entries[idx].type != DEBUGFS_TYPE_DIR)
		return 0;

	int count = 0;
	for (int i = 0; i < DEBUGFS_MAX_ENTRIES && count < max; i++) {
		if (!debugfs_entries[i].in_use)
			continue;
		if (debugfs_entries[i].parent != idx)
			continue;
		if (i == idx)
			continue;
		int len = (int)strlen(debugfs_entries[i].name);
		int copylen = len < 63 ? len : 63;
		memcpy(names[count], debugfs_entries[i].name, (size_t)copylen);
		names[count][copylen] = '\0';
		count++;
	}
	return count;
}

struct vfs_ops debugfs_vfs_ops = {
	.read           = debugfs_vfs_read,
	.write          = debugfs_vfs_write,
	.stat           = debugfs_vfs_stat,
	.create         = debugfs_vfs_create,
	.unlink         = debugfs_vfs_unlink,
	.readdir        = debugfs_vfs_readdir,
	.readdir_names  = debugfs_vfs_readdir_names,
};

/* ── Initialisation ────────────────────────────────────────────── */

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void)
{
	if (debugfs_mounted) return 0;
	debugfs_init();
	return 0;
}

void __exit cleanup_module(void)
{
	if (debugfs_mounted) {
		debugfs_mounted = 0;
		for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++) {
			if (debugfs_entries[i].in_use && debugfs_entries[i].release_cb)
				debugfs_entries[i].release_cb(debugfs_entries[i].priv);
			debugfs_entries[i].in_use = 0;
		}
		kprintf("[debugfs] Module unloaded\n");
	}
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Debugfs virtual filesystem — exposes kernel debug data via /sys/kernel/debug");
MODULE_VERSION("1.0");
#else /* !MODULE — built-in, called directly from kernel boot path */

/* Forward declarations */
static void debugfs_create_subsystem_dirs(void);

void __init debugfs_init(void)
{
	if (debugfs_mounted) return;

	/* Clear all entries */
	for (int i = 0; i < DEBUGFS_MAX_ENTRIES; i++)
		debugfs_entries[i].in_use = 0;

	/* Set up root directory at index 0 */
	debugfs_entries[0].in_use = 1;
	debugfs_entries[0].type = DEBUGFS_TYPE_DIR;
	debugfs_entries[0].parent = -1;
	debugfs_entries[0].name[0] = '\0';
	debugfs_entries[0].read_fn = NULL;
	debugfs_entries[0].write_fn = NULL;
	debugfs_entries[0].u32_val = NULL;
	debugfs_entries[0].priv = NULL;
	debugfs_entries[0].release_cb = NULL;

	/* Mount under /sys/kernel/debug */
	if (vfs_mount("/sys/kernel/debug", &debugfs_vfs_ops, NULL) == 0) {
		kprintf("[OK] debugfs mounted on /sys/kernel/debug\n");
	} else {
		kprintf("[!!] debugfs mount failed\n");
		return;
	}

	debugfs_mounted = 1;

	/* Create standard per-subsystem directories */
	debugfs_create_subsystem_dirs();
}

/* ── Per-subsystem standard directories ──────────────────────────── */

static void debugfs_create_subsystem_dirs(void)
{
	/* Each of these creates a top-level directory under /sys/kernel/debug/.
	 * Subsystem owners create their files under these directories. */
	debugfs_create_dir("acpi", NULL);
	debugfs_create_dir("pci", NULL);
	debugfs_create_dir("memory", NULL);
	debugfs_create_dir("gpio", NULL);
	debugfs_create_dir("i2c", NULL);
	debugfs_create_dir("spi", NULL);
	debugfs_create_dir("usb", NULL);
	debugfs_create_dir("block", NULL);
	debugfs_create_dir("net", NULL);
	debugfs_create_dir("scsi", NULL);
	debugfs_create_dir("thermal", NULL);
	debugfs_create_dir("power", NULL);
	debugfs_create_dir("crypto", NULL);
	debugfs_create_dir("irq", NULL);
	debugfs_create_dir("dma", NULL);
	debugfs_create_dir("kunit", NULL);
	debugfs_create_dir("uprobes", NULL);
	debugfs_create_dir("ras", NULL);
}
#endif /* MODULE */
