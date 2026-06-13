#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* cmd_nft — nftables rule listing and management */
void cmd_nft(const char *args) {
    if (!args) args = "";

    if (strcmp(args, "list ruleset") == 0 || strcmp(args, "list") == 0 || *args == '\0') {
        kprintf("nftables ruleset:\n");
        kprintf("  table inet filter {\n");

        extern void nf_print_rules(void);
        nf_print_rules();

        kprintf("  }\n");
    } else if (strncmp(args, "add rule", 8) == 0) {
        const char *rule = args + 8;
        while (*rule == ' ') rule++;
        kprintf("Adding nftables rule: %s\n", rule);

        extern int nf_add_rule(void *rule);
        int ret = nf_add_rule((void*)rule);
        if (ret == 0) {
            kprintf("Rule added successfully\n");
        } else {
            kprintf("Failed to add rule: err=%d\n", ret);
        }
    } else if (strncmp(args, "flush ruleset", 13) == 0) {
        kprintf("Flushing nftables ruleset\n");
        extern void nf_flush_rules(void);
        nf_flush_rules();
        kprintf("Ruleset flushed\n");
    } else {
        kprintf("Usage: nft [list ruleset|add rule <rule>|flush ruleset]\n");
    }
}
