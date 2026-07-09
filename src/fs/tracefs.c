/*
 * src/fs/tracefs.c — Trace filesystem (tracefs)
 *
 * Provides per-CPU ring buffers exposed through /sys/kernel/trace.
 * Writers (trace points, ftrace) append timestamped entries to the
 * current CPU's buffer.  Readers consume entries through the VFS layer.
 *
 * Mounted at /sys/kernel/trace.
 */

#define KERNEL_INTERNAL
#include "tracefs.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"
#include "ftrace.h"
#include "smp.h"
#include "spinlock.h"
#include "timer.h"

/* ── Global state ──────────────────────────────────────────────────── */

/* Per-CPU ring buffers */
static struct tracefs_percpu_buf percpu_bufs[TRACEFS_MAX_CPUS];

/* Global tracing enable flag */
static int tracing_global_enabled;
static int tracefs_mounted;

/* Entry table for VFS operations */
static struct tracefs_entry entries[TRACEFS_MAX_ENTRIES];

/* Number of online CPUs (cached at init) */
static int tracefs_nr_cpus;

/* ── Entry table helpers ───────────────────────────────────────────── */

static int tracefs_alloc_entry(void)
{
	for (int i = 0; i < TRACEFS_MAX_ENTRIES; i++) {
		if (!entries[i].in_use) {
			entries[i].in_use = 1;
			return i;
		}
	}
	return -ENOMEM;
}

static int tracefs_find_entry(const char *path)
{
	if (!path || path[0] != '/')
		return -EINVAL;

	/* Strip /sys/kernel/trace prefix if present */
	const char *rel = path;
	if (strncmp(path, "/sys/kernel/trace", 17) == 0) {
		rel = path + 17;
		if (*rel == '\0')
			rel = "/";
	}

	/* Root path */
	if (rel[0] == '/' && rel[1] == '\0')
		return 0;

	/* Walk hierarchy — currently only one level supported */
	int current = 0;
	const char *rem = rel + 1; /* skip leading '/' */

	while (*rem) {
		/* Find '/' or end */
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

		/* Look for child with matching name */
		int found = -ENOENT;
		for (int i = 0; i < TRACEFS_MAX_ENTRIES; i++) {
			if (!entries[i].in_use)
				continue;
			if (entries[i].parent != current)
				continue;
			if (i == current)
				continue;
			if ((int)strlen(entries[i].name) == comp_len &&
			    memcmp(entries[i].name, rem, (size_t)comp_len) == 0) {
				found = i;
				break;
			}
		}
		if (found < 0)
			return found;
		current = found;

		if (slash) {
			rem = slash + 1;
		} else {
			break;
		}
	}
	return current;
}

static int tracefs_create_entry(const char *name, uint8_t type, int parent,
				void (*read_fn)(char *, int *, void *),
				int (*write_fn)(const char *, int, void *),
				void *priv)
{
	if (!name || name[0] == '\0')
		return -EINVAL;

	int idx = tracefs_alloc_entry();
	if (idx < 0)
		return idx;

	int nlen = (int)strlen(name);
	if (nlen >= 48)
		nlen = 47;
	memcpy(entries[idx].name, name, (size_t)nlen);
	entries[idx].name[nlen] = '\0';
	entries[idx].type = type;
	entries[idx].parent = parent;
	entries[idx].read_fn = read_fn;
	entries[idx].write_fn = write_fn;
	entries[idx].priv = priv;
	return idx;
}

/* ── Per-CPU buffer management ─────────────────────────────────────── */

