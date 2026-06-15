#ifndef IMA_H
#define IMA_H

/*
 * ima.h — Integrity Measurement Architecture (IMA) for Hermes OS
 *
 * Provides file integrity measurement and appraisal using SHA256
 * and TPM 2.0 PCR 10 extension for remote attestation.
 *
 * Functions:
 *   ima_measure()  — hash a file and log the measurement
 *   ima_appraise() — appraise a file against security.ima xattr
 *   ima_init()     — called during boot to enable IMA
 *   ima_buf_read() — read measurement log into a buffer
 */

#include "types.h"

/* IMA measurement type constants */
#define IMA_FILE_READ  0  /* type for file reads */
#define IMA_FILE_EXEC  1  /* type for file execute */

/**
 * ima_measure — Measure a file by hashing its contents with SHA256,
 *               extending TPM PCR 10 with the hash, and logging it.
 *
 * @path:  Absolute path to the file
 * @type:  IMA_FILE_READ (0) or IMA_FILE_EXEC (1)
 *
 * Returns 0 on success, negative errno on failure.
 */
int ima_measure(const char *path, int type);

/**
 * ima_appraise — Appraise a file by comparing its SHA256 hash against
 *                the security.ima extended attribute.
 *
 * @path:  Absolute path to the file
 *
 * Returns 0 on success (hash matches), -EACCES on mismatch,
 * negative errno on other error.
 */
int ima_appraise(const char *path);

/**
 * ima_init — Initialise the IMA subsystem.
 *
 * Zeroes the measurement log, creates /sys/kernel/security entries,
 * and logs a boot-time kernel measurement. Called during kernel boot.
 */
void ima_init(void);

/**
 * ima_buf_read — Copy the measurement log into a caller-supplied buffer
 *                for attestation or userspace consumption.
 *
 * @buf:   Destination buffer
 * @size:  Size of destination buffer
 *
 * Returns the number of bytes written, or negative errno.
 */
int ima_buf_read(char *buf, int size);

#endif /* IMA_H */
