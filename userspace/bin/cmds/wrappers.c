/* wrappers.c — display wrapper/alias information */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(void){
    printf("Available wrapper commands:\n");
    printf("  arch       -> uname -m\n");
    printf("  lzcat      -> xz --decompress --stdout\n");
    printf("  bzless     -> bzip2 -dc | less\n");
    printf("  zless      -> gunzip -c | less\n");
    printf("  zmore      -> gunzip -c | more\n");
    printf("  zgrep      -> gunzip -c | grep\n");
    printf("  zegrep     -> gunzip -c | grep -E\n");
    printf("  zfgrep     -> gunzip -c | grep -F\n");
    printf("  zcmp       -> compare gzipped files\n");
    printf("  zdiff      -> diff gzipped files\n");
    printf("  zcat       -> gunzip -c\n");
    printf("  zforce     -> force .gz extension\n");
    printf("  zipcloak   -> toggle zip encryption\n");
    printf("  zipnote    -> read/write zip comment\n");
    printf("  zipsplit   -> split zip archive\n");
    printf("Note: wrappers are thin stubs; use the actual commands directly.\n");
    return 0;
}