static int tracefs_init_percpu_bufs(void)
{
	int nr = smp_get_cpu_count();
	if (nr <= 0)
		nr = 1;
	if (nr > TRACEFS_MAX_CPUS)
		nr = TRACEFS_MAX_CPUS;
	tracefs_nr_cpus = nr;

	for (int i = 0; i < nr; i++) {
		percpu_bufs[i].data = (char *)kmalloc(TRACEFS_BUF_SIZE);
		if (!percpu_bufs[i].data) {
			/* Free already-allocated buffers */
			for (int j = 0; j < i; j++) {
				if (percpu_bufs[j].data)
					kfree(percpu_bufs[j].data);
				percpu_bufs[j].data = NULL;
			}
			return -ENOMEM;
		}
		memset(percpu_bufs[i].data, 0, TRACEFS_BUF_SIZE);
		percpu_bufs[i].size = TRACEFS_BUF_SIZE;
		percpu_bufs[i].write_pos = 0;
		percpu_bufs[i].read_pos = 0;
		spinlock_init(&percpu_bufs[i].lock);
	}
	return 0;
}

static void tracefs_free_percpu_bufs(void)
{
	for (int i = 0; i < TRACEFS_MAX_CPUS; i++) {
		if (percpu_bufs[i].data) {
			kfree(percpu_bufs[i].data);
			percpu_bufs[i].data = NULL;
		}
		percpu_bufs[i].size = 0;
		percpu_bufs[i].write_pos = 0;
		percpu_bufs[i].read_pos = 0;
	}
}

/* Write a trace entry to the current CPU's ring buffer.
 * The entry is formatted as: [timestamp] data\n */
int tracefs_write_entry(const char *data, uint32_t len)
{
	if (!tracing_global_enabled)
		return 0;
	if (!data || len == 0)
		return -EINVAL;

	int cpu_id = smp_get_cpu_id();
	if (cpu_id < 0 || cpu_id >= tracefs_nr_cpus)
		return -EINVAL;

	struct tracefs_percpu_buf *buf = &percpu_bufs[cpu_id];
	if (!buf->data)
		return -ENOMEM;

	/* Format: [timestamp] data\n
	 * We need a small stack buffer for the prefix. */
	char prefix[32];
	uint64_t now = timer_get_ticks();
	int plen = snprintf(prefix, sizeof(prefix), "[%llu] ", (unsigned long long)now);
	if (plen < 0)
		plen = 0;
	if ((uint32_t)plen >= sizeof(prefix))
		plen = (int)sizeof(prefix) - 1;

	uint32_t total = (uint32_t)plen + len + 1; /* +1 for newline */
	if (total > buf->size) {
		/* Entry too large — drop */
		return -ENOSPC;
	}

	/* Acquire per-CPU write lock */
	spinlock_acquire(&buf->lock);

	uint32_t wp = buf->write_pos;

	/* Write prefix */
	for (int i = 0; i < plen; i++) {
		buf->data[wp] = prefix[i];
		wp = (wp + 1) % buf->size;
	}

	/* Write data */
	for (uint32_t i = 0; i < len; i++) {
		buf->data[wp] = data[i];
		wp = (wp + 1) % buf->size;
	}

	/* Write trailing newline */
	buf->data[wp] = '\n';
	wp = (wp + 1) % buf->size;

	buf->write_pos = wp;

	spinlock_release(&buf->lock);
	return 0;
}

/* Enable/disable tracing globally */
void tracefs_enable(void)
{
	tracing_global_enabled = 1;
}

void tracefs_disable(void)
{
	tracing_global_enabled = 0;
}

int tracefs_is_enabled(void)
{
	return tracing_global_enabled;
}

/* Get bytes available to read from a per-CPU buffer */
uint32_t tracefs_percpu_avail(int cpu_id)
{
	if (cpu_id < 0 || cpu_id >= tracefs_nr_cpus)
		return 0;
	struct tracefs_percpu_buf *buf = &percpu_bufs[cpu_id];
	if (!buf->data || buf->size == 0)
		return 0;

	spinlock_acquire(&buf->lock);
	uint32_t avail;
	if (buf->write_pos >= buf->read_pos)
		avail = buf->write_pos - buf->read_pos;
	else
		avail = (buf->size - buf->read_pos) + buf->write_pos;
	spinlock_release(&buf->lock);
	return avail;
}

/* ── Read callbacks for files ──────────────────────────────────────── */

