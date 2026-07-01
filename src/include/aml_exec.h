#ifndef AML_EXEC_H
#define AML_EXEC_H

#include "types.h"

/* ── AML Object Types ───────────────────────────────────────────── */

/* ACPI AML data object types */
#define AML_OBJ_UNDEFINED  0   /* Uninitialized / no value */
#define AML_OBJ_INTEGER    1   /* 64-bit unsigned integer */
#define AML_OBJ_STRING     2   /* Null-terminated string */
#define AML_OBJ_BUFFER     3   /* Raw byte buffer */
#define AML_OBJ_PACKAGE    4   /* Array of AML objects */
#define AML_OBJ_REFERENCE  5   /* Reference to a namespace node */
#define AML_OBJ_OPREGION   6   /* Operation region descriptor */

/* Maximum number of method arguments (ACPI spec: Arg0-Arg6 = 7) */
#define AML_MAX_ARGS   7

/* Maximum number of local variables (Local0-Local7 = 8) */
#define AML_MAX_LOCALS 8

/* Maximum number of method-local named objects (NameOp inside method body) */
#define AML_MAX_LOCAL_NAMES 16

/* ── Method-Local Named Object ────────────────────────────────────── */

/*
 * Represents a named object created by NameOp inside a method body.
 * These are temporary names that exist only for the method's duration.
 */
struct aml_local_name_entry {
    char name[4];               /* 4-byte NameSeg */
    struct aml_object *value;   /* The evaluated value */
};

/* ── Operation Region Descriptor ──────────────────────────────────── */

/* Operation region descriptor — stored in a namespace node's value field */
struct aml_opregion_info {
    uint8_t  space_id;     /* AML_OPREGION_* (0-9) */
    uint64_t region_offset;/* Starting offset/address */
    uint64_t region_length;/* Size in bytes */
};

/* ── AML Object Structure ───────────────────────────────────────── */

/*
 * An AML data object representing any ACPI data type.
 * Used for passing arguments and return values between methods.
 */
struct aml_object {
    uint8_t  type;      /* AML_OBJ_* type */
    uint8_t  from_heap; /* 1 = allocated via kmalloc, needs kfree */
    uint16_t ref_count; /* Not yet used; reserved */
    union {
        uint64_t integer;          /* AML_OBJ_INTEGER */
        struct {
            char    *ptr;          /* Null-terminated string */
            uint32_t len;          /* Length excluding null terminator */
        } string;                  /* AML_OBJ_STRING */
        struct {
            uint8_t *data;         /* Raw buffer data */
            uint32_t length;       /* Buffer length in bytes */
        } buffer;                  /* AML_OBJ_BUFFER */
        struct {
            struct aml_object *elements; /* Array of objects */
            uint32_t count;              /* Number of elements */
        } package;                 /* AML_OBJ_PACKAGE */
        struct {
            int node_index;        /* Namespace node index */
        } ref;                     /* AML_OBJ_REFERENCE */
        struct aml_opregion_info opregion; /* AML_OBJ_OPREGION */
    } value;
};

/* ── AML Control Method Evaluation API ──────────────────────────── */

/*
 * Evaluate an ACPI control method by its full namespace path.
 *
 * @param path      Absolute namespace path (e.g. "\\_SB_.PCI0.EC0._BIF")
 * @param args      Array of argument objects (may be NULL if num_args == 0)
 * @param num_args  Number of arguments (0-7)
 *
 * Returns a newly allocated aml_object with the method's return value.
 * The caller must free the returned object with aml_free_object().
 * Returns NULL on error (method not found, execution failure).
 */
struct aml_object *aml_evaluate_method(const char *path,
                                       struct aml_object *args[],
                                       int num_args);

/*
 * Free an AML object and all its owned resources.
 * Safe to call with NULL (no-op).
 */
void aml_free_object(struct aml_object *obj);

/*
 * Create an integer AML object (heap-allocated).
 * Returns NULL on allocation failure.
 */
struct aml_object *aml_create_integer(uint64_t value);

