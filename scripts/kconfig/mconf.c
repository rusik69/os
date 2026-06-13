/*
 * mconf.c — Minimal Kconfig parser and text-based menuconfig interface
 *
 * Parses Kconfig files (similar to Linux Kconfig format) and presents
 * a simple text-based menu interface for configuring kernel options.
 *
 * The generated configuration is written as C macros to a header file
 * (auto.conf) which can be included by the build system.
 *
 * This is a standalone tool that runs on the host system (not in-kernel).
 * It does NOT depend on any kernel headers.
 *
 * Usage:
 *   gcc -o mconf mconf.c -lreadline
 *   ./mconf [Kconfig-file]
 *
 * Item S198: menuconfig
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

/* ── Configuration options ─────────────────────────────────────────── */

#define MAX_SYMBOLS     1024
#define MAX_LINE_LEN    4096
#define MAX_DEPTH       16
#define MAX_STRING_LEN   256

/* ── Symbol types ──────────────────────────────────────────────────── */

enum sym_type {
    SYM_BOOL,
    SYM_TRISTATE,
    SYM_INT,
    SYM_STRING,
    SYM_HEX,
    SYM_UNDEFINED,
};

/* ── Symbol structure ──────────────────────────────────────────────── */

struct symbol {
    char            name[MAX_STRING_LEN];
    enum sym_type   type;
    char            value[MAX_STRING_LEN];
    char            default_val[MAX_STRING_LEN];
    char            prompt[MAX_STRING_LEN];  /* help text / prompt */
    int             is_defined;
    int             depends_on;  /* index of dependency symbol, -1 if none */
    int             is_visible;
};

/* ── Global state ──────────────────────────────────────────────────── */

static struct symbol g_symbols[MAX_SYMBOLS];
static int g_num_symbols = 0;
static const char *g_kconfig_file = "Kconfig";
static const char *g_output_file = "auto.conf";

/* ── Forward declarations ──────────────────────────────────────────── */

static void parse_kconfig(const char *filename);
static int  find_symbol(const char *name);
static int  add_symbol(const char *name, enum sym_type type,
                       const char *prompt, const char *default_val);
static void set_symbol_value(int idx, const char *value);
static void generate_config(void);
static void show_menu(void);
static int  read_line(FILE *f, char *buf, int max_len);

/* ── String helpers ────────────────────────────────────────────────── */

static char *trim_whitespace(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return s;
}

static int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ── Kconfig lexical analysis ─────────────────────────────────────── */