/* tracing_on: show 1 or 0 */
static void tracefs_read_tracing_on(char *buf, int *len, void *priv)
{
	(void)priv;
	buf[0] = tracing_global_enabled ? '1' : '0';
	buf[1] = '\n';
	buf[2] = '\0';
	*len = 2;
}

/* tracing_on write: parse 1/0 and propagate to ftrace / trace events */
static int tracefs_write_tracing_on(const char *buf, int len, void *priv)
{
	(void)priv;
	for (int i = 0; i < len; i++) {
		if (buf[i] == '1') {
			tracing_global_enabled = 1;
			ftrace_enable();
			trace_events_v2_enable();
			return len;
		} else if (buf[i] == '0') {
			tracing_global_enabled = 0;
			ftrace_disable();
			trace_events_v2_disable();
			return len;
		}
	}
	return -EINVAL;
}

/* buffer_size_kb read: show current size per CPU */
static void tracefs_read_buffer_size(char *buf, int *len, void *priv)
{
	int cpu_id = (int)(uintptr_t)priv;
	if (cpu_id < 0 || cpu_id >= tracefs_nr_cpus) {
		*len = 0;
		buf[0] = '\0';
		return;
	}
	uint32_t size_kb = percpu_bufs[cpu_id].size / 1024;
	*len = snprintf(buf, 32, "%u\n", size_kb);
}

/* buffer_size_kb write: resize buffer (simple kfree + kmalloc) */
static int tracefs_write_buffer_size(const char *buf, int len, void *priv)
{
	int cpu_id = (int)(uintptr_t)priv;
	if (cpu_id < 0 || cpu_id >= tracefs_nr_cpus)
		return -EINVAL;

	/* Parse number */
	uint32_t val = 0;
	for (int i = 0; i < len; i++) {
		if (buf[i] >= '0' && buf[i] <= '9')
			val = val * 10 + (uint32_t)(buf[i] - '0');
		else if (buf[i] == '\n' || buf[i] == '\0')
			break;
	}

	if (val < (TRACEFS_BUF_MIN / 1024))
		val = TRACEFS_BUF_MIN / 1024;
	if (val > (TRACEFS_BUF_MAX / 1024))
		val = TRACEFS_BUF_MAX / 1024;

	uint32_t new_size = val * 1024;

	struct tracefs_percpu_buf *pb = &percpu_bufs[cpu_id];
	spinlock_acquire(&pb->lock);

	char *new_buf = (char *)kmalloc(new_size);
	if (!new_buf) {
		spinlock_release(&pb->lock);
		return -ENOMEM;
	}
	memset(new_buf, 0, new_size);

	/* Free old buffer */
	if (pb->data)
		kfree(pb->data);
	pb->data = new_buf;
	pb->size = new_size;
	pb->write_pos = 0;
	pb->read_pos = 0;

	spinlock_release(&pb->lock);
	return len;
}

/* Read per-CPU trace buffer content (snapshot of current contents) */
static void tracefs_read_percpu_trace(char *buf, int *len, void *priv)
{
	int cpu_id = (int)(uintptr_t)priv;
	if (cpu_id < 0 || cpu_id >= tracefs_nr_cpus) {
		*len = 0;
		buf[0] = '\0';
		return;
	}

	struct tracefs_percpu_buf *pb = &percpu_bufs[cpu_id];
	if (!pb->data || pb->size == 0) {
		*len = 0;
		buf[0] = '\0';
		return;
	}

	spinlock_acquire(&pb->lock);

	uint32_t rp = pb->read_pos;
	uint32_t wp = pb->write_pos;
	int pos = 0;

	/* Copy data from read_pos to write_pos */
	if (wp >= rp) {
		uint32_t count = wp - rp;
		if (count > 4095)
			count = 4095;
		memcpy(buf, pb->data + rp, count);
		pos = (int)count;
	} else {
		/* Wrapped */
		uint32_t first = pb->size - rp;
		if (first > 4095)
			first = 4095;
		memcpy(buf, pb->data + rp, first);
		pos = (int)first;

		if (pos < 4095) {
			uint32_t second = wp;
			if (second > (uint32_t)(4095 - pos))
				second = (uint32_t)(4095 - pos);
			memcpy(buf + pos, pb->data, second);
			pos += (int)second;
		}
	}

	buf[pos] = '\0';
	*len = pos;

	/* Advance read position to write position (consumes all data) */
	pb->read_pos = pb->write_pos;

	spinlock_release(&pb->lock);
}

