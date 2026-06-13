#ifndef GHES_H
#define GHES_H

#include "types.h"

/* ── ACPI GHES: Generic Hardware Error Source ─────────────────────── */

/* CPER section type constants */
#define CPER_SEC_PROC_GENERIC       0
#define CPER_SEC_MEMORY             1
#define CPER_SEC_PCIE               2
#define CPER_SEC_PCI_BUS            3
#define CPER_SEC_SAL_RECORD         4

/* Error severity */
#define CPER_SEV_RECOVERABLE        0
#define CPER_SEV_FATAL              1
#define CPER_SEV_CORRECTED          2
#define CPER_SEV_INFO               3

/* GHES notification types */
#define GHES_NOTIFY_POLLED          0
#define GHES_NOTIFY_SCI             1
#define GHES_NOTIFY_NMI             2
#define GHES_NOTIFY_MCE             3
#define GHES_NOTIFY_EXTERNAL        4

/* Error handler callback type.
 * @sec_type:     CPER_SEC_* section type
 * @severity:     CPER_SEV_* error severity
 * @header:       pointer to the CPER section header
 * @payload:      pointer to the section payload data
 * @payload_len:  length of the payload in bytes
 */
typedef void (*ghes_error_handler_t)(uint32_t sec_type,
                                     uint32_t severity,
                                     const void *header,
                                     const uint8_t *payload,
                                     uint32_t payload_len);

/* Initialize the GHES subsystem */
void ghes_init(void);

/* Register a GHES error source (called by ACPI HEST parser) */
int ghes_register_source(int notify_type, uint64_t status_addr,
                         uint32_t status_length, int poll_interval_ms);

/* Enable or disable a GHES source */
int ghes_set_enabled(int source_id, int enabled);

/* Poll all registered GHES sources for pending errors */
int ghes_poll_all(void);

/* Process an error notification for a specific source */
int ghes_notify(int source_id);

/* Register a handler for CPER error events */
int ghes_register_handler(ghes_error_handler_t handler);

/* Get the number of registered GHES sources */
int ghes_source_count(void);

/* Get the status of a GHES source */
int ghes_source_status(int source_id, int *enabled, int *notify_type,
                       uint64_t *status_addr);

#endif /* GHES_H */
