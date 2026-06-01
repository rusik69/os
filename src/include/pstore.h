#ifndef PSTORE_H
#define PSTORE_H

#include "types.h"

#define PSTORE_MAX_RECORDS    256
#define PSTORE_MAX_DATA_LEN   256
#define PSTORE_RESERVE_PHYS   0x7FE00000ULL
#define PSTORE_REGION_SIZE    (64 * 1024)  /* 64 KB */

/* Record types */
#define PSTORE_TYPE_DMESG     1
#define PSTORE_TYPE_PANIC     2
#define PSTORE_TYPE_OOPS      3
#define PSTORE_TYPE_EMERG     4

/* Header for each pstore record */
struct pstore_record {
    uint8_t  type;
    uint8_t  len;
    uint16_t sequence;
    uint64_t timestamp;
    uint8_t  data[PSTORE_MAX_DATA_LEN];
} __attribute__((packed));

/* Write a record to persistent storage.
 * Returns 0 on success, negative on error. */
int pstore_write(uint8_t type, const uint8_t *data, int len);

/* Read a record by index (0-based).
 * Returns bytes read, or negative on error. */
int pstore_read(int index, uint8_t *buf, int len);

/* Get total number of valid records in the buffer. */
int pstore_get_count(void);

/* Initialize persistent storage. */
void pstore_init(void);

#endif /* PSTORE_H */