/* trace_marker write: inject a trace entry */
static int tracefs_write_marker(const char *buf, int len, void *priv)
{
	(void)priv;
	return tracefs_write_entry(buf, (uint32_t)len);
}

/* trace_marker read: show count of entries (stub) */
static void tracefs_read_marker(char *buf, int *len, void *priv)
{
	(void)priv;
	/* Just show that the write interface is active */
	int n = snprintf(buf, 64, "trace_marker: write interface active\n");
	if (n < 0)
		n = 0;
	if (n > 63)
		n = 63;
	*len = n;
}

/* current_tracer read: show the current tracer name */
static void tracefs_read_current_tracer(char *buf, int *len, void *priv)
{
	(void)priv;
	int mode = ftrace_get_tracer();
	const char *name;
	switch (mode) {
	case FTRACE_TRACER_FUNCTION:
		name = "function";
		break;
	case FTRACE_TRACER_FUNCTION_GRAPH:
		name = "function_graph";
		break;
	default:
		name = "nop";
		break;
	}
	int n = snprintf(buf, 32, "%s\n", name);
	if (n < 0)
		n = 0;
	if (n > 31)
		n = 31;
	*len = n;
}

/* current_tracer write: parse and switch tracer */
static int tracefs_write_current_tracer(const char *buf, int len, void *priv)
{
	(void)priv;
	/* Trim leading whitespace/newlines */
	int start = 0;
	while (start < len && (buf[start] == ' ' || buf[start] == '	' ||
			       buf[start] == '\n' || buf[start] == '\r'))
		start++;
	/* Trim trailing whitespace/newlines */
	int end = len - 1;
	while (end >= start && (buf[end] == ' ' || buf[end] == '	' ||
				buf[end] == '\n' || buf[end] == '\r'))
		end--;
	int name_len = end - start + 1;
	if (name_len <= 0)
		return -EINVAL;

	if (name_len == 3 && memcmp(buf + start, "nop", 3) == 0)
		return ftrace_set_tracer(FTRACE_TRACER_NOP);
	if (name_len == 8 && memcmp(buf + start, "function", 8) == 0)
		return ftrace_set_tracer(FTRACE_TRACER_FUNCTION);
	if (name_len == 14 && memcmp(buf + start, "function_graph", 14) == 0)
		return ftrace_set_tracer(FTRACE_TRACER_FUNCTION_GRAPH);

	return -EINVAL;
}

