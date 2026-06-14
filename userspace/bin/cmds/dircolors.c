/* dircolors.c — set color for ls: print default LS_COLORS */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *colors = "LS_COLORS='rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:do=01;35:bd=40;33;01:cd=40;33;01:or=40;31;01:mi=00:su=37;41:sg=30;43:ca=30;41:tw=30;42:ow=34;42:st=37;44:ex=01;32:*.tar=01;31:*.tgz=01;31:*.gz=01;31:*.bz2=01;31:*.xz=01;31:*.zip=01;31:*.7z=01;31:*.png=01;35:*.jpg=01;35:*.jpeg=01;35:*.gif=01;35:*.mp3=01;36:*.wav=01;36:*.mp4=01;36:'\n";
    write(1, colors, strlen(colors));
    return 0;
}