/*
 * Create a string AML object (heap-allocated, copies the string).
 * Returns NULL on allocation failure.
 */
struct aml_object *aml_create_string(const char *str);

/*
 * Create a buffer AML object (heap-allocated, copies the data).
 * Returns NULL on allocation failure.
 */
struct aml_object *aml_create_buffer(const uint8_t *data, uint32_t length);

/*
 * Create a reference AML object pointing to a namespace node.
 * Returns NULL if node_idx is invalid.
 */
struct aml_object *aml_create_reference(int node_index);

/*
 * Create a package AML object containing an array of sub-objects.
 * The elements array is deep-copied; the caller may free the source
 * array after calling.  Returns NULL on allocation failure (partial
 * cleanup is performed so no elements are leaked).
 */
struct aml_object *aml_create_package(const struct aml_object *elements,
                                      uint32_t count);

/* ── Operation Region Support ─────────────────────────────────────── */

/* ACPI operation region space IDs (ACPI v6.3, Section 5.5.2.4) */
#define AML_OPREGION_SYSTEM_MEMORY    0
#define AML_OPREGION_SYSTEM_IO        1
#define AML_OPREGION_PCI_CONFIG       2
#define AML_OPREGION_EMBEDDED_CONTROL 3
#define AML_OPREGION_SMBUS            4
#define AML_OPREGION_SYSTEM_CMOS      5
#define AML_OPREGION_PCI_BAR_TARGET   6
#define AML_OPREGION_IPMI             7
#define AML_OPREGION_GPIO             8
#define AML_OPREGION_SERIAL_BUS       9

/* Maximum number of registered operation region handlers */
#define AML_OPREGION_MAX_HANDLERS     16

/* Access context for operation region read/write */
struct aml_opregion_access {
    uint8_t  *buf;         /* Data buffer */
    uint32_t  length;      /* Buffer length in bytes */
    uint64_t  offset;      /* Byte offset within the region */
    int       write;       /* 1 = write, 0 = read */
};

/*
 * Operation region access handler callback.
 * Called when AML code reads/writes a field defined on an operation region.
 *
 * @param region   The operation region info (space_id, offset, length)
 * @param access   Access context (buffer, offset, length, direction)
 * @param context  Opaque user data registered with the handler
 *
 * Returns 0 on success, negative on error.
 */
typedef int (*aml_opregion_handler_t)(const struct aml_opregion_info *region,
                                      struct aml_opregion_access *access,
                                      void *context);

/*
 * Register an operation region handler for a given space ID.
 * Only one handler per space ID is allowed; a second registration
 * replaces the first.
 *
 * @param space_id  AML_OPREGION_* value
 * @param handler   The callback function
 * @param context   Opaque user data passed to the handler
 *
 * Returns 0 on success, -1 if handler table is full.
 */
int aml_opregion_register_handler(uint8_t space_id,
                                  aml_opregion_handler_t handler,
                                  void *context);

/*
 * Remove a previously registered operation region handler.
 * Safe to call with a handler that was never registered (no-op).
 */
void aml_opregion_unregister_handler(uint8_t space_id);

/*
 * Access an operation region at the given byte offset.
 * Routes through the registered handler for the region's space_id.
 *
 * @param region   The operation region descriptor
 * @param offset   Byte offset within the region
 * @param buf      Data buffer for read/write
 * @param length   Number of bytes to transfer
 * @param write    0 = read, 1 = write
 *
 * Returns 0 on success, negative on error (-ENOENT if no handler, -EIO on error).
 */
int aml_opregion_access(const struct aml_opregion_info *region,
                        uint64_t offset, uint8_t *buf,
                        uint32_t length, int write);

/*
 * Create an AML object containing operation region information.
 * This is stored in the namespace node's value field when parsing OpRegion.
 * Returns NULL on allocation failure.
 */
struct aml_object *aml_create_opregion(uint8_t space_id,
                                       uint64_t region_offset,
                                       uint64_t region_length);

#endif /* AML_EXEC_H */