/* trace read: show aggregate per-CPU summary with actual data */
static void tracefs_read_trace(char *buf, int *len, void *priv)
{
	(void)priv;
	int pos = 0;
	int mode = ftrace_get_tracer();
	const char *tname = "nop";
	if (mode == FTRACE_TRACER_FUNCTION)
		tname = "function";
	else if (mode == FTRACE_TRACER_FUNCTION_GRAPH)
		tname = "function_graph";

	pos += snprintf(buf + pos, 64, "# tracer: %s\n", tname);
	pos += snprintf(buf + pos, 64, "#\n");

	/* If function_graph tracer is active, read graph trace data */
	if (mode == FTRACE_TRACER_FUNCTION_GRAPH) {
		int ret = ftrace_graph_read_trace(buf + pos, 4096 - pos);
		if (ret > 0) {
			pos += ret;
			if (pos > 4095)
				pos = 4095;
			*len = pos;
			return;
		}
	}

	/* Show per-CPU entry counts */
	uint32_t total_entries = 0;
	for (int i = 0; i < tracefs_nr_cpus; i++)
		total_entries += tracefs_percpu_avail(i);

	pos += snprintf(buf + pos, 128,
			"# entries-in-buffer/entries-written: %u/??? (%d CPUs)\n",
			total_entries, tracefs_nr_cpus);
	pos += snprintf(buf + pos, 64, "#\n");

	/* Read a snapshot from each per-CPU buffer */
	for (int i = 0; i < tracefs_nr_cpus && pos < 4000; i++) {
		uint32_t avail = tracefs_percpu_avail(i);
		if (avail == 0)
			continue;

		pos += snprintf(buf + pos, 128, "# CPU:%d entries:\n", i);

		/* Read directly from per-CPU buffer without consuming */
		struct tracefs_percpu_buf *pb = &percpu_bufs[i];
		spinlock_acquire(&pb->lock);

		uint32_t rp = pb->read_pos;
		uint32_t wp = pb->write_pos;
		uint32_t max_copy = 256; /* Limit per-CPU to avoid overflow */

		/* Copy data from read_pos to write_pos (snapshot, don't consume) */
		if (wp > rp) {
			uint32_t count = wp - rp;
			if (count > max_copy)
				count = max_copy;
			int cpos = 0;
			for (uint32_t j = 0; j < count && pos + cpos < 4095; j++) {
				buf[pos + cpos] = pb->data[(rp + j) % pb->size];
				cpos++;
			}
			pos += cpos;
		} else if (wp < rp) {
			/* Wrapped */
			uint32_t first = pb->size - rp;
			if (first > max_copy)
				first = max_copy;
			int cpos = 0;
			for (uint32_t j = 0; j < first && pos + cpos < 4095; j++) {
				buf[pos + cpos] = pb->data[rp + j];
				cpos++;
			}
			pos += cpos;
			if ((uint32_t)cpos >= max_copy) {
				spinlock_release(&pb->lock);
				continue;
			}
			uint32_t second = wp;
			uint32_t remain = max_copy - (uint32_t)cpos;
			if (second > remain)
				second = remain;
			for (uint32_t j = 0; j < second && pos < 4095; j++) {
				buf[pos] = pb->data[j];
				pos++;
			}
		}

		spinlock_release(&pb->lock);

		if (pos < 4095) {
			pos += snprintf(buf + pos, 32, "\n");
		}
	}

	if (pos > 4095)
		pos = 4095;
	buf[pos] = '\0';
	*len = pos;
}

/* trace write: clear all trace buffers */
static int tracefs_write_trace(const char *buf, int len, void *priv)
{
	(void)priv;
	(void)buf;
	(void)len;

	/* Clear per-CPU buffers */
	for (int i = 0; i < tracefs_nr_cpus; i++) {
		struct tracefs_percpu_buf *pb = &percpu_bufs[i];
		if (!pb->data)
			continue;
		spinlock_acquire(&pb->lock);
		pb->read_pos = pb->write_pos;
		spinlock_release(&pb->lock);
	}

	/* Clear function graph trace */
	ftrace_graph_clear();

	return len ? len : 1; /* echo "" > trace succeeds */
}

/* trace stats: CPU-by-CPU buffer fill levels */
static void tracefs_read_trace_stats(char *buf, int *len, void *priv)
{
	(void)priv;
	int pos = 0;
	pos += snprintf(buf + pos, 64, "tracing_on: %d\n", tracing_global_enabled ? 1 : 0);
	pos += snprintf(buf + pos, 64, "nr_cpus: %d\n", tracefs_nr_cpus);
	for (int i = 0; i < tracefs_nr_cpus && pos < 4000; i++) {
		uint32_t avail = tracefs_percpu_avail(i);
		uint32_t size_kb = percpu_bufs[i].size / 1024;
		pos += snprintf(buf + pos, 128, "cpu%d: %u bytes available / %u KB buffer\n",
			       i, avail, size_kb);
	}
	if (pos > 4095)
		pos = 4095;
	*len = pos;
}

/* ── VFS operations ────────────────────────────────────────────────── */

