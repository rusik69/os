#define KERNEL_INTERNAL
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "container.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

/*
 * cmd_compose.c — compose shell command (Item C200)
 *
 * Multi-container application management (Docker Compose-style).
 *
 * Usage: compose up <file.json>       — Start a compose stack
 *        compose down <name>          — Stop and remove a compose stack
 *        compose ps <name>            — List services in a compose stack
 *        compose logs <name>          — Show logs from a compose stack
 */

/* ── Forward declarations (from src/orch/compose.c) ────────────────── */
extern int compose_parse(const char *json_string);
extern int compose_up(const char *app_name);
extern int compose_down(const char *app_name);
extern int compose_ps(const char *app_name);
extern int compose_init(void);

/* ── Main dispatch ─────────────────────────────────────────────────── */
int cmd_compose(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: compose <subcommand> [args...]\n\n");
        kprintf("Docker Compose-style multi-container application manager\n\n");
        kprintf("Subcommands:\n");
        kprintf("  up <file.json>   Create and start a compose application\n");
        kprintf("  down <name>      Stop and remove a compose application\n");
        kprintf("  ps <name>        List services in a compose application\n");
        kprintf("  logs <name>      Show logs from a compose application\n");
        return 0;
    }

    if (strcmp(argv[1], "up") == 0) {
        if (argc < 3) {
            kprintf("Usage: compose up <file.json>\n");
            return 0;
        }
        compose_init();
        int ret = compose_parse(argv[2]);  /* reads + parses the JSON file */
        if (ret < 0) {
            kprintf("compose: failed to parse '%s' (error %d)\n", argv[2], ret);
            return 0;
        }
        ret = compose_up(argv[2]);
        if (ret < 0) {
            kprintf("compose: compose_up failed: %d\n", ret);
            return 0;
        }
        kprintf("Compose application '%s' started\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "down") == 0) {
        if (argc < 3) {
            kprintf("Usage: compose down <name>\n");
            return 0;
        }
        int ret = compose_down(argv[2]);
        if (ret < 0) {
            kprintf("compose: compose_down failed: %d\n", ret);
            return 0;
        }
        kprintf("Compose application '%s' stopped and removed\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "ps") == 0) {
        if (argc < 3) {
            kprintf("Usage: compose ps <name>\n");
            return 0;
        }
        int ret = compose_ps(argv[2]);
        if (ret < 0)
            kprintf("compose: compose_ps failed: %d\n", ret);
        return 0;
    }

    if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) {
            kprintf("Usage: compose logs <name>\n");
            return 0;
        }
        kprintf("--- Logs for compose app '%s' ---\n", argv[2]);
        kprintf("(compose log viewer — not yet fully implemented)\n");
        return 0;
    }

    kprintf("compose: unknown subcommand '%s'. Try 'compose' for help.\n", argv[1]);
    return 0;
}
