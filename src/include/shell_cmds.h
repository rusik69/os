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
void cmd_dmesg(const char *args);
void cmd_cc(const char *args);
void cmd_ccbuilder(const char *args);
void cmd_ld(const char *args);
void cmd_sort(const char *args);
void cmd_find(const char *args);
void cmd_calc(const char *args);
void cmd_uniq(const char *args);
void cmd_tr(const char *args);
void cmd_tmux(const char *args);
void cmd_jobs(void);
void cmd_fg(const char *args);
void cmd_bg(const char *args);
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

/* GUI commands */
void cmd_gui(void);
void cmd_doom(const char *args);

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
void cmd_capprof(const char *args);

/* Service management */
void cmd_service(const char *args);

/* New text processing utilities */
void cmd_fold(const char *args);
void cmd_expand(const char *args);
void cmd_comm(const char *args);
void cmd_split(const char *args);

/* New system utilities */
void cmd_top(void);
void cmd_sed(const char *args);
void cmd_tar(const char *args);
void cmd_which(const char *args);
void cmd_ln(const char *args);
void cmd_true(const char *args);
void cmd_false(const char *args);
void cmd_more(const char *args);
void cmd_file(const char *args);
void cmd_nslookup(const char *args);

/* Phase-3 new commands */
void cmd_nc(const char *args);
void cmd_wget(const char *args);
void cmd_watch(const char *args);
void cmd_sha256sum(const char *args);

/* Phase-6 new commands */
void cmd_alias(const char *args);
void cmd_unalias(const char *args);
void cmd_readlink(const char *args);
void cmd_cd(const char *args);
void cmd_pwd(void);
void cmd_nice(const char *args);
void cmd_renice(const char *args);
void cmd_awk(const char *args);
void cmd_netstat(const char *args);
void cmd_trap(const char *args);
void cmd_rawsend(const char *args);

/* Exit status API */
void shell_set_exit_status(int s);
int  shell_get_exit_status(void);

/* Shell stdin (pipe input) API */
void shell_set_stdin(const char *buf, int len);
void shell_clear_stdin(void);
int  shell_has_stdin(void);
int  shell_stdin_read(char *buf, int max);
void cmd_dosbox(const char *args);

/* Phase 7 new commands */
void cmd_shuf(const char *args);
void cmd_logname(const char *args);
void cmd_who(const char *args);
void cmd_banner(const char *args);
void cmd_tsort(const char *args);
void cmd_join(const char *args);
void cmd_sum(const char *args);
void cmd_sync(void);
void cmd_unexpand(const char *args);
void cmd_bt(void);

/* Phase 8 — new shell commands */
void cmd_cmp(const char *args);
void cmd_dirname(const char *args);
void cmd_groups(const char *args);
void cmd_link(const char *args);
void cmd_mknod(const char *args);
void cmd_nohup(const char *args);
void cmd_printenv(const char *args);
void cmd_realpath(const char *args);
void cmd_rmdir(const char *args);
void cmd_shred(const char *args);
void cmd_truncate(const char *args);
void cmd_tty(const char *args);
void cmd_unlink(const char *args);
void cmd_chgrp(const char *args);
void cmd_hostid(const char *args);
void cmd_arch(const char *args);
void cmd_factor(const char *args);
void cmd_fmt(const char *args);
void cmd_mktemp(const char *args);
void cmd_pathchk(const char *args);
void cmd_size(const char *args);
void cmd_chrt(const char *args);
void cmd_taskset(const char *args);
void cmd_clear(const char *args);

/* Phase 9 — new shell commands */
void cmd_dd(const char *args);
void cmd_cksum(const char *args);
void cmd_mkfifo(const char *args);
void cmd_logger(const char *args);
void cmd_mesg(const char *args);
void cmd_nproc(const char *args);
void cmd_pinky(const char *args);
void cmd_tset(const char *args);
void cmd_reset(const char *args);

/* Phase 10 — new compression/network/system commands */
void cmd_chroot(const char *args);
void cmd_pax(const char *args);
void cmd_uuencode(const char *args);
void cmd_uudecode(const char *args);
void cmd_unzip(const char *args);
void cmd_gzip(const char *args);
void cmd_gunzip(const char *args);
void cmd_bunzip2(const char *args);
void cmd_bzcat(const char *args);
void cmd_xz(const char *args);
void cmd_unxz(const char *args);
void cmd_lzma(const char *args);
void cmd_unlzma(const char *args);
void cmd_zcat(const char *args);
void cmd_telnet(const char *args);
void cmd_ftp(const char *args);
void cmd_ip(const char *args);
void cmd_ss(const char *args);
void cmd_iostat(const char *args);
void cmd_vmstat(const char *args);
void cmd_lscpu(const char *args);
void cmd_lshw(const char *args);
void cmd_blkid(const char *args);

/* Phase 11 — new shell commands */
void cmd_404(const char *args);
void cmd_zcmp(const char *args);
void cmd_zdiff(const char *args);
void cmd_zegrep(const char *args);
void cmd_zfgrep(const char *args);
void cmd_zforce(const char *args);
void cmd_zgrep(const char *args);
void cmd_zip(const char *args);
void cmd_zipcloak(const char *args);
void cmd_zipnote(const char *args);
void cmd_zipsplit(const char *args);
void cmd_less(const char *args);
void cmd_ed(const char *args);
void cmd_patch(const char *args);
void cmd_pr(const char *args);
void cmd_look(const char *args);
void cmd_locale(const char *args);
void cmd_localedef(const char *args);
void cmd_iconv(const char *args);
void cmd_script(const char *args);
void cmd_mcookie(const char *args);
void cmd_shar(const char *args);

#endif