static void parse_kconfig_line(const char *line, int *in_help)
{
    char buf[MAX_LINE_LEN];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = trim_whitespace(buf);

    /* Skip comments and blank lines */
    if (*p == '\0' || *p == '#')
        return;

    /* Handle help text continuation */
    if (*in_help) {
        if (str_starts_with(p, "\t") || str_starts_with(p, "  ")) {
            /* Continuation of help text — append to last symbol's prompt */
            if (g_num_symbols > 0) {
                strncat(g_symbols[g_num_symbols - 1].prompt, " ",
                        MAX_STRING_LEN - strlen(g_symbols[g_num_symbols - 1].prompt) - 1);
                strncat(g_symbols[g_num_symbols - 1].prompt, trim_whitespace(p),
                        MAX_STRING_LEN - strlen(g_symbols[g_num_symbols - 1].prompt) - 1);
            }
            return;
        }
        *in_help = 0;
    }

    /* Parse keywords */
    if (str_starts_with(p, "menuconfig ") || str_starts_with(p, "config ")) {
        /* Extract the symbol name after "config " or "menuconfig " */
        const char *name_start = p;
        while (*name_start && !isspace((unsigned char)*name_start))
            name_start++;
        while (*name_start && isspace((unsigned char)*name_start))
            name_start++;

        char sym_name[MAX_STRING_LEN];
        strncpy(sym_name, name_start, MAX_STRING_LEN - 1);
        sym_name[MAX_STRING_LEN - 1] = '\0';
        /* Take only the first token (the symbol name) */
        char *space = strchr(sym_name, ' ');
        if (space) *space = '\0';
        char *newline = strchr(sym_name, '\n');
        if (newline) *newline = '\0';

        /* Add the symbol with default type BOOL */
        add_symbol(sym_name, SYM_BOOL, "", "");
        return;
    }

    if (str_starts_with(p, "bool")) {
        /* bool prompt */
        const char *prompt = p + 4;
        while (*prompt && isspace((unsigned char)*prompt)) prompt++;
        /* Strip quotes if present */
        char prompt_buf[MAX_STRING_LEN] = "";
        if (*prompt == '"') {
            prompt++;
            char *end = strchr(prompt, '"');
            if (end) {
                strncpy(prompt_buf, prompt, end - prompt);
                prompt_buf[end - prompt] = '\0';
            }
        } else {
            strncpy(prompt_buf, prompt, MAX_STRING_LEN - 1);
        }

        if (g_num_symbols > 0)
            strncpy(g_symbols[g_num_symbols - 1].prompt, prompt_buf, MAX_STRING_LEN - 1);
        return;
    }

    if (str_starts_with(p, "tristate")) {
        if (g_num_symbols > 0)
            g_symbols[g_num_symbols - 1].type = SYM_TRISTATE;
        return;
    }

    if (str_starts_with(p, "int")) {
        if (g_num_symbols > 0)
            g_symbols[g_num_symbols - 1].type = SYM_INT;
        return;
    }

    if (str_starts_with(p, "hex")) {
        if (g_num_symbols > 0)
            g_symbols[g_num_symbols - 1].type = SYM_HEX;
        return;
    }

    if (str_starts_with(p, "string")) {
        if (g_num_symbols > 0)
            g_symbols[g_num_symbols - 1].type = SYM_STRING;
        return;
    }

    if (str_starts_with(p, "default")) {
        /* Extract default value */
        const char *val = p + 7;
        while (*val && isspace((unsigned char)*val)) val++;
        if (*val == 'y' || *val == 'n' || *val == 'm' ||
            isdigit((unsigned char)*val) || *val == '"' || *val == '0') {
            if (g_num_symbols > 0) {
                strncpy(g_symbols[g_num_symbols - 1].default_val, val,
                        MAX_STRING_LEN - 1);
                /* Set as current value too */
                strncpy(g_symbols[g_num_symbols - 1].value, val,
                        MAX_STRING_LEN - 1);
            }
        }
        return;
    }

    if (str_starts_with(p, "depends on")) {
        /* Parse dependency */
        const char *dep = p + 10;
        while (*dep && isspace((unsigned char)*dep)) dep++;
        if (g_num_symbols > 0) {
            int dep_idx = find_symbol(dep);
            if (dep_idx < 0)
                dep_idx = add_symbol(dep, SYM_BOOL, "", "n");
            g_symbols[g_num_symbols - 1].depends_on = dep_idx;
        }
        return;
    }

    if (str_starts_with(p, "select ")) {
        /* 'select' forces another symbol to be y when this is y */
        /* For simplicity, we note it but don't implement auto-select */
        return;
    }

    if (str_starts_with(p, "help") || strcmp(p, "---help---") == 0) {
        *in_help = 1;
        return;
    }

    if (str_starts_with(p, "menu")) {
        /* 'menu' introduces a menu — ignore for flat parsing */
        return;
    }

    if (str_starts_with(p, "endmenu")) {
        return;
    }

    if (str_starts_with(p, "if")) {
        /* conditional block — skip for simplicity */
        return;
    }

    if (str_starts_with(p, "endif")) {
        return;
    }

    if (str_starts_with(p, "source")) {
        /* 'source' includes another Kconfig file */
        const char *inc = p + 6;
        while (*inc && isspace((unsigned char)*inc)) inc++;
        if (*inc == '"') {
            inc++;
            char inc_file[MAX_LINE_LEN];
            char *end = strchr(inc, '"');
            if (end) {
                strncpy(inc_file, inc, end - inc);
                inc_file[end - inc] = '\0';
                parse_kconfig(inc_file);
            }
        }
        return;
    }

    if (str_starts_with(p, "comment")) {
        return;
    }

    if (str_starts_with(p, "choice")) {
        return;
    }

    if (str_starts_with(p, "endchoice")) {
        return;
    }

    if (str_starts_with(p, "mainmenu_name")) {
        return;
    }

    if (str_starts_with(p, "mainmenu")) {
        return;
    }
}

