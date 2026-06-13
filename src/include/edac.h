#ifndef EDAC_H
#define EDAC_H

#include "types.h"

/* ── EDAC: DRAM ECC error detection ──────────────────────────────── */

/* Forward declaration */
struct edac_controller;

/* Initialize the EDAC subsystem */
void edac_init(void);

/* Register a memory controller for EDAC monitoring */
int edac_register_controller(const char *name, int type,
                             int (*poll_fn)(struct edac_controller *));

/* Unregister a memory controller */
int edac_unregister_controller(int ctl_id);

/* Report a correctable error (CE) */
void edac_ce_error(int ctl_id, int csrow, int channel, uint64_t addr);

/* Report an uncorrectable error (UE) */
void edac_ue_error(int ctl_id, int csrow, int channel, uint64_t addr);

/* Configure CSROW layout for a controller */
int edac_setup_csrows(int ctl_id, int num_csrows);

/* Set custom error handlers for a controller */
void edac_set_handlers(int ctl_id,
                       void (*ce_fn)(int, int, int, uint64_t),
                       void (*ue_fn)(int, int, int, uint64_t));

/* Poll all registered controllers for errors */
int edac_poll_all(void);

/* Get EDAC statistics for a controller */
int edac_get_stats(int ctl_id, uint64_t *ce, uint64_t *ue, int *type);

/* Dump EDAC status for all controllers */
void edac_dump_status(void);

/* Controller type constants */
#define EDAC_CTL_UNKNOWN  0
#define EDAC_CTL_SBRIDGE  1
#define EDAC_CTL_IBRIDGE  2
#define EDAC_CTL_HASWELL  3
#define EDAC_CTL_SKX      4
#define EDAC_CTL_ZEN      5

#endif /* EDAC_H */
