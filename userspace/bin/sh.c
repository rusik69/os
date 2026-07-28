/* sh.c — BusyBox-style single-binary userspace shell
 *
 * Prints prompt "sh$ " and reads input line by line.
 * Supports built-in commands and external command execution via PATH.
 */

#include "stdarg.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* Exit status decoding macros (Linux-compatible encoding) */
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) ((((s) & 0x7f) != 0) && (((s) & 0x7f) < 0x7f))
#define WTERMSIG(s) ((s) & 0x7f)

/* ── Shell config ─────────────────────────────────────────────── */
#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_ENV 64
#define PATH_MAX 256

/* ── Environment ──────────────────────────────────────────────── */
static char *sh_env[MAX_ENV];
static int sh_env_count;
static int last_exit_code; /* $? — exit status of last command */

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
            if (!new_entry)
                return -1;
            char *p = new_entry;
            while (*var)
                *p++ = *var++;
            *p++ = '=';
            while (*val)
                *p++ = *val++;
            *p = '\0';
            sh_env[i] = new_entry;
            return 0;
        }
    }
    /* Add new */
    if (sh_env_count >= MAX_ENV - 1)
        return -1;
    char *new_entry = malloc(vlen + 1 + strlen(val) + 1);
    if (!new_entry)
        return -1;
    char *p = new_entry;
    while (*var)
        *p++ = *var++;
    *p++ = '=';
    while (*val)
        *p++ = *val++;
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
        if (n <= 0)
            break;
        if (c == '\n') {
            buf[i] = '\0';
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
    }
    buf[i] = '\0';
    return i;
}

/* ── Command parser ───────────────────────────────────────────── */
int sh_parse(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (*p == '\0')
            break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    argv[argc] = 0;
    return argc;
}

/* ── Variable expansion (currently just $?) ──────────────────── */
static void sh_expand_line(char *line, int max) {
    char tmp[MAX_LINE];
    int i = 0, j = 0;
    while (line[i] && j < max - 1) {
        if (line[i] == '$' && line[i + 1] == '?') {
            /* Convert last_exit_code to string */
            char buf[16];
            int k = 0;
            int code = last_exit_code;
            if (code == 0) {
                buf[k++] = '0';
            } else {
                char rev[16];
                int r = 0;
                while (code > 0) {
                    rev[r++] = (char)('0' + (code % 10));
                    code /= 10;
                }
                while (r > 0)
                    buf[k++] = rev[--r];
            }
            buf[k] = '\0';
            int n = 0;
            while (buf[n] && j < max - 1)
                tmp[j++] = buf[n++];
            i += 2;
        } else {
            tmp[j++] = line[i++];
        }
    }
    tmp[j] = '\0';
    strncpy(line, tmp, (unsigned long)max - 1);
    line[max - 1] = '\0';
}

/* ── External command execution ───────────────────────────────── */
extern char **environ; /* provided by crt0, but we use our own sh_env */

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
    if (!path)
        path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, ':');
        if (next)
            *next++ = '\0';

        /* Build full path */
        unsigned long dlen = strlen(dir);
        unsigned long nlen = strlen(argv[0]);
        if (dlen + 1 + nlen >= PATH_MAX) {
            dir = next;
            continue;
        }
        unsigned long pos = 0;
        while (dir[pos]) {
            full[pos] = dir[pos];
            pos++;
        }
        full[pos++] = '/';
        unsigned long j = 0;
        while (argv[0][j]) {
            full[pos++] = argv[0][j];
            j++;
        }
        full[pos] = '\0';

        pid = fork();
        if (pid == 0) {
            execve(full, argv, sh_env);
            /* If exec returns, it failed — try next dir */
            exit(127); /* will be caught by parent wait loop if needed */
        }
        if (pid > 0)
            return pid; /* success, returned child pid */

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
    static const char *builtins[] = {"cd",     "pwd",   "exit", "help", "echo",   "clear", "exec",
                                     "export", "which", "ps",   "free", "uptime", "uname", 0};
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) {
            printf("%s: shell built-in\n", name);
            return 0;
        }
    }

    /* Search PATH */
    char *path = sh_getenv("PATH");
    if (!path)
        path = "/bin";
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    path_copy[PATH_MAX - 1] = '\0';

    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, ':');
        if (next)
            *next++ = '\0';

        char full[PATH_MAX];
        unsigned long pos = 0;
        while (dir[pos]) {
            full[pos] = dir[pos];
            pos++;
        }
        full[pos++] = '/';
        unsigned long j = 0;
        while (name[j]) {
            full[pos++] = name[j];
            j++;
        }
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
    printf("  PID NAME\n");
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        int is_num = 1;
        char *p = d->d_name;
        while (*p) {
            if (*p < '0' || *p > '9') {
                is_num = 0;
                break;
            }
            p++;
        }
        if (is_num) {
            char procpath[64];
            snprintf(procpath, 64, "/proc/%s/cmdline", d->d_name);
            int pfd = open(procpath, O_RDONLY, 0);
            if (pfd >= 0) {
                char cmdline[256];
                int r = read(pfd, cmdline, 255);
                close(pfd);
                if (r > 0) {
                    cmdline[r] = '\0';
                    for (int i = 0; i < r; i++)
                        if (cmdline[i] == '\0')
                            cmdline[i] = ' ';
                    printf("%5s %s\n", d->d_name, cmdline);
                } else {
                    printf("%5s\n", d->d_name);
                }
            } else {
                printf("%5s\n", d->d_name);
            }
        }
        pos += d->d_reclen;
    }
    return 0;
}

