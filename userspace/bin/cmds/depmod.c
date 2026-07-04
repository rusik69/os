/* depmod.c — generate module dependency files (D234 tasks 12-14)
 *
 * Scans /modules/*.ko files and generates:
 *   - modules.dep:     dependency graph (module.ko: dep1.ko dep2.ko)
 *   - modules.alias:   alias patterns (alias pci:v... module_name)
 *   - modules.symbols: exported symbol list (symbol module_name)
 *
 * Usage: depmod
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MODULES_DIR "/modules/"

/* Search for a key=value in the modinfo section of a .ko file.
 * Returns 1 if found, 0 if not.  Value is placed in @out (up to @outsz). */
static int modinfo_lookup(const char *filebuf, int filesz,
                           const char *key, char *out, int outsz)
{
    int keylen = (int)strlen(key);
    const char *p = filebuf;
    int remaining = filesz;

    while (remaining > 0) {
        const char *found = strstr(p, key);
        if (!found || found - filebuf >= filesz) break;

        found += keylen;
        int outpos = 0;
        while (outpos < outsz - 1 && found < filebuf + filesz && *found && *found != '\n') {
            out[outpos++] = *found;
            found++;
        }
        out[outpos] = '\0';
        if (outpos > 0)
            return 1;
        break;
    }
    out[0] = '\0';
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Open /modules directory */
    int dirfd = open(MODULES_DIR, 0, 0);  /* O_RDONLY */
    if (dirfd < 0) {
        printf("depmod: cannot open %s\n", MODULES_DIR);
        return 1;
    }

    /* Create output files */
    int depfd = open("/modules/modules.dep", 0x202, 0644);   /* O_WRONLY|O_CREAT|O_TRUNC */
    if (depfd < 0) {
        printf("depmod: cannot create /modules/modules.dep\n");
        close(dirfd);
        return 1;
    }

    int aliasfd = open("/modules/modules.alias", 0x202, 0644);
    int symfd = open("/modules/modules.symbols", 0x202, 0644);

    /* Read directory entries */
    char readbuf[8192];
    int n;
    while ((n = getdents64(dirfd, readbuf, sizeof(readbuf))) > 0) {
        struct dirent *d;
        int pos = 0;
        while (pos < n) {
            d = (struct dirent *)(readbuf + pos);
            pos += d->d_reclen;

            /* Skip . and .. */
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                continue;

            /* Check if it's a .ko file */
            unsigned long len = strlen(d->d_name);
            if (len < 4 || strcmp(d->d_name + len - 3, ".ko") != 0)
                continue;

            /* Read entire .ko file into memory */
            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "%s%s", MODULES_DIR, d->d_name);

            int fd = open(fullpath, 0, 0);
            if (fd < 0) continue;
            char filebuf[65536];
            int fsz = read(fd, filebuf, sizeof(filebuf));
            close(fd);
            if (fsz <= 0) continue;

            /* ── modules.dep: dependencies ────────────────────────── */
            char deps[512];
            modinfo_lookup(filebuf, fsz, "depends=", deps, sizeof(deps));

            write(depfd, d->d_name, strlen(d->d_name));
            write(depfd, ": ", 2);
            if (deps[0]) {
                /* Convert comma-separated module names to .ko suffixes */
                char *tok = deps;
                int first = 1;
                while (*tok) {
                    char *comma = strchr(tok, ',');
                    if (comma) *comma = '\0';
                    if (!first) write(depfd, " ", 1);
                    write(depfd, tok, strlen(tok));
                    write(depfd, ".ko", 3);
                    first = 0;
                    if (comma) {
                        *comma = ',';
                        tok = comma + 1;
                    } else {
                        break;
                    }
                }
            }
            write(depfd, "\n", 1);

            /* ── modules.alias: alias patterns ────────────────────── */
            if (aliasfd >= 0) {
                /* Search for all alias= entries in modinfo */
                const char *alias_tag = "alias=";
                const char *p_modinfo = filebuf;
                int remaining = fsz;

                while (remaining > 0) {
                    const char *found = strstr(p_modinfo, alias_tag);
                    if (!found || found - filebuf >= fsz) break;

                    found += 6; /* skip "alias=" */
                    char alias_buf[256];
                    int opos = 0;
                    while (opos < 255 && found < filebuf + fsz && *found && *found != '\n') {
                        alias_buf[opos++] = *found;
                        found++;
                    }
                    alias_buf[opos] = '\0';

                    if (opos > 0) {
                        char modname[64];
                        /* Strip .ko suffix if present */
                        unsigned long nlen = strlen(d->d_name);
                        if (nlen > 3 && strcmp(d->d_name + nlen - 3, ".ko") == 0)
                            nlen -= 3;
                        memcpy(modname, d->d_name, nlen);
                        modname[nlen] = '\0';

                        write(aliasfd, "alias ", 6);
                        write(aliasfd, alias_buf, strlen(alias_buf));
                        write(aliasfd, " ", 1);
                        write(aliasfd, modname, strlen(modname));
                        write(aliasfd, "\n", 1);
                    }

                    /* Skip past this null-terminated entry */
                    while (remaining > 0 && p_modinfo <= found)
                        { p_modinfo++; remaining--; }
                }
            }

            /* ── modules.symbols: exported symbols ────────────────── */
            if (symfd >= 0) {
                char sym_name[256];
                if (modinfo_lookup(filebuf, fsz, "name=", sym_name, sizeof(sym_name))) {
                    /* Write: symbol module_name — placeholder for now */
                    /* In a full implementation, we'd parse the ELF symbol
                     * table and find EXPORT_SYMBOL entries.  For now,
                     * just record that the module is available. */
                    write(symfd, "# symbols for ", 14);
                    write(symfd, sym_name, strlen(sym_name));
                    write(symfd, "\n", 1);
                }
            }

            close(fd);
        }
    }

    close(dirfd);
    close(depfd);
    if (aliasfd >= 0) close(aliasfd);
    if (symfd >= 0) close(symfd);

    printf("depmod: generated /modules/modules.dep");
    if (aliasfd >= 0) printf(", modules.alias");
    if (symfd >= 0) printf(", modules.symbols");
    printf("\n");

    return 0;
}
