/* cmd_mcookie.c — generate a random cookie string */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

/* Simple LCG PRNG seeded from uptime ticks */
static uint64_t mcookie_seed = 0;

static uint8_t mcookie_rand(void) {
    if (mcookie_seed == 0)
        mcookie_seed = libc_uptime_ticks() ^ 0xDEADBEEF;
    mcookie_seed = mcookie_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint8_t)(mcookie_seed >> 32);
}

void cmd_mcookie(const char *args) {
    (void)args;
    uint8_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = mcookie_rand();
    kprintf("%02x%02x%02x%02x%02x%02x%02x%02x"
            "%02x%02x%02x%02x%02x%02x%02x%02x\n",
            (unsigned int)buf[0], (unsigned int)buf[1],
            (unsigned int)buf[2], (unsigned int)buf[3],
            (unsigned int)buf[4], (unsigned int)buf[5],
            (unsigned int)buf[6], (unsigned int)buf[7],
            (unsigned int)buf[8], (unsigned int)buf[9],
            (unsigned int)buf[10], (unsigned int)buf[11],
            (unsigned int)buf[12], (unsigned int)buf[13],
            (unsigned int)buf[14], (unsigned int)buf[15]);
}
