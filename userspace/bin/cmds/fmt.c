/* fmt.c — simple word wrap formatter */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_WORDS 65536
#define MAX_WORD_LEN 256

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    char buf[4096];
    char words[MAX_WORDS][MAX_WORD_LEN];
    int nwords = 0;
    char word[MAX_WORD_LEN];
    int wpos = 0;
    int nread;
    while ((nread = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n') {
                if (wpos > 0) {
                    word[wpos] = '\0';
                    if (nwords < MAX_WORDS) {
                        int j = 0;
                        while (word[j]) { words[nwords][j] = word[j]; j++; }
                        words[nwords][j] = '\0';
                        nwords++;
                    }
                    wpos = 0;
                }
                if (c == '\n' && wpos == 0) {
                    /* Force paragraph break */
                    if (nwords < MAX_WORDS) {
                        words[nwords][0] = '\0';
                        nwords++;
                    }
                }
            } else {
                if (wpos < MAX_WORD_LEN - 1)
                    word[wpos++] = c;
            }
        }
    }
    if (wpos > 0) {
        word[wpos] = '\0';
        if (nwords < MAX_WORDS) {
            int j = 0;
            while (word[j]) { words[nwords][j] = word[j]; j++; }
            words[nwords][j] = '\0';
            nwords++;
        }
    }
    /* Output with wrapping at 75 chars */
    int col = 0;
    for (int i = 0; i < nwords; i++) {
        if (words[i][0] == '\0') {
            if (col > 0) write(STDOUT_FILENO, "\n", 1);
            write(STDOUT_FILENO, "\n", 1);
            col = 0;
        } else {
            int wlen = 0;
            while (words[i][wlen]) wlen++;
            if (col > 0) {
                if (col + 1 + wlen > 75) {
                    write(STDOUT_FILENO, "\n", 1);
                    col = 0;
                } else {
                    write(STDOUT_FILENO, " ", 1);
                    col++;
                }
            }
            write(STDOUT_FILENO, words[i], wlen);
            col += wlen;
        }
    }
    if (col > 0) write(STDOUT_FILENO, "\n", 1);
    return 0;
}
