/* gui.c — GUI is a kernel-level service */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    printf("GUI is a kernel-level service. Run 'gui' from the kernel shell. "
           "Or use the kernel shell 'gui_shell' command.\n");
    return 0;
}
