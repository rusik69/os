/*
 * dlfcn.h — dlopen/dlsym/dlclose/dlerror API (Item U19)
 *
 * Provides POSIX-compatible dynamic linking loader interface for
 * loading ELF shared objects (.so files) at runtime.
 *
 * Flags:
 *   RTLD_LAZY   — resolve symbols as needed (default)
 *   RTLD_NOW    — resolve all symbols before returning
 *   RTLD_LOCAL  — symbols not exported to other libraries (default)
 *   RTLD_GLOBAL — symbols exported to other libraries
 *
 * Special handles:
 *   RTLD_DEFAULT — search all loaded libraries for symbol
 *   RTLD_NEXT    — find next occurrence of symbol (for dlsym wrappers)
 */

#ifndef DLFCN_H
#define DLFCN_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── dlopen flags ─────────────────────────────────────────────────── */
#define RTLD_LAZY    0x00001  /* resolve undefined symbols lazily */
#define RTLD_NOW     0x00002  /* resolve all symbols immediately */
#define RTLD_BINDING_MASK 0x00003

#define RTLD_NOLOAD  0x00004  /* don't load, just test if loaded */
#define RTLD_DEEPBIND 0x00008 /* use deep binding (library first) */
#define RTLD_GLOBAL  0x00100  /* symbols available to others */
#define RTLD_LOCAL   0x00000  /* symbols not available (default) */

/* ── Special handles for dlsym ────────────────────────────────────── */
#define RTLD_DEFAULT ((void*)0)   /* search all libs in load order */
#define RTLD_NEXT    ((void*)-1L) /* search all libs except caller */

/* ── dlclose semantics ────────────────────────────────────────────── */
/* If RTLD_NODELETE is not set, the handle may be closed. */
#define RTLD_NODELETE 0x01000

/* ── Core API ─────────────────────────────────────────────────────── */

/*
 * dlopen — Load a shared object (.so) into the process's address space.
 *
 * @filename  Path to the shared object, or NULL for the main program.
 * @flags     RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL | RTLD_LOCAL | ...
 *
 * @return    Opaque handle on success, NULL on error (use dlerror()).
 */
void *dlopen(const char *filename, int flags);

/*
 * dlsym — Get the address of a symbol defined in a loaded library.
 *
 * @handle    Handle from dlopen(), or RTLD_DEFAULT / RTLD_NEXT.
 * @symbol    Null-terminated symbol name.
 *
 * @return    Address of the symbol, or NULL if not found.
 */
void *dlsym(void *handle, const char *symbol);

/*
 * dlclose — Unload a shared object.
 *
 * @handle    Handle from dlopen().
 *
 * @return    0 on success, non-zero on error (use dlerror()).
 */
int dlclose(void *handle);

/*
 * dlerror — Return a human-readable error message from the last
 *           dlopen/dlsym/dlclose call, or NULL if no error.
 *
 * The returned string is statically allocated and must not be freed.
 * Each call overwrites the previous error string.
 */
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif /* DLFCN_H */
