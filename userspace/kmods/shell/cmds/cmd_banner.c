/* cmd_banner.c — print large text banner */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* ASCII banner font: 5 rows per character. Order: A-Z, then 0-9. */
static const char font[36][5][6] = {
    /* A */    {"  A  ", " A A ", "AAAAA", "A   A", "A   A"},
    /* B */    {"BBBB ", "B   B", "BBBB ", "B   B", "BBBB "},
    /* C */    {" CCC ", "C   C", "C    ", "C   C", " CCC "},
    /* D */    {"DDDD ", "D   D", "D   D", "D   D", "DDDD "},
    /* E */    {"EEEEE", "E    ", "EEE  ", "E    ", "EEEEE"},
    /* F */    {"FFFFF", "F    ", "FFF  ", "F    ", "F    "},
    /* G */    {" GGG ", "G   G", "G   G", "G  GG", " GGG "},
    /* H */    {"H   H", "H   H", "HHHHH", "H   H", "H   H"},
    /* I */    {" III ", "  I  ", "  I  ", "  I  ", " III "},
    /* J */    {"  JJJ", "   J ", "   J ", "J  J ", " JJ  "},
    /* K */    {"K   K", "K  K ", "KKK  ", "K  K ", "K   K"},
    /* L */    {"L    ", "L    ", "L    ", "L    ", "LLLLL"},
    /* M */    {"M   M", "MM MM", "M M M", "M   M", "M   M"},
    /* N */    {"N   N", "NN  N", "N N N", "N  NN", "N   N"},
    /* O */    {" OOO ", "O   O", "O   O", "O   O", " OOO "},
    /* P */    {"PPPP ", "P   P", "PPPP ", "P    ", "P    "},
    /* Q */    {" QQQ ", "Q   Q", "Q   Q", "Q  QQ", " QQQQ"},
    /* R */    {"RRRR ", "R   R", "RRRR ", "R  R ", "R   R"},
    /* S */    {" SSS ", "S   S", " SS  ", "  S S", "SSS  "},
    /* T */    {"TTTTT", "  T  ", "  T  ", "  T  ", "  T  "},
    /* U */    {"U   U", "U   U", "U   U", "U   U", " UUU "},
    /* V */    {"V   V", "V   V", "V   V", " V V ", "  V  "},
    /* W */    {"W   W", "W   W", "W W W", "WW WW", "W   W"},
    /* X */    {"X   X", " X X ", "  X  ", " X X ", "X   X"},
    /* Y */    {"Y   Y", " Y Y ", "  Y  ", "  Y  ", "  Y  "},
    /* Z */    {"ZZZZZ", "   Z ", "  Z  ", " Z   ", "ZZZZZ"},
    /* 0 */    {" 000 ", "0   0", "0   0", "0   0", " 000 "},
    /* 1 */    {"  1  ", " 11  ", "  1  ", "  1  ", " 111 "},
    /* 2 */    {" 222 ", "2   2", "  2  ", " 2   ", "22222"},
    /* 3 */    {" 333 ", "3   3", "  33 ", "3   3", " 333 "},
    /* 4 */    {"4  4 ", "4  4 ", "44444", "   4 ", "   4 "},
    /* 5 */    {"55555", "5    ", "5555 ", "    5", "5555 "},
    /* 6 */    {" 666 ", "6    ", "6666 ", "6   6", " 666 "},
    /* 7 */    {"77777", "   7 ", "  7  ", " 7   ", "7    "},
    /* 8 */    {" 888 ", "8   8", " 888 ", "8   8", " 888 "},
    /* 9 */    {" 999 ", "9   9", " 999 ", "   9 ", " 999 "},
};

static int font_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return c - '0' + 26;
    return -1;
}

void cmd_banner(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: banner <text>\n");
        return;
    }
    for (int row = 0; row < 5; row++) {
        const char *p = args;
        while (*p) {
            if (*p == ' ') { kprintf("     "); p++; continue; }
            int idx = font_index(*p);
            if (idx >= 0) kprintf("%s ", font[idx][row]);
            else kprintf("?    ");
            p++;
        }
        kprintf("\n");
    }
}
