#ifndef EDITOR_H
#define EDITOR_H

void editor_open(const char *filename);

/* Called by telnetd to feed raw input characters when editor is active */
void editor_feed_char(char c);

/* Returns 1 if the editor is currently running in telnet mode */
int editor_is_active(void);

#endif
