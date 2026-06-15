/* fatattr.c — read and display FAT filesystem attributes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* FAT attribute bits */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

static void show_attributes(unsigned char attr) {
    printf("  Read-only:  %c\n", (attr & FAT_ATTR_READ_ONLY) ? 'Y' : 'N');
    printf("  Hidden:     %c\n", (attr & FAT_ATTR_HIDDEN) ? 'Y' : 'N');
    printf("  System:     %c\n", (attr & FAT_ATTR_SYSTEM) ? 'Y' : 'N');
    printf("  Volume ID:  %c\n", (attr & FAT_ATTR_VOLUME_ID) ? 'Y' : 'N');
    printf("  Directory:  %c\n", (attr & FAT_ATTR_DIRECTORY) ? 'Y' : 'N');
    printf("  Archive:    %c\n", (attr & FAT_ATTR_ARCHIVE) ? 'Y' : 'N');

    char buf[8];
    int pos = 0;
    if (attr & FAT_ATTR_READ_ONLY) buf[pos++] = 'r';
    else buf[pos++] = '-';
    if (attr & FAT_ATTR_HIDDEN) buf[pos++] = 'h';
    else buf[pos++] = '-';
    if (attr & FAT_ATTR_SYSTEM) buf[pos++] = 's';
    else buf[pos++] = '-';
    if (attr & FAT_ATTR_DIRECTORY) buf[pos++] = 'd';
    else buf[pos++] = '-';
    if (attr & FAT_ATTR_ARCHIVE) buf[pos++] = 'a';
    else buf[pos++] = '-';
    buf[pos] = '\0';
    printf("  Attributes: %s\n", buf);
}

int main(int argc, char *argv[]) {
    const char *file = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "+r") == 0) continue;
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "+h") == 0) continue;
            if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "+s") == 0) continue;
            if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "+a") == 0) continue;
        } else {
            file = argv[i];
        }
    }

    if (!file) {
        printf("Usage: fatattr [options] <file>\n");
        printf("  -r  set read-only attribute\n");
        printf("  -h  set hidden attribute\n");
        printf("  -s  set system attribute\n");
        printf("  -a  set archive attribute\n");
        printf("  To read attributes, just specify the file.\n");
        return 1;
    }

    printf("FAT attributes for %s:\n", file);

    /* Try to read FAT attributes via stat */
    struct stat st;
    if (stat(file, &st) == 0) {
        unsigned char detected = 0;
        /* Map Unix permissions to FAT attrs */
        if (!(st.st_mode & 0222)) detected |= FAT_ATTR_READ_ONLY;
        if (st.st_mode & 0100000) detected |= FAT_ATTR_ARCHIVE;

        printf("  (from Unix permissions - limited accuracy)\n");
        show_attributes(detected);
        printf("\n  Note: For accurate FAT attributes, the file must be\n");
        printf("  on a mounted FAT filesystem with kernel FAT support.\n");
        return 0;
    }

    printf("  Cannot stat file '%s'\n", file);
    return 1;
}
