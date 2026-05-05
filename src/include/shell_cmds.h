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
void cmd_jobs(void);
void cmd_fg(const char *args);
void cmd_wait(const char *args);

/* Additional tool commands */
void cmd_tee(const char *args);
void cmd_cut(const char *args);
void cmd_paste(const char *args);
void cmd_basename(const char *args);
void cmd_dirname(const char *args);
void cmd_yes(const char *args);
void cmd_rev(const char *args);
void cmd_nl(const char *args);
void cmd_du(const char *args);
void cmd_id(const char *args);
void cmd_diff(const char *args);
void cmd_md5sum(const char *args);
void cmd_od(const char *args);
void cmd_expr(const char *args);
void cmd_test(const char *args);
void cmd_xargs(const char *args);

/* New utility commands */
void cmd_printf(const char *args);
void cmd_time(const char *args);
void cmd_strings(const char *args);
void cmd_tac(const char *args);
void cmd_base64(const char *args);

/* Hardware commands */
void cmd_cmos(void);
void cmd_hwinfo(void);
void cmd_fbinfo(void);
void cmd_serial(const char *args);

/* New hardware & FS commands */
void cmd_lsusb(void);
void cmd_lsblk(void);
void cmd_fat(const char *args);
void cmd_chmod(const char *args);
void cmd_chown(const char *args);

/* Multiuser commands */
void cmd_login(const char *args);
void cmd_logout(void);
void cmd_useradd(const char *args);
void cmd_userdel(const char *args);
void cmd_passwd(const char *args);
void cmd_users(void);

#endif
