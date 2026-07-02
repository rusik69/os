/*
 * sysfs_numa.c — NUMA node attributes under /sys/devices/system/node/
 *
 * Creates Linux-compatible per-node attributes exposing CPU topology,
 * memory information, and distance data for each NUMA node.
 *
 * Layout:
 *   /sys/devices/system/node/
 *   /sys/devices/system/node/node<N>/
 *   /sys/devices/system/node/node<N>/cpumap     — CPU bitmask (hex)
 *   /sys/devices/system/node/node<N>/cpulist    — CPU list (0-3,5,7-15)
 *   /sys/devices/system/node/node<N>/distance   — distances to all nodes
 *   /sys/devices/system/node/node<N>/meminfo    — per-node memory statistics
 *   /sys/devices/system/node/node<N>/numastat   — NUMA allocation statistics
 *   /sys/devices/system/node/has_cpu            — nodes that have CPUs
 *   /sys/devices/system/node/has_memory         — nodes that have memory
 *   /sys/devices/system/node/has_normal_memory  — nodes with normal (non-ZONE_DEVICE) memory
 *   /sys/devices/system/node/online             — online node list
 *   /sys/devices/system/node/possible           — possible node list
 */

#include "sysfs.h"
#include "cpu_topology.h"
#include "numa_mem.h"
#include "string.h"
#include "printf.h"
#include "types.h"
#include "smp.h"        /* smp_get_cpu_count() */
#include "pmm.h"        /* pmm_get_total_frames(), pmm_get_used_frames() */

/* ── Forward declarations ──────────────────────────────────────────── */
static void sysfs_create_one_node_dir(int node);

/* ── Helpers ──────────────────────────────────────────────────────── */

/*
 * Format a cpulist string from a CPU bitmask.
 * Writes to buf (max_sz bytes) in the form "0-3,5,7-11\n".
 * Returns the number of bytes written.
 */
static int sysfs_format_cpulist(char *buf, uint32_t max_sz, uint64_t mask)
{
	int pos = 0;
	int start = -1;
	int prev = -2;
	int first = 1;

	for (int i = 0; i < 64 && (uint32_t)pos + 16 < max_sz; i++) {
		if (mask & (1ULL << i)) {
			if (start < 0)
				start = i;
			prev = i;
		} else {
			if (start >= 0) {
				if (!first) {
					if ((uint32_t)pos < max_sz)
						buf[pos++] = ',';
				}
				first = 0;
				if (prev > start) {
					int n = snprintf(buf + pos, max_sz - (uint32_t)pos,
							 "%d-%d", start, prev);
					if (n > 0) pos += n;
				} else {
					int n = snprintf(buf + pos, max_sz - (uint32_t)pos,
							 "%d", start);
					if (n > 0) pos += n;
				}
				start = -1;
			}
		}
	}
	/* Handle trailing run */
	if (start >= 0) {
		if (!first && (uint32_t)pos < max_sz)
			buf[pos++] = ',';
		if (prev > start) {
			int n = snprintf(buf + pos, max_sz - (uint32_t)pos,
					 "%d-%d", start, prev);
			if (n > 0) pos += n;
		} else {
			int n = snprintf(buf + pos, max_sz - (uint32_t)pos,
					 "%d", start);
			if (n > 0) pos += n;
		}
	}

	if ((uint32_t)pos < max_sz)
		buf[pos++] = '\n';
	if ((uint32_t)pos < max_sz)
		buf[pos] = '\0';

	return pos;
}

/* ── Read callbacks ───────────────────────────────────────────────── */

/*
 * Read callback for /sys/devices/system/node/node<N>/cpumap.
 * Returns the CPU bitmask of this node as a hex string.
 */
