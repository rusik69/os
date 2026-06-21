/*
 * ubsan.c — Undefined Behavior Sanitizer
 *
 * Implements handlers for Clang/GCC UndefinedBehaviorSanitizer (UBSan)
 * runtime checks.  When the compiler inserts __ubsan_* calls, they
 * are routed here for reporting.
 *
 * Supported checks:
 *   - Signed integer overflow (add/sub/mul/neg)
 *   - Division by zero
 *   - Shift out of bounds
 *   - Out-of-bounds array access
 *   - Misaligned pointer access
 *   - Null pointer dereference with non-null attribute
 *   - Function type mismatch
 *   - VLAs with non-positive bound
 */

#define KERNEL_INTERNAL
#include "printf.h"
#include "string.h"
#include "panic.h"

/* ── UBSan source location ──────────────────────────────────────── */

struct ubsan_source_location {
    const char *file;
    uint32_t line;
    uint32_t column;
};

struct ubsan_type_descriptor {
    uint16_t type_kind;
    uint16_t type_info;
    char     type_name[];
};

/* ── Type mismatch info ──────────────────────────────────────────── */

struct ubsan_type_mismatch_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
    uint8_t log_alignment;
    uint8_t type_check_kind;
};

/* ── Overflow data ────────────────────────────────────────────────── */

struct ubsan_overflow_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

struct ubsan_shift_out_of_bounds_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *lhs_type;
    struct ubsan_type_descriptor *rhs_type;
};

struct ubsan_out_of_bounds_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *array_type;
    struct ubsan_type_descriptor *index_type;
};

struct ubsan_unreachable_data {
    struct ubsan_source_location loc;
};

struct ubsan_vla_bound_data {
    struct ubsan_source_location loc;
    struct ubsan_type_descriptor *type;
};

/* ── Internal helper ─────────────────────────────────────────────── */

static void ubsan_log(const char *msg, struct ubsan_source_location *loc)
{
    if (loc && loc->file) {
        kprintf("[UBSan] %s at %s:%u:%u\n",
                msg, loc->file, loc->line, loc->column);
    } else {
        kprintf("[UBSan] %s (no location info)\n", msg);
    }
}

/* ── Handlers ─────────────────────────────────────────────────────── */

void __ubsan_handle_type_mismatch(struct ubsan_type_mismatch_data *data,
                                  uintptr_t ptr)
{
    if (!data) return;
    if (ptr == 0) {
        ubsan_log("NULL pointer dereference", &data->loc);
    } else if (data->log_alignment) {
        uintptr_t align = (uintptr_t)1 << data->log_alignment;
        if (ptr & (align - 1)) {
            kprintf("[UBSan] Misaligned pointer at %s:%u:%u: "
                    "ptr=0x%lx, expected alignment=%lu\n",
                    data->loc.file, data->loc.line, data->loc.column,
                    (unsigned long)ptr, (unsigned long)align);
        }
    }
}

void __ubsan_handle_type_mismatch_v1(struct ubsan_type_mismatch_data *data,
                                     uintptr_t ptr)
{
    __ubsan_handle_type_mismatch(data, ptr);
}

void __ubsan_handle_add_overflow(struct ubsan_overflow_data *data,
                                 uint64_t lhs, uint64_t rhs)
{
    ubsan_log("signed integer overflow (addition)", &data->loc);
}

void __ubsan_handle_sub_overflow(struct ubsan_overflow_data *data,
                                 uint64_t lhs, uint64_t rhs)
{
    ubsan_log("signed integer overflow (subtraction)", &data->loc);
}

void __ubsan_handle_mul_overflow(struct ubsan_overflow_data *data,
                                 uint64_t lhs, uint64_t rhs)
{
    ubsan_log("signed integer overflow (multiplication)", &data->loc);
}

void __ubsan_handle_negate_overflow(struct ubsan_overflow_data *data,
                                    uint64_t old_val)
{
    ubsan_log("signed integer overflow (negation)", &data->loc);
}

void __ubsan_handle_divrem_overflow(struct ubsan_overflow_data *data,
                                    uint64_t lhs, uint64_t rhs)
{
    if (rhs == 0) {
        ubsan_log("division by zero", &data->loc);
    } else {
        ubsan_log("signed integer overflow (division)", &data->loc);
    }
}

void __ubsan_handle_shift_out_of_bounds(
    struct ubsan_shift_out_of_bounds_data *data,
    uint64_t lhs, uint64_t rhs)
{
    ubsan_log("shift out of bounds", &data->loc);
}

void __ubsan_handle_out_of_bounds(struct ubsan_out_of_bounds_data *data,
                                  uintptr_t index)
{
    ubsan_log("array index out of bounds", &data->loc);
}

void __ubsan_handle_unreachable(struct ubsan_unreachable_data *data)
{
    ubsan_log("unreachable code reached", data ? &data->loc : NULL);
    panic("UBSan: unreachable");
}

void __ubsan_handle_vla_bound_not_positive(struct ubsan_vla_bound_data *data,
                                            uint64_t bound)
{
    ubsan_log("VLA with non-positive bound", &data->loc);
}

void __ubsan_handle_nonnull_return(struct ubsan_source_location *loc)
{
    ubsan_log("non-null return violated", loc);
}

void __ubsan_handle_nonnull_arg(struct ubsan_source_location *loc)
{
    ubsan_log("non-null argument violated", loc);
}

void __ubsan_handle_builtin_unreachable(struct ubsan_unreachable_data *data)
{
    ubsan_log("builtin unreachable reached", data ? &data->loc : NULL);
}

void __ubsan_handle_cfi_bad_type(struct ubsan_source_location *loc,
                                 uint16_t type, uint16_t expected)
{
    kprintf("[UBSan] CFI type mismatch at %s:%u:%u: "
            "type=0x%x expected=0x%x\n",
            loc->file, loc->line, loc->column, type, expected);
}

/* ── Initialization ──────────────────────────────────────────────── */

void ubsan_init(void)
{
    kprintf("[OK] UBSan: Undefined Behavior Sanitizer initialized\n");
}

/* ── Stub: ubsan_handle_overflow ─────────────────────────────── */
int ubsan_handle_overflow(void *data, void *lhs, void *rhs)
{
    (void)data;
    (void)lhs;
    (void)rhs;
    kprintf("[ubsan] ubsan_handle_overflow: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: ubsan_handle_out_of_bounds ─────────────────────────────── */
int ubsan_handle_out_of_bounds(void *data, void *index)
{
    (void)data;
    (void)index;
    kprintf("[ubsan] ubsan_handle_out_of_bounds: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: ubsan_handle_shift_out_of_bounds ─────────────────────────────── */
int ubsan_handle_shift_out_of_bounds(void *data, void *lhs, void *rhs)
{
    (void)data;
    (void)lhs;
    (void)rhs;
    kprintf("[ubsan] ubsan_handle_shift_out_of_bounds: not yet implemented\n");
    return -ENOSYS;
}