static int tracefs_vfs_read(void *priv, const char *path, void *buf,
			    uint32_t max_size, uint32_t *out_size)
{
	(void)priv;
	int idx = tracefs_find_entry(path);
	if (idx < 0)
		return -ENOENT;
	if (entries[idx].type != TRACEFS_TYPE_FILE)
		return -EISDIR;

	char *cbuf = (char *)buf;

	if (entries[idx].read_fn) {
		int len = 0;
		entries[idx].read_fn(cbuf, &len, entries[idx].priv);
		if (len < 0) len = 0;
		if ((uint32_t)len > max_size)
			len = (int)max_size;
		if ((uint32_t)len < max_size)
			cbuf[len] = '\0';
		*out_size = (uint32_t)len;
		return 0;
	}

	*out_size = 0;
	return 0;
}

static int tracefs_vfs_write(void *priv, const char *path, const void *data,
			     uint32_t size)
{
	(void)priv;
	int idx = tracefs_find_entry(path);
	if (idx < 0)
		return idx;
	if (entries[idx].type != TRACEFS_TYPE_FILE)
		return -EISDIR;

	if (entries[idx].write_fn) {
		int ret = entries[idx].write_fn((const char *)data, (int)size,
						entries[idx].priv);
		if (ret < 0)
			return ret;
		return (int)size;
	}

	return -EROFS;
}

static int tracefs_vfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
	(void)priv;
	int idx = tracefs_find_entry(path);
	if (idx < 0)
		return -ENOENT;

	memset(st, 0, sizeof(*st));
	if (entries[idx].type == TRACEFS_TYPE_DIR) {
		st->size = 0;
		st->type = VFS_TYPE_DIR;
		st->mode = 0555;
	} else {
		st->size = 4096;
		st->type = VFS_TYPE_FILE;
		st->mode = 0644;
	}
	st->uid = 0;
	st->gid = 0;
	st->nlink = 1;
	st->ino = (uint32_t)(idx + 1);
	return 0;
}

static int tracefs_vfs_create(void *priv, const char *path, uint8_t type)
{
	(void)priv;
	(void)path;
	(void)type;
	return -EROFS;
}

static int tracefs_vfs_unlink(void *priv, const char *path)
{
	(void)priv;
	(void)path;
	return -EROFS;
}

static int tracefs_vfs_readdir(void *priv, const char *path)
{
	(void)priv;
	int idx = tracefs_find_entry(path);
	if (idx < 0 || entries[idx].type != TRACEFS_TYPE_DIR)
		return -EINVAL;

	for (int i = 0; i < TRACEFS_MAX_ENTRIES; i++) {
		if (!entries[i].in_use)
			continue;
		if (entries[i].parent != idx)
			continue;
		if (i == idx)
			continue;
		const char *t = (entries[i].type == TRACEFS_TYPE_DIR) ? "D" : "F";
		kprintf("  [%s] %s\n", t, entries[i].name);
	}
	return 0;
}

static int tracefs_vfs_readdir_names(void *priv, const char *path,
				     char names[][64], int max)
{
	(void)priv;
	int idx = tracefs_find_entry(path);
	if (idx < 0 || entries[idx].type != TRACEFS_TYPE_DIR)
		return 0;

	int count = 0;
	for (int i = 0; i < TRACEFS_MAX_ENTRIES && count < max; i++) {
		if (!entries[i].in_use)
			continue;
		if (entries[i].parent != idx)
			continue;
		if (i == idx)
			continue;
		int len = (int)strlen(entries[i].name);
		int copylen = len < 63 ? len : 63;
		memcpy(names[count], entries[i].name, (size_t)copylen);
		names[count][copylen] = '\0';
		count++;
	}
	return count;
}

struct vfs_ops tracefs_vfs_ops = {
	.read           = tracefs_vfs_read,
	.write          = tracefs_vfs_write,
	.stat           = tracefs_vfs_stat,
	.create         = tracefs_vfs_create,
	.unlink         = tracefs_vfs_unlink,
	.readdir        = tracefs_vfs_readdir,
	.readdir_names  = tracefs_vfs_readdir_names,
};