static int sysfs_read_node_cpumap(char *buf, uint32_t max_sz, void *priv)
{
	int node = (int)(uintptr_t)priv;
	uint64_t mask = numa_cpus_of_node(node);
	int n = snprintf(buf, max_sz, "%016llx\n", (unsigned long long)mask);
	if (n < 0) return -EINVAL;
	return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Read callback for /sys/devices/system/node/node<N>/cpulist.
 * Returns the CPU list as a human-readable range string.
 */
static int sysfs_read_node_cpulist(char *buf, uint32_t max_sz, void *priv)
{
	int node = (int)(uintptr_t)priv;
	uint64_t mask = numa_cpus_of_node(node);
	int ret = sysfs_format_cpulist(buf, max_sz, mask);
	return ret > 0 ? ret : -EINVAL;
}

/*
 * Read callback for /sys/devices/system/node/node<N>/distance.
 * Returns the distance from this node to every other node in
 * ACPI SLIT-compatible format (space-separated integers).
 */
static int sysfs_read_node_distance(char *buf, uint32_t max_sz, void *priv)
{
	int node = (int)(uintptr_t)priv;
	int ncpus_node = numa_node_count;
	if (ncpus_node < 1) ncpus_node = 1;
	if (ncpus_node > NUMA_MAX_NODES) ncpus_node = NUMA_MAX_NODES;

	int pos = 0;
	for (int i = 0; i < ncpus_node; i++) {
		unsigned int dist = numa_distance(node, i);
		int n = snprintf(buf + pos, max_sz - (uint32_t)pos,
				 "%s%u", (i > 0) ? " " : "", dist);
		if (n > 0)
			pos += n;
		else
			break;
		if ((uint32_t)pos >= max_sz)
			break;
	}
	if ((uint32_t)pos + 1 < max_sz) {
		buf[pos++] = '\n';
		buf[pos] = '\0';
	}
	return pos;
}

/*
 * Read callback for /sys/devices/system/node/node<N>/meminfo.
 * Returns per-node memory statistics in Linux-compatible format:
 *   Node <N> MemTotal:     <KB> kB
 *   Node <N> MemFree:      <KB> kB
 *   Node <N> MemUsed:      <KB> kB
 *   Node <N> Active:       <KB> kB
 *   Node <N> Inactive:     <KB> kB
 */
static int sysfs_read_node_meminfo(char *buf, uint32_t max_sz, void *priv)
{
	int node = (int)(uintptr_t)priv;
	uint64_t start = numa_node_memory_start[node];
	uint64_t end   = numa_node_memory_end[node];
	uint64_t total_kb, free_kb, used_kb;
	uint64_t total_pages = 0;

	if (end > start) {
		total_pages = (end - start) / PAGE_SIZE;
	}

	total_kb = (total_pages * PAGE_SIZE) / 1024;
	free_kb  = 0;
	used_kb  = 0;

	if (numa_node_has_memory(node)) {
		uint64_t global_total = pmm_get_total_frames();
		uint64_t global_used  = pmm_get_used_frames();
		uint64_t total_frames = (end - start) / PAGE_SIZE;

		/*
		 * Estimate per-node usage proportionally.
		 * In a full implementation the PMM would track
		 * per-node frame counts directly.
		 */
		if (global_total > 0) {
			uint64_t node_ratio = (total_frames << 16) / global_total;
			used_kb = (uint64_t)(((unsigned long long)global_used * node_ratio) >> 16) * PAGE_SIZE / 1024;
		}
		if (total_kb > used_kb)
			free_kb = total_kb - used_kb;
	}

	int n = snprintf(buf, max_sz,
			 "Node %d MemTotal:     %llu kB\n"
			 "Node %d MemFree:      %llu kB\n"
			 "Node %d MemUsed:      %llu kB\n"
			 "Node %d Active:       %llu kB\n"
			 "Node %d Inactive:     %llu kB\n",
			 node, (unsigned long long)total_kb,
			 node, (unsigned long long)free_kb,
			 node, (unsigned long long)used_kb,
			 node, (unsigned long long)total_kb,
			 node, 0ULL);
	if (n < 0) return -EINVAL;
	return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/*
 * Read callback for /sys/devices/system/node/node<N>/numastat.
 * Returns NUMA allocation statistics (per-node).
 */
static int sysfs_read_node_numastat(char *buf, uint32_t max_sz, void *priv)
{
	int node = (int)(uintptr_t)priv;
	(void)node;
	/*
	 * Placeholder statistics.  In a full implementation these would
	 * be counters incremented by the NUMA-aware page allocator.
	 */
	int n = snprintf(buf, max_sz,
			 "numa_hit %llu\n"
			 "numa_miss %llu\n"
			 "numa_foreign %llu\n"
			 "interleave_hit %llu\n"
			 "local_node %llu\n"
			 "other_node %llu\n",
			 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL);
	if (n < 0) return -EINVAL;
	return (uint32_t)n < max_sz ? n : (int)(max_sz - 1);
}

/* ── Top-level aggregate files ────────────────────────────────────── */

/*
 * Read callback for /sys/devices/system/node/has_cpu.
 * Lists all nodes that have at least one CPU.
 */
static int sysfs_read_has_cpu(char *buf, uint32_t max_sz, void *priv)
{
	(void)priv;
	int nodes = numa_node_count;
	if (nodes < 1) nodes = 1;
	if (nodes > NUMA_MAX_NODES) nodes = NUMA_MAX_NODES;

	uint64_t mask = 0;
	for (int i = 0; i < nodes; i++) {
		if (numa_cpus_of_node(i) != 0)
			mask |= (1ULL << i);
	}
	int ret = sysfs_format_cpulist(buf, max_sz, mask);
	return ret > 0 ? ret : -EINVAL;
}

/*
 * Read callback for /sys/devices/system/node/has_memory.
 * Lists all nodes that have physical memory.
 */
static int sysfs_read_has_memory(char *buf, uint32_t max_sz, void *priv)
{
	(void)priv;
	int nodes = numa_node_count;
	if (nodes < 1) nodes = 1;
	if (nodes > NUMA_MAX_NODES) nodes = NUMA_MAX_NODES;

	uint64_t mask = 0;
	for (int i = 0; i < nodes; i++) {
		if (numa_node_has_memory(i))
			mask |= (1ULL << i);
	}
	int ret = sysfs_format_cpulist(buf, max_sz, mask);
	return ret > 0 ? ret : -EINVAL;
}

/*
 * Read callback for /sys/devices/system/node/online.
 * Lists all online NUMA nodes.
 */
static int sysfs_read_online_nodes(char *buf, uint32_t max_sz, void *priv)
{
	(void)priv;
	int nodes = numa_node_count;
	if (nodes < 1) nodes = 1;
	if (nodes > NUMA_MAX_NODES) nodes = NUMA_MAX_NODES;

	uint64_t mask = 0;
	for (int i = 0; i < nodes; i++)
		mask |= (1ULL << i); /* all detected nodes are online */
	int ret = sysfs_format_cpulist(buf, max_sz, mask);
	return ret > 0 ? ret : -EINVAL;
}

/*
 * Read callback for /sys/devices/system/node/possible.
 * Lists all possible NUMA nodes (max capacity).
 */
static int sysfs_read_possible_nodes(char *buf, uint32_t max_sz, void *priv)
{
	(void)priv;
	uint64_t mask = 0;
	for (int i = 0; i < NUMA_MAX_NODES; i++)
		mask |= (1ULL << i);
	int ret = sysfs_format_cpulist(buf, max_sz, mask);
	return ret > 0 ? ret : -EINVAL;
}

/* ── Top-level creation ──────────────────────────────────────────── */

/*
 * Create a single per-node directory with all its attribute files.
 */
static void sysfs_create_one_node_dir(int node)
{
	char dirpath[64];
	int n;

	n = snprintf(dirpath, sizeof(dirpath),
		     "/sys/devices/system/node/node%d", node);
	if (n < 0 || (uint32_t)n >= sizeof(dirpath))
		return;

	sysfs_create_dir(dirpath);

	/* cpumap — CPU bitmask */
	char path[80];
	snprintf(path, sizeof(path), "%s/cpumap", dirpath);
	sysfs_create_writable_file(path, "\n",
				   (void *)(uintptr_t)node,
				   sysfs_read_node_cpumap, NULL);

	/* cpulist — CPU list */
	snprintf(path, sizeof(path), "%s/cpulist", dirpath);
	sysfs_create_writable_file(path, "\n",
				   (void *)(uintptr_t)node,
				   sysfs_read_node_cpulist, NULL);

	/* distance — node distances */
	snprintf(path, sizeof(path), "%s/distance", dirpath);
	sysfs_create_writable_file(path, "\n",
				   (void *)(uintptr_t)node,
				   sysfs_read_node_distance, NULL);

	/* meminfo — per-node memory statistics */
	snprintf(path, sizeof(path), "%s/meminfo", dirpath);
	sysfs_create_writable_file(path, "\n",
				   (void *)(uintptr_t)node,
				   sysfs_read_node_meminfo, NULL);

	/* numastat — NUMA allocation statistics */
	snprintf(path, sizeof(path), "%s/numastat", dirpath);
	sysfs_create_writable_file(path, "\n",
				   (void *)(uintptr_t)node,
				   sysfs_read_node_numastat, NULL);
}

/*
 * sysfs_create_numa_dirs() — Create /sys/devices/system/node/ hierarchy.
 *
 * Called from sysfs_init() after the firmware directories are created.
 * Creates the per-node directory tree exposing CPU affinity, memory
 * information, and distance data for each detected NUMA node.
 */
void sysfs_create_numa_dirs(void)
{
	int nodes = numa_node_count;
	if (nodes < 1) nodes = 1;
	if (nodes > NUMA_MAX_NODES) nodes = NUMA_MAX_NODES;

	/* Create /sys/devices/system/node/ directory.
	 * Note: /sys/devices/system/ is created by sysfs_create_cpu_hotplug_files(). */
	sysfs_create_dir("/sys/devices/system/node");

	/* Create aggregate topology files */
	sysfs_create_writable_file("/sys/devices/system/node/has_cpu", "\n",
				   NULL, sysfs_read_has_cpu, NULL);
	sysfs_create_writable_file("/sys/devices/system/node/has_memory", "\n",
				   NULL, sysfs_read_has_memory, NULL);
	sysfs_create_writable_file("/sys/devices/system/node/has_normal_memory", "\n",
				   NULL, sysfs_read_has_memory, NULL);
	sysfs_create_writable_file("/sys/devices/system/node/online", "\n",
				   NULL, sysfs_read_online_nodes, NULL);
	sysfs_create_writable_file("/sys/devices/system/node/possible", "\n",
				   NULL, sysfs_read_possible_nodes, NULL);

	/* Create per-node directories */
	for (int i = 0; i < nodes; i++)
		sysfs_create_one_node_dir(i);

	kprintf("[sysfs] Created NUMA node directories for %d node(s)\n", nodes);
}

/* ── Module info ────────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("sysfs NUMA node attributes — per-node CPU, memory, and distance information");
MODULE_AUTHOR("OS Kernel Team");
