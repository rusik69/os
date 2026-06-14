/* sh.c — BusyBox-style single-binary userspace shell
 *
 * Prints prompt "sh$ " and reads input line by line.
 * Supports built-in commands and external command execution via PATH.
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdarg.h"

/* ── Shell config ─────────────────────────────────────────────── */
#define MAX_LINE   1024
#define MAX_ARGS   64
#define MAX_ENV    64
#define PATH_MAX   256

/* ── Environment ──────────────────────────────────────────────── */
static char *sh_env[MAX_ENV];
static int   sh_env_count;

static void sh_init_env(void) {
    sh_env[0] = "PATH=/bin";
    sh_env[1] = "HOME=/";
    sh_env[2] = "SHELL=/bin/sh";
    sh_env[3] = 0;
    sh_env_count = 4;
}

static char *sh_getenv(const char *name) {
    unsigned long nlen = strlen(name);
    for (int i = 0; i < sh_env_count && sh_env[i]; i++) {
        if (strncmp(sh_env[i], name, nlen) == 0 && sh_env[i][nlen] == '=')
            return sh_env[i] + nlen + 1;
    }
    return 0;
}

static int sh_setenv(const char *var, const char *val) {
    unsigned long vlen = strlen(var);
    /* Check if already exists */
    for (int i = 0; i < sh_env_count && sh_env[i]; i++) {
        if (strncmp(sh_env[i], var, vlen) == 0 && sh_env[i][vlen] == '=') {
            /* Replace */
            char *new_entry = malloc(vlen + 1 + strlen(val) + 1);
            if (!new_entry) return -1;
            char *p = new_entry;
            while (*var) *p++ = *var++;
            *p++ = '=';
            while (*val) *p++ = *val++;
            *p = '\0';
            sh_env[i] = new_entry;
            return 0;
        }
    }
    /* Add new */
    if (sh_env_count >= MAX_ENV - 1) return -1;
    char *new_entry = malloc(vlen + 1 + strlen(val) + 1);
    if (!new_entry) return -1;
    char *p = new_entry;
    while (*var) *p++ = *var++;
    *p++ = '=';
    while (*val) *p++ = *val++;
    *p = '\0';
    sh_env[sh_env_count++] = new_entry;
    sh_env[sh_env_count] = 0;
    return 0;
}

/* ── Line input ───────────────────────────────────────────────── */
int sh_getline(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c;
        int n = read(0, &c, 1);
        if (n <= 0) break;
        if (c == '\n') {
            buf[i] = '\0';
            write(1, "\n", 1);
            return i;
        }
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        buf[i++] = c;
        write(1, &c, 1);  /* echo */
    }
    buf[i] = '\0';
    return i;
}

/* ── Command parser ───────────────────────────────────────────── */
int sh_parse(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    argv[argc] = 0;
    return argc;
}

/* ── External command execution ───────────────────────────────── */
extern char **environ;  /* provided by crt0, but we use our own sh_env */

int sh_exec_ext(char **argv) {
    char full[PATH_MAX];
    int pid;

    /* Try direct path first */
    if (argv[0][0] == '/' || argv[0][0] == '.') {
        pid = fork();
        if (pid == 0) {
            execve(argv[0], argv, sh_env);
            printf("sh: %s: not found\n", argv[0]);
            exit(127);
        }
        return pid;
    }

    /* Search PATH */
    char *path = sh_getenv("PATH");
    if (!path) path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, ':');
        if (next) *next++ = '\0';

        /* Build full path */
        unsigned long dlen = strlen(dir);
        unsigned long nlen = strlen(argv[0]);
        if (dlen + 1 + nlen >= PATH_MAX) { dir = next; continue; }
        unsigned long pos = 0;
        while (dir[pos]) { full[pos] = dir[pos]; pos++; }
        full[pos++] = '/';
        unsigned long j = 0;
        while (argv[0][j]) { full[pos++] = argv[0][j]; j++; }
        full[pos] = '\0';

        pid = fork();
        if (pid == 0) {
            execve(full, argv, sh_env);
            /* If exec returns, it failed — try next dir */
            exit(127);  /* will be caught by parent wait loop if needed */
        }
        if (pid > 0) return pid;  /* success, returned child pid */

        dir = next;
    }
    return -1;
}

/* ── Built-in: which ──────────────────────────────────────────── */
static int cmd_which(char **argv) {
    if (!argv[1]) {
        printf("usage: which <command>\n");
        return 1;
    }
    const char *name = argv[1];

    /* Check built-ins */
    static const char *builtins[] = {
        "cd", "pwd", "exit", "help", "echo", "clear",
        "exec", "export", "which", "ps", 0
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) {
            printf("%s: shell built-in\n", name);
            return 0;
        }
    }

    /* Search PATH */
    char *path = sh_getenv("PATH");
    if (!path) path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, ':');
        if (next) *next++ = '\0';

        char full[PATH_MAX];
        unsigned long pos = 0;
        while (dir[pos]) { full[pos] = dir[pos]; pos++; }
        full[pos++] = '/';
        unsigned long j = 0;
        while (name[j]) { full[pos++] = name[j]; j++; }
        full[pos] = '\0';

        struct stat st;
        if (stat(full, &st) == 0) {
            printf("%s\n", full);
            return 0;
        }
        dir = next;
    }
    printf("which: %s: not found\n", name);
    return 1;
}