static int read_line(FILE *f, char *buf, int max_len)
{
    if (!fgets(buf, max_len, f))
        return -1;
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    return len;
}

static void parse_kconfig(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Warning: cannot open '%s': %s\n",
                filename, strerror(errno));
        return;
    }

    char line[MAX_LINE_LEN];
    int in_help = 0;
    int line_num = 0;

    while (read_line(f, line, sizeof(line)) >= 0) {
        line_num++;
        parse_kconfig_line(line, &in_help);
    }

    fclose(f);
}

/* ── Symbol table management ───────────────────────────────────────── */

static int find_symbol(const char *name)
{
    for (int i = 0; i < g_num_symbols; i++) {
        if (strcmp(g_symbols[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int add_symbol(const char *name, enum sym_type type,
                      const char *prompt, const char *default_val)
{
    if (g_num_symbols >= MAX_SYMBOLS) {
        fprintf(stderr, "Too many symbols (max %d)\n", MAX_SYMBOLS);
        return -1;
    }

    int idx = find_symbol(name);
    if (idx >= 0)
        return idx;  /* already exists */

    idx = g_num_symbols++;
    strncpy(g_symbols[idx].name, name, MAX_STRING_LEN - 1);
    g_symbols[idx].type = type;
    strncpy(g_symbols[idx].prompt, prompt, MAX_STRING_LEN - 1);
    strncpy(g_symbols[idx].default_val, default_val, MAX_STRING_LEN - 1);
    strncpy(g_symbols[idx].value, default_val, MAX_STRING_LEN - 1);
    g_symbols[idx].is_defined = 1;
    g_symbols[idx].depends_on = -1;
    g_symbols[idx].is_visible = 1;

    return idx;
}

static void set_symbol_value(int idx, const char *value)
{
    if (idx < 0 || idx >= g_num_symbols)
        return;

    switch (g_symbols[idx].type) {
    case SYM_BOOL:
    case SYM_TRISTATE:
        if (strcmp(value, "y") == 0 || strcmp(value, "1") == 0 ||
            strcmp(value, "yes") == 0) {
            strcpy(g_symbols[idx].value, "y");
        } else if (strcmp(value, "m") == 0) {
            strcpy(g_symbols[idx].value, "m");
        } else {
            strcpy(g_symbols[idx].value, "n");
        }
        break;
    case SYM_INT:
    case SYM_HEX:
    case SYM_STRING:
        strncpy(g_symbols[idx].value, value, MAX_STRING_LEN - 1);
        break;
    default:
        break;
    }
}

/* ── Configuration generation ──────────────────────────────────────── */

static void generate_config(void)
{
    FILE *f = fopen(g_output_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s': %s\n",
                g_output_file, strerror(errno));
        return;
    }

    fprintf(f, "#\n");
    fprintf(f, "# Auto-generated config — do not edit.\n");
    fprintf(f, "# Generated by mconf (Kconfig parser)\n");
    fprintf(f, "#\n\n");

    for (int i = 0; i < g_num_symbols; i++) {
        /* Skip symbols whose dependencies are not met */
        if (g_symbols[i].depends_on >= 0) {
            int dep = g_symbols[i].depends_on;
            if (strcmp(g_symbols[dep].value, "y") != 0)
                continue;
        }

        const char *name = g_symbols[i].name;
        const char *val  = g_symbols[i].value;

        /* Write comment with prompt if available */
        if (g_symbols[i].prompt[0]) {
            fprintf(f, "# %s\n", g_symbols[i].prompt);
        }

        switch (g_symbols[i].type) {
        case SYM_BOOL:
            if (strcmp(val, "y") == 0)
                fprintf(f, "CONFIG_%s=y\n\n", name);
            else
                fprintf(f, "# CONFIG_%s is not set\n\n", name);
            break;
        case SYM_TRISTATE:
            if (strcmp(val, "y") == 0)
                fprintf(f, "CONFIG_%s=y\n\n", name);
            else if (strcmp(val, "m") == 0)
                fprintf(f, "CONFIG_%s=m\n\n", name);
            else
                fprintf(f, "# CONFIG_%s is not set\n\n", name);
            break;
        case SYM_INT:
        case SYM_HEX:
        case SYM_STRING:
            fprintf(f, "CONFIG_%s=\"%s\"\n\n", name, val);
            break;
        default:
            break;
        }
    }

    fclose(f);
    printf("Configuration written to '%s'\n", g_output_file);
}

/* ── Text-based menu interface ─────────────────────────────────────── */

static void show_menu(void)
{
    int running = 1;

    while (running) {
        printf("\n");
        printf("=====================================\n");
        printf(" Kernel Configuration (mconf)\n");
        printf("=====================================\n");
        printf("\n");

        /* Display all symbols with current values */
        printf("%-4s %-30s %-10s %s\n", "  #", "Symbol", "Value", "Prompt");
        printf("%-4s %-30s %-10s %s\n", "----", "------", "-----", "------");

        int display_count = 0;
        for (int i = 0; i < g_num_symbols; i++) {
            /* Skip symbols without prompts and dependencies not met */
            if (!g_symbols[i].prompt[0] && !g_symbols[i].is_defined)
                continue;

            display_count++;
            const char *val = g_symbols[i].value;
            const char *disp_val = val;
            char val_buf[16];

            switch (g_symbols[i].type) {
            case SYM_BOOL:
            case SYM_TRISTATE:
                if (strcmp(val, "y") == 0) disp_val = "[*]";
                else if (strcmp(val, "m") == 0) disp_val = "[M]";
                else disp_val = "[ ]";
                break;
            default:
                snprintf(val_buf, sizeof(val_buf), "\"%s\"", val);
                disp_val = val_buf;
                break;
            }

            /* Show dependency status */
            char dep_mark = ' ';
            if (g_symbols[i].depends_on >= 0) {
                int dep = g_symbols[i].depends_on;
                if (strcmp(g_symbols[dep].value, "y") != 0)
                    dep_mark = '-';  /* hidden due to dependency */
            }

            printf(" %c%c %-30s %-10s %s\n",
                   dep_mark, display_count > 9 ? ' ' : ' ',
                   g_symbols[i].name,
                   disp_val,
                   g_symbols[i].prompt);
        }

        printf("\n");
        printf("Commands:\n");
        printf("  <num>=<val>  Set symbol <num> to value (y/n/m/string)\n");
        printf("  q            Save and quit\n");
        printf("  Q            Quit without saving\n");
        printf("  ?<num>       Show info for symbol <num>\n");
        printf("  help         Show this help\n");
        printf("\n> ");

        char input[MAX_LINE_LEN];
        if (!fgets(input, sizeof(input), stdin))
            break;

        /* Strip newline */
        char *nl = strchr(input, '\n');
        if (nl) *nl = '\0';

        char *cmd = trim_whitespace(input);

        if (strcmp(cmd, "q") == 0) {
            generate_config();
            running = 0;
        } else if (strcmp(cmd, "Q") == 0) {
            printf("Quit without saving.\n");
            running = 0;
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            /* already showing help */
        } else if (cmd[0] == '?') {
            /* Show info for symbol */
            int num = atoi(cmd + 1);
            if (num >= 1 && num <= g_num_symbols) {
                int idx = num - 1;
                printf("\n--- Symbol: %s ---\n", g_symbols[idx].name);
                printf("  Type:    %s\n",
                       g_symbols[idx].type == SYM_BOOL ? "bool" :
                       g_symbols[idx].type == SYM_TRISTATE ? "tristate" :
                       g_symbols[idx].type == SYM_INT ? "int" :
                       g_symbols[idx].type == SYM_HEX ? "hex" :
                       g_symbols[idx].type == SYM_STRING ? "string" : "?");
                printf("  Prompt:  %s\n", g_symbols[idx].prompt);
                printf("  Value:   %s\n", g_symbols[idx].value);
                printf("  Default: %s\n", g_symbols[idx].default_val);
                if (g_symbols[idx].depends_on >= 0)
                    printf("  Depends: %s\n",
                           g_symbols[g_symbols[idx].depends_on].name);
            } else {
                printf("Invalid symbol number: %d\n", num);
            }
        } else if (strchr(cmd, '=') != NULL) {
            /* Set a symbol value: <num>=<val> */
            char *eq = strchr(cmd, '=');
            *eq = '\0';
            int num = atoi(cmd);
            const char *val = trim_whitespace(eq + 1);

            if (num >= 1 && num <= g_num_symbols) {
                int idx = num - 1;

                switch (g_symbols[idx].type) {
                case SYM_BOOL:
                    if (strcmp(val, "y") == 0 || strcmp(val, "1") == 0 ||
                        strcmp(val, "yes") == 0 || strcmp(val, "Y") == 0)
                        set_symbol_value(idx, "y");
                    else if (strcmp(val, "n") == 0 || strcmp(val, "0") == 0 ||
                             strcmp(val, "no") == 0 || strcmp(val, "N") == 0)
                        set_symbol_value(idx, "n");
                    else
                        printf("Invalid value for bool: '%s' (use y/n)\n", val);
                    break;
                case SYM_TRISTATE:
                    if (strcmp(val, "y") == 0 || strcmp(val, "1") == 0)
                        set_symbol_value(idx, "y");
                    else if (strcmp(val, "m") == 0)
                        set_symbol_value(idx, "m");
                    else if (strcmp(val, "n") == 0 || strcmp(val, "0") == 0)
                        set_symbol_value(idx, "n");
                    else
                        printf("Invalid value for tristate: '%s' (use y/m/n)\n", val);
                    break;
                case SYM_INT: {
                    /* Validate integer */
                    int valid = 1;
                    for (const char *c = val; *c; c++)
                        if (!isdigit((unsigned char)*c) && *c != '-') {
                            valid = 0;
                            break;
                        }
                    if (valid)
                        set_symbol_value(idx, val);
                    else
                        printf("Invalid integer: '%s'\n", val);
                    break;
                }
                case SYM_HEX: {
                    /* Validate hex */
                    int valid = 1;
                    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
                        val += 2;
                    for (const char *c = val; *c; c++)
                        if (!isxdigit((unsigned char)*c)) {
                            valid = 0;
                            break;
                        }
                    if (valid)
                        set_symbol_value(idx, val);
                    else
                        printf("Invalid hex: '%s'\n", val);
                    break;
                }
                case SYM_STRING:
                    set_symbol_value(idx, val);
                    break;
                default:
                    printf("Unknown symbol type\n");
                    break;
                }
            } else {
                printf("Invalid symbol number: %d\n", num);
            }
        }
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    printf("mconf — Kernel Configuration Menu\n");
    printf("==================================\n\n");

    /* Parse command line */
    if (argc >= 2)
        g_kconfig_file = argv[1];
    if (argc >= 3)
        g_output_file = argv[2];

    /* Parse the Kconfig file */
    printf("Reading '%s'...\n", g_kconfig_file);
    parse_kconfig(g_kconfig_file);
    printf("Found %d config symbols\n\n", g_num_symbols);

    /* Show interactive menu (or just generate if stdin not interactive) */
    if (isatty(STDIN_FILENO)) {
        show_menu();
    } else {
        printf("Non-interactive mode, using defaults.\n");
        generate_config();
    }

    return 0;
}
