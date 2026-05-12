/* cmd_which.c — check if a shell command is known */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* Keep in sync with shell.c dispatch */
static const char *known_cmds[] = {
    "help","echo","clear","meminfo","ps","uptime","reboot","shutdown",
    "kill","color","hexdump","date","cpuinfo","history","ls","cat",
    "write","touch","rm","mkdir","stat","format","edit","exec","run",
    "ifconfig","ping","dns","curl","udpsend","beep","play","mouse",
    "wc","head","tail","cp","mv","grep","df","free","whoami","hostname",
    "env","xxd","sleep","seq","arp","route","uname","lspci","dmesg",
    "cc","ccbuilder","sort","find","calc","uniq","tr","tmux","jobs",
    "fg","wait","tee","cut","paste","basename","dirname","yes","rev",
    "nl","du","id","diff","md5sum","od","expr","test","[","xargs",
    "printf","time","strings","tac","base64","cmos","hwinfo","fbinfo",
    "gui","serial","lsusb","lsblk","fat","chmod","chown","login",
    "logout","useradd","userdel","passwd","users","capprof","service",
    "fold","expand","comm","split","which","ln","true","false","more",
    "file","nslookup","exit",
    (const char *)0
};

void cmd_which(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: which <command>\n"); return; }

    char name[64];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) { name[i] = args[i]; i++; }
    name[i] = '\0';

    for (int k = 0; known_cmds[k]; k++) {
        if (strcmp(known_cmds[k], name) == 0) {
            kprintf("%s: shell built-in\n", name);
            return;
        }
    }
    kprintf("%s: not found\n", name);
}