/* ── Built-in: ps (read /proc entries) ────────────────────────── */
static int cmd_ps(void) {
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) {
        printf("ps: cannot open /proc\n");
        return 1;
    }
    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);
    if (n <= 0) {
        printf("ps: no entries\n");
        return 1;
    }
    printf("PID   NAME\n");
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        /* Only show numeric entries (processes) */
        int is_num = 1;
        char *p = d->d_name;
        while (*p) { if (*p < '0' || *p > '9') { is_num = 0; break; } p++; }
        if (is_num) {
            /* Try to read the process name from /proc/<pid>/cmdline or /proc/<pid>/stat */
            char procpath[64];
            snprintf(procpath, 64, "/proc/%s/cmdline", d->d_name);
            int pfd = open(procpath, O_RDONLY, 0);
            if (pfd >= 0) {
                char cmdline[256];
                int r = read(pfd, cmdline, 255);
                close(pfd);
                cmdline[r] = '\0';
                /* Replace null bytes with spaces */
                for (int i = 0; i < r; i++) if (cmdline[i] == '\0') cmdline[i] = ' ';
                printf("%-5s %s\n", d->d_name, cmdline);
            } else {
                printf("%-5s\n", d->d_name);
            }
        }
        pos += d->d_reclen;
    }
    return 0;
}

/* ── Built-in: help ───────────────────────────────────────────── */
static int cmd_help(void) {
    printf("BusyBox-style shell built-in commands:\n");
    printf("  cd <path>        — Change directory\n");
    printf("  pwd              — Print working directory\n");
    printf("  exit [code]      — Exit shell\n");
    printf("  help             — Show this help\n");
    printf("  echo <args...>   — Print arguments\n");
    printf("  clear            — Clear screen\n");
    printf("  exec <path> [args...] — Replace shell with command\n");
    printf("  export VAR=VALUE — Set environment variable\n");
    printf("  which <cmd>      — Show path to command\n");
    printf("  ps               — List processes\n");
    printf("  For unknown commands, fork+execve in PATH (/bin:/usr/bin)\n");
    return 0;
}

/* ── Built-in dispatch ────────────────────────────────────────── */
static int run_builtin(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, "exit") == 0) {
        int code = (argc > 1) ? atoi(argv[1]) : 0;
        exit(code);
        return 0; /* not reached */
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *path = argc > 1 ? argv[1] : sh_getenv("HOME");
        if (!path) path = "/";
        if (chdir(path) < 0) {
            printf("cd: %s: No such directory\n", argv[1] ? argv[1] : "");
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "pwd") == 0) {
        char buf[PATH_MAX];
        if (getcwd(buf, PATH_MAX) == 0) {
            printf("pwd: error\n");
            return 1;
        }
        printf("%s\n", buf);
        return 0;
    }

    if (strcmp(cmd, "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) write(1, " ", 1);
            write(1, argv[i], strlen(argv[i]));
        }
        write(1, "\n", 1);
        return 0;
    }

    if (strcmp(cmd, "clear") == 0) {
        write(1, "\033[2J\033[H", 7);
        return 0;
    }

    if (strcmp(cmd, "help") == 0) {
        return cmd_help();
    }

    if (strcmp(cmd, "exec") == 0) {
        if (argc < 2) {
            printf("exec: missing argument\n");
            return 1;
        }
        execve(argv[1], argv + 1, sh_env);
        printf("exec: %s: not found\n", argv[1]);
        return 1;
    }

    if (strcmp(cmd, "export") == 0) {
        if (argc < 2) {
            printf("export: usage: export VAR=VALUE\n");
            return 1;
        }
        char *eq = strchr(argv[1], '=');
        if (!eq) {
            printf("export: missing '=' in '%s'\n", argv[1]);
            return 1;
        }
        *eq = '\0';
        sh_setenv(argv[1], eq + 1);
        *eq = '=';
        return 0;
    }

    if (strcmp(cmd, "which") == 0) {
        return cmd_which(argv);
    }

    if (strcmp(cmd, "ps") == 0) {
        return cmd_ps();
    }

    /* Not a built-in */
    return -1;
}

/* ── Main ─────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc;

    sh_init_env();

    /* If we have a command as argument, run it non-interactively */
    if (argc >= 2) {
        /* Run argv[1] with argv[1..] as arguments */
        char *cmd_argv[MAX_ARGS];
        int cmd_argc = argc - 1;
        for (int i = 0; i < cmd_argc && i < MAX_ARGS - 1; i++)
            cmd_argv[i] = argv[i + 1];
        cmd_argv[cmd_argc] = 0;

        int r = run_builtin(cmd_argc, cmd_argv);
        if (r >= 0) return r;

        /* External command */
        int pid = sh_exec_ext(cmd_argv);
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            return status;
        }
        printf("sh: %s: not found\n", cmd_argv[0]);
        return 127;
    }

    /* Interactive shell */
    char line[MAX_LINE];

    while (1) {
        write(1, "sh$ ", 4);
        int n = sh_getline(line, MAX_LINE);
        if (n <= 0) {
            write(1, "\n", 1);
            break;
        }

        char *argv_buf[MAX_ARGS];
        int ac = sh_parse(line, argv_buf, MAX_ARGS);
        if (ac == 0) continue;

        /* Check built-ins */
        int r = run_builtin(ac, argv_buf);
        if (r >= 0) continue;

        /* External command */
        int pid = sh_exec_ext(argv_buf);
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            printf("sh: %s: not found\n", argv_buf[0]);
        }
    }

    return 0;
}
