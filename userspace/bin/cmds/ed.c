/* ed.c — Line-oriented text editor */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAXLINES 1024
#define MAXLINE 256

static char lines[MAXLINES][MAXLINE];
static int nlines;
static int cur;

static void print_prompt(void) { printf("ed: "); }

static void list_all(void) {
    for (int i = 0; i < nlines; i++)
        printf("%d:\t%s\n", i + 1, lines[i]);
}

static void append(void) {
    char buf[MAXLINE];
    while (1) {
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0 || (n == 1 && buf[0] == '\n')) break;
        buf[n] = 0;
        char *end = buf;
        while (*end && *end != '\n') end++;
        *end = 0;
        if (strcmp(buf, ".") == 0) break;
        if (nlines < MAXLINES) {
            strcpy(lines[nlines], buf);
            nlines++;
            cur = nlines;
        }
    }
}

static void insert(void) {
    /* Insert before current line */
    char buf[MAXLINE];
    int inserted = 0;
    while (1) {
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0 || (n == 1 && buf[0] == '\n')) break;
        buf[n] = 0;
        char *end = buf;
        while (*end && *end != '\n') end++;
        *end = 0;
        if (strcmp(buf, ".") == 0) break;
        if (nlines < MAXLINES) {
            for (int i = nlines; i > cur; i--)
                strcpy(lines[i], lines[i - 1]);
            strcpy(lines[cur], buf);
            nlines++;
            cur++;
            inserted++;
        }
    }
}

int main(void) {
    char buf[MAXLINE];
    nlines = 0;
    cur = 0;
    while (1) {
        print_prompt();
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        /* Remove trailing newline */
        char *end = buf;
        while (*end && *end != '\n') end++;
        *end = 0;
        if (strcmp(buf, "q") == 0) break;
        else if (strcmp(buf, "p") == 0) list_all();
        else if (strcmp(buf, "a") == 0) append();
        else if (strcmp(buf, "i") == 0) { cur = 0; insert(); }
        else if (buf[0] == 'w' && buf[1] == ' ') {
            int fd = open(buf + 2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { printf("Error opening file\n"); continue; }
            for (int i = 0; i < nlines; i++) {
                write(fd, lines[i], strlen(lines[i]));
                write(fd, "\n", 1);
            }
            close(fd);
        } else {
            printf("?\n");
        }
    }
    return 0;
}
