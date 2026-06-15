/* pulse.c — sound/pulseaudio stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "Sound managed by kernel AC97 driver. Use 'mixer' or 'beep' kernel shell commands.\n";
    write(1, msg, strlen(msg));
    return 0;
}
