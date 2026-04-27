#ifndef SCRIPT_H
#define SCRIPT_H

#include "types.h"

/* Execute a script file from the VFS path.
 * Lines are executed one at a time via shell_exec_cmd().
 * Returns 0 on success, -1 on error. */
int script_exec(const char *path);

#endif
