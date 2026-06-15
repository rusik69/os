/* ncdu.c — simple disk usage analyzer using getdents64 + stat */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

#define INITIAL_CAP 1024

struct entry {
    char name[384];
    unsigned long long size;
};

static struct entry *entries;
static int entry_count;
static int entry_cap;

static void add_entry(const char *name, unsigned long long size) {
    if (entry_count >= entry_cap) {
        int new_cap = entry_cap ? entry_cap * 2 : INITIAL_CAP;
        struct entry *tmp = realloc(entries, (unsigned long)new_cap * sizeof(struct entry));
        if (!tmp) {
            write(2, "ncdu: out of memory\n", 20);
            exit(1);
        }
        entries = tmp;
        entry_cap = new_cap;
    }
    strncpy(entries[entry_count].name, name, sizeof(entries[entry_count].name) - 1);
    entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
    entries[entry_count].size = size;
    entry_count++;
}

/* Insertion sort descending by size */
static void sort_entries(void) {
    int i, j;
    struct entry key;
    for (i = 1; i < entry_count; i++) {
        key = entries[i];
        j = i - 1;
        while (j >= 0 && entries[j].size < key.size) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static void format_size(unsigned long long bytes, char *buf, unsigned long bufsz) {
    static const char *units[] = {"B", "K", "M", "G", "T"};
    int unit = 0;
    double val = (double)bytes;
    while (val >= 1024.0 && unit < 4) {
        val /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, bufsz, "%llu%s", bytes, units[unit]);
    else
        snprintf(buf, bufsz, "%.1f%s", val, units[unit]);
}

static unsigned long long scan_dir(const char *dirpath) {
    unsigned long long total = 0;

    int fd = open(dirpath, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    char dent_buf[4096];
    int n;

    while ((n = getdents64(fd, dent_buf, sizeof(dent_buf))) > 0) {
        int pos = 0;
        while (pos < n) {
            struct dirent *d = (struct dirent *)(dent_buf + pos);

            /* Skip . and .. */
            if (d->d_name[0] == '.') {
                if (d->d_name[1] == '\0') {
                    pos += d->d_reclen;
                    continue;
                }
                if (d->d_name[1] == '.' && d->d_name[2] == '\0') {
                    pos += d->d_reclen;
                    continue;
                }
            }

            /* Build the full path for stat */
            char fullpath[512];
            int plen = strlen(dirpath);
            if ((unsigned long)plen + 1 + strlen(d->d_name) >= sizeof(fullpath)) {
                pos += d->d_reclen;
                continue;
            }
            memcpy(fullpath, dirpath, plen);
            if (dirpath[plen - 1] != '/') {
                fullpath[plen] = '/';
                plen++;
            }
            strcpy(fullpath + plen, d->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    unsigned long long subtotal = scan_dir(fullpath);
                    add_entry(fullpath, subtotal);
                    total += subtotal;
                } else if (S_ISREG(st.st_mode)) {
                    add_entry(fullpath, st.st_size);
                    total += st.st_size;
                }
            }

            pos += d->d_reclen;
        }
    }

    close(fd);
    return total;
}

int main(int argc, char *argv[]) {
    const char *target = ".";
    if (argc > 1)
        target = argv[1];

    entries = NULL;
    entry_count = 0;
    entry_cap = 0;

    unsigned long long total = 0;
    struct stat st;
    if (stat(target, &st) != 0) {
        printf("ncdu: cannot access '%s'\n", target);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        total = scan_dir(target);
    } else if (S_ISREG(st.st_mode)) {
        add_entry(target, st.st_size);
        total = st.st_size;
    } else {
        printf("ncdu: '%s' is not a file or directory\n", target);
        return 1;
    }

    sort_entries();

    char sizebuf[32];
    for (int i = 0; i < entry_count; i++) {
        format_size(entries[i].size, sizebuf, sizeof(sizebuf));
        printf("%7s  %s\n", sizebuf, entries[i].name);
    }

    format_size(total, sizebuf, sizeof(sizebuf));
    printf("--------\n");
    printf("Total: %s\n", sizebuf);

    free(entries);
    return 0;
}
