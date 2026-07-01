/*
 * acpi_ac_adapter.c — ACPI AC adapter (power source) status driver
 *
 * Implements ACPI AC adapter device detection and power source status
 * monitoring using the AML interpreter.  Each AC adapter device
 * (ACPI0003) under _SB_ is probed for the _PSR (Power Source) control
 * method.
 *
 * _PSR returns a DWORD:
 *   0 = Offline (system is on battery power)
 *   1 = Online  (system is connected to AC power)
 *
 * The _PSR value is evaluated once during init and cached; subsequent
 * calls return the cached value.  Call acpi_ac_adapter_refresh() to
 * re-evaluate _PSR and update the cache.
 *
 * Reference: ACPI v6.3, Section 10.2.1 — AC Adapter Device.
 */

#include "acpi_ac_adapter.h"
#include "acpi.h"
#include "aml_exec.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "kernel.h"

/* ── Known ACPI AC adapter device paths ────────────────────────────── */

static const char *g_ac_paths[] = {
    "\\_SB_.AC0",
    "\\_SB_.AC1",
    "\\_SB_.AC2",
    "\\_SB_.AC3",
    "\\_SB_.AC",
    "\\_SB_.PCI0.AC",
    "\\_SB_.PCI0.LPCB.AC0",
    "\\_SB_.PCI0.SBRG.EC0.AC0",
    "\\_SB_.PCI0.LPC.EC.AC0",
    "\\_SB_.PCI0.ISA.EC0.AC0",
};

/* ── AML helper: extract uint32 from AML object ────────────────────── */

static uint32_t aml_obj_to_uint32(struct aml_object *obj)
{
	if (!obj)
		return 0;
	if (obj->type == AML_OBJ_INTEGER)
		return (uint32_t)(obj->value.integer & 0xFFFFFFFFULL);
	return 0;
}

/* ── Static state ──────────────────────────────────────────────────── */

static struct acpi_ac_adapter_dev g_ac_adapters[MAX_ACPI_AC_ADAPTERS];
static int g_ac_adapter_count = 0;
static int g_ac_adapter_init_done = 0;

/* ── Probe a single AC adapter device path ──────────────────────────── */

static int ac_probe_device(int idx)
{
	struct acpi_ac_adapter_dev *ac;
	struct aml_object *result;
	char method_path[80];
	int ac_idx;

	if (idx < 0)
		return -1;

	if (g_ac_adapter_count >= MAX_ACPI_AC_ADAPTERS)
		return -1;

	/* Verify the device node exists in the namespace */
	struct aml_ns_node *dev_node = aml_ns_lookup(g_ac_paths[idx]);
	if (!dev_node)
		return -1;
	if (dev_node->type != AML_NS_DEVICE)
		return -1;

	ac = &g_ac_adapters[g_ac_adapter_count];
	memset(ac, 0, sizeof(*ac));

	snprintf(ac->path, sizeof(ac->path), "%s", g_ac_paths[idx]);

	/* Check for _PSR method */
	snprintf(method_path, sizeof(method_path), "%s._PSR", ac->path);
	result = aml_evaluate_method(method_path, NULL, 0);
	if (!result) {
		/* AC adapter device exists but no _PSR method */
		return -1;
	}

	/* Mark the AC adapter present */
	ac->present = 1;
	ac->have_psr = 1;

	/* Parse _PSR return value (Integer: 0=offline, 1=online) */
	ac->last_status = aml_obj_to_uint32(result);
	aml_free_object(result);

	ac_idx = g_ac_adapter_count;
	g_ac_adapter_count++;

	kprintf("[ACPI_AC] Found AC adapter at %s (status=%s)\n",
		ac->path,
		ac->last_status ? "online" : "offline");

	return ac_idx;
}

/* ── Initialisation ────────────────────────────────────────────────── */

int __init acpi_ac_adapter_init(void)
{
	static const int num_paths = (int)(sizeof(g_ac_paths) /
					   sizeof(g_ac_paths[0]));

	if (g_ac_adapter_init_done)
		return g_ac_adapter_count;

	kprintf("[ACPI_AC] Probing %d ACPI AC adapter paths...\n",
		num_paths);

	for (int i = 0; i < num_paths; i++) {
		if (ac_probe_device(i) >= 0) {
			if (g_ac_adapter_count >= MAX_ACPI_AC_ADAPTERS)
				break;
		}
	}

	g_ac_adapter_init_done = 1;

	if (g_ac_adapter_count > 0) {
		kprintf("[ACPI_AC] Found %d AC adapter device(s)\n",
			g_ac_adapter_count);
	} else {
		kprintf("[ACPI_AC] No ACPI AC adapter devices found\n");
	}

	return g_ac_adapter_count;
}

/* ── Get AC adapter count ──────────────────────────────────────────── */

int acpi_ac_adapter_get_count(void)
{
	if (!g_ac_adapter_init_done)
		return 0;
	return g_ac_adapter_count;
}

/* ── Check if an AC adapter is online ───────────────────────────────── */

int acpi_ac_adapter_is_online(int idx)
{
	if (!g_ac_adapter_init_done || idx < 0 || idx >= g_ac_adapter_count)
		return -EINVAL;
	if (!g_ac_adapters[idx].present)
		return -ENODEV;

	return (int)g_ac_adapters[idx].last_status;
}

/* ── Get cached AC adapter status ──────────────────────────────────── */

int acpi_ac_adapter_get_status(int idx, uint32_t *status)
{
	if (!status)
		return -EINVAL;
	if (!g_ac_adapter_init_done || idx < 0 || idx >= g_ac_adapter_count)
		return -EINVAL;
	if (!g_ac_adapters[idx].present)
		return -ENODEV;

	*status = g_ac_adapters[idx].last_status;
	return 0;
}

/* ── Refresh AC adapter status by re-evaluating _PSR ────────────────── */

int acpi_ac_adapter_refresh(int idx)
{
	struct aml_object *result;
	char method_path[80];

	if (!g_ac_adapter_init_done || idx < 0 || idx >= g_ac_adapter_count)
		return -EINVAL;
	if (!g_ac_adapters[idx].present || !g_ac_adapters[idx].have_psr)
		return -ENODEV;

	snprintf(method_path, sizeof(method_path), "%s._PSR",
		 g_ac_adapters[idx].path);

	result = aml_evaluate_method(method_path, NULL, 0);
	if (!result)
		return -EIO;

	g_ac_adapters[idx].last_status = aml_obj_to_uint32(result);
	aml_free_object(result);

	return 0;
}

/* ── Print AC adapter info ─────────────────────────────────────────── */

void acpi_ac_adapter_print_info(void)
{
	if (!g_ac_adapter_init_done || g_ac_adapter_count == 0) {
		kprintf("[ACPI_AC] No AC adapters detected\n");
		return;
	}

	kprintf("[ACPI_AC] %d AC adapter device(s):\n", g_ac_adapter_count);
	for (int i = 0; i < g_ac_adapter_count; i++) {
		struct acpi_ac_adapter_dev *ac = &g_ac_adapters[i];
		kprintf("  AC %d: %s\n", i, ac->path);
		kprintf("    Present:  %s\n",
			ac->present ? "yes" : "no");
		kprintf("    _PSR:     %s\n",
			ac->have_psr ? "yes" : "no");
		kprintf("    Status:   %s\n",
			ac->last_status ? "online" : "offline");
	}
}
