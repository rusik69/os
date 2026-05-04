#ifndef SHELL_CMDS_H
#define SHELL_CMDS_H

/* Original commands (shell_cmds.c) */
void cmd_help(void);
void cmd_echo(const char *args);
void cmd_meminfo(void);
void cmd_ps(void);
void cmd_uptime(void);
void cmd_reboot(void);
void cmd_shutdown(void);
void cmd_kill(const char *args);
void cmd_color(const char *args);
void cmd_hexdump(const char *args);
void cmd_date(void);
void cmd_cpuinfo(void);
void cmd_ls(const char *args);
void cmd_cat(const char *args);
void cmd_write(const char *args);
void cmd_touch(const char *args);
void cmd_rm(const char *args);
void cmd_mkdir(const char *args);
void cmd_stat_file(const char *args);
void cmd_format_disk(void);
void cmd_history_show(void);
void cmd_ifconfig(void);
void cmd_ping(const char *args);
void cmd_dns(const char *args);
void cmd_curl(const char *args);
void cmd_beep(const char *args);
void cmd_play(const char *args);
void cmd_mouse_status(void);
void cmd_udpsend(const char *args);
void cmd_exec(const char *args);
void cmd_run(const char *args);

/* Tool commands (shell_tools.c) */
void cmd_wc(const char *args);
void cmd_head(const char *args);
void cmd_tail(const char *args);
void cmd_cp(const char *args);
void cmd_mv(const char *args);
void cmd_grep(const char *args);
void cmd_df(void);
void cmd_free(void);
void cmd_whoami(void);
void cmd_hostname(void);
void cmd_env(void);
void cmd_xxd(const char *args);
void cmd_sleep(const char *args);
void cmd_seq(const char *args);
void cmd_arp(void);
void cmd_route(void);
void cmd_uname(void);
void cmd_lspci(void);
void cmd_dmesg(void);
void cmd_cc(const char *args);
void cmd_sort(const char *args);
void cmd_find(const char *args);
void cmd_calc(const char *args);
void cmd_uniq(const char *args);
void cmd_tr(const char *args);
void cmd_tmux(const char *args);

#endif
