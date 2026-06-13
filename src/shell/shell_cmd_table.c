/* shell_cmd_table.c — shared command table + dispatch */

#include "shell_cmd_table.h"
#include "shell_cmds.h"
#include "vga.h"
#include "editor.h"
#include "string.h"

static void sh_help(const char *a)       { (void)a; cmd_help(); }
static void __attribute__((unused)) sh_clear(const char *a)      { (void)a; vga_clear(); }
static void sh_meminfo(const char *a)    { (void)a; cmd_meminfo(); }
static void sh_ps(const char *a)         { (void)a; cmd_ps(); }
static void sh_uptime(const char *a)     { (void)a; cmd_uptime(); }
static void sh_reboot(const char *a)     { (void)a; cmd_reboot(); }
static void sh_shutdown(const char *a)   { cmd_shutdown(a); }
static void sh_date(const char *a)       { (void)a; cmd_date(); }
static void sh_cpuinfo(const char *a)    { (void)a; cmd_cpuinfo(); }
static void sh_history(const char *a)  { (void)a; cmd_history_show(); }
static void sh_format(const char *a)     { (void)a; cmd_format_disk(); }
static void sh_ifconfig(const char *a)   { (void)a; cmd_ifconfig(); }
static void sh_df(const char *a)         { (void)a; cmd_df(); }
static void sh_free(const char *a)       { (void)a; cmd_free(); }
static void sh_whoami(const char *a)     { (void)a; cmd_whoami(); }
static void sh_hostname(const char *a)   { cmd_hostname(a); }
static void sh_env(const char *a)        { (void)a; cmd_env(); }
static void sh_arp(const char *a)        { (void)a; cmd_arp(); }
static void sh_route(const char *a)      { (void)a; cmd_route(); }
static void sh_uname(const char *a)      { (void)a; cmd_uname(); }
static void sh_lspci(const char *a)      { (void)a; cmd_lspci(); }
static void sh_dmesg(const char *a)      { cmd_dmesg(a); }
static void sh_dmsetup(const char *a)    { cmd_dmsetup(a); }
static void sh_jobs(const char *a)       { (void)a; cmd_jobs(); }
static void sh_cmos(const char *a)       { (void)a; cmd_cmos(); }
static void sh_hwinfo(const char *a)     { (void)a; cmd_hwinfo(); }
static void sh_fbinfo(const char *a)     { (void)a; cmd_fbinfo(); }
static void sh_gui(const char *a)        { (void)a; cmd_gui(); }
static void sh_lsusb(const char *a)      { (void)a; cmd_lsusb(); }
static void sh_lsblk(const char *a)      { (void)a; cmd_lsblk(); }
static void sh_logout(const char *a)     { (void)a; cmd_logout(); }
static void sh_users(const char *a)      { (void)a; cmd_users(); }
static void sh_su(const char *a)         { cmd_su(a); }
static void sh_top(const char *a)        { (void)a; cmd_top(); }
static void sh_pwd(const char *a)        { (void)a; cmd_pwd(); }
static void sh_mouse(const char *a)      { (void)a; cmd_mouse_status(); }
static void sh_sync(const char *a)       { (void)a; cmd_sync(); }
static void sh_bt(const char *a)         { (void)a; cmd_bt(); }
static void sh_iommu(const char *a)      { (void)a; cmd_iommu(); }
static void sh_cet(const char *a)        { (void)a; cmd_cet(); }
static void sh_nft(const char *a)        { cmd_nft(a); }
static void sh_true(const char *a)      { (void)a; cmd_true(); }
static void sh_false(const char *a)     { (void)a; cmd_false(); }
static void __attribute__((unused)) sh_logname(const char *a)   { (void)a; cmd_logname(); }
static void __attribute__((unused)) sh_groups(const char *a)    { (void)a; cmd_groups(); }
static void __attribute__((unused)) sh_hostid(const char *a)    { (void)a; cmd_hostid(); }
static void __attribute__((unused)) sh_tty(const char *a)       { (void)a; cmd_tty(); }
static void __attribute__((unused)) sh_mesg(const char *a)      { (void)a; cmd_mesg(); }
static void __attribute__((unused)) sh_nproc(const char *a)     { (void)a; cmd_nproc(); }
static void __attribute__((unused)) sh_pinky(const char *a)     { (void)a; cmd_pinky(); }
static void __attribute__((unused)) sh_pwdx(const char *a)      { (void)a; cmd_pwdx(); }
static void __attribute__((unused)) sh_export(const char *a)    { cmd_export(a); }
static void __attribute__((unused)) sh_unset(const char *a)     { cmd_unset(a); }
static void sh_sysctl(const char *a)    { cmd_sysctl(a); }
static void sh_bc(const char *a)        { cmd_bc(a); }
static void sh_insmod(const char *a)    { cmd_insmod(a); }
static void sh_rmmod(const char *a)     { cmd_rmmod(a); }
static void sh_lsmod(const char *a)     { (void)a; cmd_lsmod(); }
static void sh_modinfo(const char *a)   { cmd_modinfo(a); }
static void sh_modprobe(const char *a)  { cmd_modprobe(a); }
static void sh_mdev(const char *a)      { cmd_mdev(a); }

static void sh_ntpdate(const char *a)   { cmd_ntpdate(a); }
static void sh_tftpd(const char *a)     { cmd_tftpd(a); }

#include "cmd_table.inc"

int shell_cmd_count(void) {
    int n = 0;
    while (shell_cmd_table[n].name) n++;
    return n;
}

const shell_cmd_entry_t *shell_cmd_entry(int idx) {
    if (idx < 0 || idx >= shell_cmd_count()) return 0;
    return &shell_cmd_table[idx];
}

const char *shell_cmd_lookup_desc(const char *name) {
    for (int i = 0; shell_cmd_table[i].name; i++) {
        if (strcmp(shell_cmd_table[i].name, name) == 0)
            return shell_cmd_table[i].desc;
    }
    return 0;
}

shell_cmd_fn shell_cmd_lookup_fn(const char *name) {
    for (int i = 0; shell_cmd_table[i].name; i++) {
        if (strcmp(shell_cmd_table[i].name, name) == 0)
            return shell_cmd_table[i].fn;
    }
    return 0;
}

int shell_cmd_exists(const char *name) {
    if (!name || !*name) return 0;
    if (strcmp(name, "[") == 0) return 1;
    return shell_cmd_lookup_fn(name) != 0;
}
