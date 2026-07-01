/*
 * acpi_fan.c — ACPI fan control driver with _FST / _FSL evaluation
 *
 * Implements ACPI fan device detection and control using the AML
 * interpreter.  Each fan device (PNP0C0B) under _SB_ is probed for
 * _FST (Fan Status) and _FSL (Fan Set Level) control methods.
 *
 * _FST returns a Package:
 *   [0] Fan Speed        — DWORD (RPM, 0 = not spinning)
 *   [1] Fan Present      — DWORD (0 = not present, 1 = present)
 *   [2] Fan Set Level    — DWORD (0..100, current fan level)
 *
 * _FSL takes a DWORD argument (0..100) and sets the fan speed level.
 *
 * Reference: ACPI v6.3, Section 11.6 — Fan Device
 */

#include "acpi_fan.h"
#include "acpi.h"
#include "aml_exec.h"
#include "io.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "kernel.h"

/* ── Known ACPI fan device paths ────────────────────────────────────── */

static const char *g_fan_paths[] = {
    "\\_SB_.FAN0",
    "\\_SB_.FAN1",
    "\\_SB_.FAN2",
    "\\_SB_.FAN3",
    "\\_SB_.PCI0.SBRG.EC0.FAN0",
    "\\_SB_.PCI0.LPC.EC.FAN0",
    "\\_SB_.PCI0.LPCB.EC0.FAN0",
    "\\_SB_.PCI0.ISA.EC0.FAN0",
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

static struct acpi_fan_dev g_fans[MAX_ACPI_FANS];
static int g_fan_count = 0;
static int g_fan_init_done = 0;

/* ── Probe a single fan device path ──────────────────────────────────── */

static int fan_probe_device(int idx)
{
	struct acpi_fan_dev *fan;
	struct aml_object *result, *elem;
	char method_path[80];
	int fan_idx;

	if (idx < 0)
		return -1;

	if (g_fan_count >= MAX_ACPI_FANS)
		return -1;

	fan = &g_fans[g_fan_count];
	memset(fan, 0, sizeof(*fan));

	snprintf(fan->path, sizeof(fan->path), "%s", g_fan_paths[idx]);

	/* Try to evaluate _FST — if the method exists, the fan device is
	 * reachable in the namespace.  We use this as the primary probe. */
	snprintf(method_path, sizeof(method_path), "%s._FST", fan->path);
	result = aml_evaluate_method(method_path, NULL, 0);
	if (!result) {
		/* Fan device not present at this path */
		return -1;
	}

	/* Mark the fan present */
	fan->present = 1;
	fan->have_fst = 1;

	/* Parse _FST return value (Package with 3 DWORD elements) */
	if (result->type == AML_OBJ_PACKAGE && result->value.package.count >= 3) {
		elem = &result->value.package.elements[0];
		fan->speed = aml_obj_to_uint32(elem);

		/* Element [1]: Fan Present — already 1 if we got here */

		elem = &result->value.package.elements[2];
		fan->level = aml_obj_to_uint32(elem);
	} else {
		kprintf("[ACPI_FAN] _FST for %s returned unexpected type %u "
			"count %u\n",
			fan->path,
			(unsigned int)result->type,
			(unsigned int)(result->type == AML_OBJ_PACKAGE
				       ? result->value.package.count : 0));
		/* Still accept the fan as present even if parse failed */
	}

	aml_free_object(result);

	/* Check for _FSL (fan level set) support */
	snprintf(method_path, sizeof(method_path), "%s._FSL", fan->path);
	result = aml_evaluate_method(method_path, NULL, 0);
	if (result) {
		fan->have_fsl = 1;
		/* _FSL without arguments returns the current level */
		if (result->type == AML_OBJ_INTEGER) {
			fan->level = (uint32_t)(result->value.integer & 0xFFFFFFFFULL);
		}
		aml_free_object(result);
	}

	fan_idx = g_fan_count;
	g_fan_count++;

	kprintf("[ACPI_FAN] Found fan at %s (speed=%u RPM, level=%u, _FSL=%s)\n",
		fan->path,
		(unsigned int)fan->speed,
		(unsigned int)fan->level,
		fan->have_fsl ? "yes" : "no");

	return fan_idx;
}

/* ── Initialisation ────────────────────────────────────────────────── */

int __init acpi_fan_init(void)
{
	static const int num_paths = (int)(sizeof(g_fan_paths) /
					   sizeof(g_fan_paths[0]));

	if (g_fan_init_done)
		return g_fan_count;

	kprintf("[ACPI_FAN] Probing %d ACPI fan device paths...\n",
		num_paths);

	for (int i = 0; i < num_paths; i++) {
		if (fan_probe_device(i) >= 0) {
			if (g_fan_count >= MAX_ACPI_FANS)
				break;
		}
	}

	g_fan_init_done = 1;

	if (g_fan_count > 0) {
		kprintf("[ACPI_FAN] Found %d fan device(s)\n", g_fan_count);
	} else {
		kprintf("[ACPI_FAN] No ACPI fan devices found\n");
	}

	return g_fan_count;
}

/* ── Get fan count ──────────────────────────────────────────────────── */

int acpi_fan_get_count(void)
{
	if (!g_fan_init_done)
		return 0;
	return g_fan_count;
}

/* ── Get fan speed (RPM) via _FST ───────────────────────────────────── */

int acpi_fan_get_speed(int fan, uint32_t *speed)
{
	struct aml_object *result, *elem;
	char method_path[80];

	if (!speed)
		return -1;
	if (!g_fan_init_done || fan < 0 || fan >= g_fan_count)
		return -1;
	if (!g_fans[fan].present || !g_fans[fan].have_fst)
		return -1;

	snprintf(method_path, sizeof(method_path), "%s._FST",
		 g_fans[fan].path);

	result = aml_evaluate_method(method_path, NULL, 0);
	if (!result)
		return -1;

	if (result->type != AML_OBJ_PACKAGE || result->value.package.count < 1) {
		aml_free_object(result);
		return -1;
	}

	elem = &result->value.package.elements[0];
	*speed = aml_obj_to_uint32(elem);
	g_fans[fan].speed = *speed;

	/* Update cached level from _FST if available */
	if (result->value.package.count >= 3) {
		elem = &result->value.package.elements[2];
		g_fans[fan].level = aml_obj_to_uint32(elem);
	}

	aml_free_object(result);
	return 0;
}

/* ── Get fan level (0..100) via _FST ─────────────────────────────────── */

int acpi_fan_get_level(int fan, uint32_t *level)
{
	struct aml_object *result, *elem;
	char method_path[80];

	if (!level)
		return -1;
	if (!g_fan_init_done || fan < 0 || fan >= g_fan_count)
		return -1;
	if (!g_fans[fan].present || !g_fans[fan].have_fst)
		return -1;

	snprintf(method_path, sizeof(method_path), "%s._FST",
		 g_fans[fan].path);

	result = aml_evaluate_method(method_path, NULL, 0);
	if (!result)
		return -1;

	if (result->type != AML_OBJ_PACKAGE || result->value.package.count < 3) {
		aml_free_object(result);
		return -1;
	}

	elem = &result->value.package.elements[2];
	*level = aml_obj_to_uint32(elem);
	g_fans[fan].level = *level;

	/* Update cached speed */
	elem = &result->value.package.elements[0];
	g_fans[fan].speed = aml_obj_to_uint32(elem);

	aml_free_object(result);
	return 0;
}

/* ── Set fan level (0..100) via _FSL ────────────────────────────────── */

int acpi_fan_set_level(int fan, uint32_t level)
{
	struct aml_object *args[1];
	struct aml_object *arg;
	struct aml_object *result;
	char method_path[80];

	if (!g_fan_init_done || fan < 0 || fan >= g_fan_count)
		return -1;
	if (!g_fans[fan].present || !g_fans[fan].have_fsl)
		return -1;

	if (level > 100)
		return -EINVAL;

	/* Build argument: Integer with the desired fan level */
	arg = aml_create_integer(level);
	if (!arg)
		return -ENOMEM;

	args[0] = arg;

	snprintf(method_path, sizeof(method_path), "%s._FSL",
		 g_fans[fan].path);

	result = aml_evaluate_method(method_path, args, 1);
	aml_free_object(arg);

	if (!result) {
		return -1;
	}

	/* _FSL may return Integer (old level or status) or nothing */
	g_fans[fan].level = level;
	aml_free_object(result);

	return 0;
}

/* ── Print fan info ─────────────────────────────────────────────────── */

void acpi_fan_print_info(void)
{
	if (!g_fan_init_done || g_fan_count == 0) {
		kprintf("[ACPI_FAN] No fans detected\n");
		return;
	}

	kprintf("[ACPI_FAN] %d fan device(s):\n", g_fan_count);
	for (int i = 0; i < g_fan_count; i++) {
		struct acpi_fan_dev *fan = &g_fans[i];
		kprintf("  Fan %d: %s\n", i, fan->path);
		kprintf("    Present:  %s\n",
			fan->present ? "yes" : "no");
		kprintf("    _FST:     %s\n",
			fan->have_fst ? "yes" : "no");
		kprintf("    _FSL:     %s\n",
			fan->have_fsl ? "yes" : "no");
		kprintf("    Speed:    %u RPM\n",
			(unsigned int)fan->speed);
		kprintf("    Level:    %u%%\n",
			(unsigned int)fan->level);
	}
}
