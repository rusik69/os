#ifndef SNDSTAT_H
#define SNDSTAT_H

/**
 * sndstat_generate — Generate /proc/asound summary content.
 * @buf:  Output buffer.
 * @size: Maximum buffer size.
 *
 * Returns the number of bytes written to buf, or -1 on error.
 */
int sndstat_generate(char *buf, int size);

#endif /* SNDSTAT_H */