/* ── Initialisation ────────────────────────────────────────────────── */

static int tracefs_create_percpu_files(void)
{
	/* Create per_cpu/ directory */
	struct tracefs_entry root;
	root.in_use = 0;

	(void)root;

	int per_cpu_dir = tracefs_create_entry("per_cpu", TRACEFS_TYPE_DIR, 0,
					       NULL, NULL, NULL);
	if (per_cpu_dir < 0)
		return per_cpu_dir;

	/* Create cpu<N> subdirectories and trace files */
	for (int i = 0; i < tracefs_nr_cpus; i++) {
		char dirname[16];
		snprintf(dirname, sizeof(dirname), "cpu%d", i);

		int cpu_dir = tracefs_create_entry(dirname, TRACEFS_TYPE_DIR,
						   per_cpu_dir, NULL, NULL, NULL);
		if (cpu_dir < 0)
			continue;

		/* trace file in each cpu dir — reads the per-CPU buffer */
		int ret = tracefs_create_entry("trace", TRACEFS_TYPE_FILE,
					       cpu_dir,
					       tracefs_read_percpu_trace,
					       NULL,
					       (void *)(uintptr_t)i);
		if (ret < 0)
			continue;

		/* buffer_size_kb file in each cpu dir */
		ret = tracefs_create_entry("buffer_size_kb", TRACEFS_TYPE_FILE,
					   cpu_dir,
					   tracefs_read_buffer_size,
					   tracefs_write_buffer_size,
					   (void *)(uintptr_t)i);
		if (ret < 0)
			continue;
	}

	return 0;
}

void __init tracefs_init(void)
{
	if (tracefs_mounted)
		return;

	/* Clear entry table */
	for (int i = 0; i < TRACEFS_MAX_ENTRIES; i++)
		entries[i].in_use = 0;

	/* Set up root directory at index 0 */
	entries[0].in_use = 1;
	entries[0].type = TRACEFS_TYPE_DIR;
	entries[0].parent = -1;
	entries[0].name[0] = '\0';

	/* Allocate per-CPU trace buffers */
	if (tracefs_init_percpu_bufs() < 0) {
		kprintf("[!!] tracefs: failed to allocate per-CPU buffers\n");
		return;
	}

	/* Mount under /sys/kernel/trace */
	if (vfs_mount("/sys/kernel/trace", &tracefs_vfs_ops, NULL) == 0) {
		kprintf("[OK] tracefs mounted on /sys/kernel/trace\n");
	} else {
		kprintf("[!!] tracefs mount failed\n");
		tracefs_free_percpu_bufs();
		return;
	}

	tracefs_mounted = 1;
	tracing_global_enabled = 1;

	/* Create standard files */
	tracefs_create_entry("tracing_on", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_tracing_on,
			     tracefs_write_tracing_on, NULL);

	tracefs_create_entry("trace", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_trace, tracefs_write_trace, NULL);

	tracefs_create_entry("trace_marker", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_marker,
			     tracefs_write_marker, NULL);

	tracefs_create_entry("current_tracer", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_current_tracer,
			     tracefs_write_current_tracer, NULL);

	tracefs_create_entry("trace_stats", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_trace_stats, NULL, NULL);

	/* Per-CPU subdirectory structure */
	tracefs_create_percpu_files();

	/* Create buffer_size_kb at top level that mirrors CPU 0 */
	tracefs_create_entry("buffer_size_kb", TRACEFS_TYPE_FILE, 0,
			     tracefs_read_buffer_size,
			     tracefs_write_buffer_size,
			     (void *)(uintptr_t)0);

	kprintf("[tracefs] Per-CPU trace buffers ready (%d CPUs, %u KB each)\n",
		tracefs_nr_cpus, TRACEFS_BUF_SIZE / 1024);
}

EXPORT_SYMBOL(tracefs_write_entry);
EXPORT_SYMBOL(tracefs_enable);
EXPORT_SYMBOL(tracefs_disable);
EXPORT_SYMBOL(tracefs_is_enabled);