/* ── Built-in: free (read /proc/meminfo) ──────────────────────── */
static int cmd_free(void) {
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) {
        printf("free: cannot open /proc/meminfo\n");
        return 1;
    }
    char buf[1024];
    int r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r > 0) {
        buf[r] = '\0';
        printf("%s", buf);
    }
    return 0;
}

/* ── Built-in: uptime ─────────────────────────────────────────── */
static int cmd_uptime(void) {
    int fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd < 0) {
        printf("uptime: cannot open /proc/uptime\n");
        return 1;
    }
    char buf[128];
    int r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r > 0) {
        buf[r] = '\0';
        printf("%s", buf);
    }
    return 0;
}

/* ── Built-in: uname ──────────────────────────────────────────── */
static int cmd_uname(int argc, char **argv) {
    struct utsname u;
    if (uname(&u) < 0) {
        printf("uname: failed\n");
        return 1;
    }
    if (argc < 2 || argv[1][0] != '-') {
        printf("%s\n", u.sysname);
        return 0;
    }
    int show_all = 0;
    const char *a = argv[1];
    for (int i = 1; a[i]; i++) {
        if (a[i] == 'a')
            show_all = 1;
    }
    if (show_all) {
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    } else {
        for (int i = 1; a[i]; i++) {
            switch (a[i]) {
            case 's':
                printf("%s ", u.sysname);
                break;
            case 'n':
                printf("%s ", u.nodename);
                break;
            case 'r':
                printf("%s ", u.release);
                break;
            case 'v':
                printf("%s ", u.version);
                break;
            case 'm':
                printf("%s ", u.machine);
                break;
            }
        }
        printf("\n");
    }
    return 0;
}

/* ── Built-in: help ───────────────────────────────────────────── */
static int cmd_help(void) {
    printf("Shell built-in commands:\n");
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
    printf("  free             — Show memory usage\n");
    printf("  uptime           — Show system uptime\n");
    printf("  uname [-snrvma]  — Show system information\n");
    printf("  For unknown commands, fork+execve in PATH (/bin)\n");
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
        if (!path)
            path = "/";
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
            if (i > 1)
                write(1, " ", 1);
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

    if (strcmp(cmd, "free") == 0) {
        return cmd_free();
    }

    if (strcmp(cmd, "uptime") == 0) {
        return cmd_uptime();
    }

    if (strcmp(cmd, "uname") == 0) {
        return cmd_uname(argc, argv);
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
        if (r >= 0)
            return r;

        /* External command */
        int pid = sh_exec_ext(cmd_argv);
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return 0;
        }
        printf("sh: %s: not found\n", cmd_argv[0]);
        return 127;
    }

    /* Interactive shell */
    char line[MAX_LINE];

    while (1) {
        write(1, "\nsh$ ", 5);
        int n = sh_getline(line, MAX_LINE);
        if (n <= 0) {
            write(1, "\n", 1);
            break;
        }

        /* Expand $? before parsing */
        sh_expand_line(line, MAX_LINE);

        char *argv_buf[MAX_ARGS];
        int ac = sh_parse(line, argv_buf, MAX_ARGS);
        if (ac == 0)
            continue;

        /* Check built-ins */
        int r = run_builtin(ac, argv_buf);
        if (r >= 0) {
            last_exit_code = r;
            continue;
        }

        /* External command */
        int pid = sh_exec_ext(argv_buf);
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status))
                last_exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                last_exit_code = 128 + WTERMSIG(status);
            else
                last_exit_code = 0;
        } else {
            printf("sh: %s: not found\n", argv_buf[0]);
            last_exit_code = 127;
        }
    }

    return last_exit_code;
}
