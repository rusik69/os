#ifndef SHELL_CMDS_H
#define SHELL_CMDS_H

/* Original commands (shell_cmds.c) */
void cmd_help(void);
void cmd_echo(const char *args);
void cmd_meminfo(void);
void cmd_ps(void);
void cmd_uptime(void);
void cmd_reboot(void);
void cmd_shutdown(const char *args);
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
void cmd_rmmod(const char *args);
void cmd_minesweeper(const char *args);
void cmd_snake(const char *args);
void cmd_connect4(const char *args);
void cmd_tetris(const char *args);
void cmd_mkdir(const char *args);
void cmd_stat_file(const char *args);
void cmd_format_disk(void);
void cmd_history_show(void);
void cmd_ifconfig(void);
void cmd_ping(const char *args);
void cmd_dns(const char *args);
void cmd_dmsetup(const char *args);
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
void cmd_hostname(const char *args);
void cmd_env(void);
void cmd_export(const char *args);
void cmd_unset(const char *args);
void cmd_sysctl(const char *args);
void cmd_xxd(const char *args);
void cmd_sleep(const char *args);
int cmd_seq(int argc, char **argv);
int cmd_cal(int argc, char **argv);
void cmd_arp(void);
void cmd_route(void);
void cmd_uname(void);
void cmd_lspci(void);
void cmd_dmesg(const char *args);
void cmd_ulimit(const char *args);
void cmd_cc(const char *args);
void cmd_ccbuilder(const char *args);
void cmd_ld(const char *args);
void cmd_sort(const char *args);
void cmd_find(const char *args);
void cmd_calc(const char *args);
void cmd_bc(const char *args);
void cmd_uniq(const char *args);
void cmd_tr(const char *args);
void cmd_tmux(const char *args);
void cmd_jobs(void);
void cmd_fg(const char *args);
void cmd_bg(const char *args);
void cmd_wait(const char *args);

/* Additional tool commands */
int cmd_tee(int argc, char **argv);
void cmd_cut(const char *args);
void cmd_paste(const char *args);
void cmd_basename(const char *args);
void cmd_dirname(const char *args);
int cmd_yes(int argc, char **argv);
void cmd_rev(const char *args);
void cmd_nl(const char *args);
void cmd_du(const char *args);
void cmd_id(const char *args);
void cmd_diff(const char *args);
void cmd_ncdu(const char *args);
void cmd_nvme(const char *args);
void cmd_fm(const char *args);
void cmd_mc(const char *args);
void cmd_md5sum(const char *args);
void cmd_od(const char *args);
void cmd_expr(const char *args);
void cmd_test(const char *args);
void cmd_blkdiscard(const char *args);
void cmd_xargs(const char *args);

/* New utility commands */
void cmd_printf(const char *args);
void cmd_time(const char *args);
void cmd_strings(const char *args);
void cmd_tac(const char *args);
void cmd_base64(const char *args);
/* Wrappers for commands with incompatible function signatures */
void cmd_comm_wrapper(const char *args);
void cmd_expand_wrapper(const char *args);
void cmd_fold_wrapper(const char *args);
void cmd_seq_wrapper(const char *args);
void cmd_tee_wrapper(const char *args);
void cmd_yes_wrapper(const char *args);
void cmd_tsort_wrapper(const char *args);
void cmd_join_wrapper(const char *args);
void cmd_unexpand_wrapper(const char *args);
void cmd_fmt_wrapper(const char *args);
void cmd_pr_wrapper(const char *args);
void cmd_base32_wrapper(const char *args);

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
void cmd_su(const char *args);
void cmd_useradd(const char *args);
void cmd_userdel(const char *args);
void cmd_passwd(const char *args);
void cmd_users(void);
void cmd_capprof(const char *args);

/* Service management */
void cmd_service(const char *args);

/* Init runlevel switching (U4) */
void cmd_init(const char *args);

/* New text processing utilities */
int cmd_fold(int argc, char **argv);
int cmd_expand(int argc, char **argv);
int cmd_comm(int argc, char **argv);
void cmd_split(const char *args);

/* New system utilities */
void cmd_top(void);
void cmd_sed(const char *args);
void cmd_tar(const char *args);
void cmd_which(const char *args);
void cmd_ln(const char *args);
void cmd_true(void);
void cmd_false(void);
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
void cmd_logname(void);
void cmd_who(const char *args);
void cmd_banner(const char *args);
int cmd_tsort(int argc, char **argv);
int cmd_join(int argc, char **argv);
void cmd_sum(const char *args);
void cmd_sync(void);
int cmd_unexpand(int argc, char **argv);
void cmd_bt(void);

/* Phase 8 — new shell commands */
void cmd_cmp(const char *args);
void cmd_dirname(const char *args);
void cmd_groups(void);
void cmd_link(const char *args);
void cmd_mknod(const char *args);
void cmd_nohup(const char *args);
void cmd_printenv(const char *args);
void cmd_realpath(const char *args);
void cmd_rmdir(const char *args);
void cmd_shred(const char *args);
void cmd_truncate(const char *args);
void cmd_tty(void);
void cmd_unlink(const char *args);
void cmd_chgrp(const char *args);
void cmd_hostid(void);
void cmd_arch(const char *args);
void cmd_factor(const char *args);
int cmd_fmt(int argc, char **argv);
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
void cmd_mesg(void);
void cmd_nproc(void);
void cmd_pinky(void);
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
int cmd_pr(int argc, char **argv);
void cmd_look(const char *args);
void cmd_locale(const char *args);
void cmd_localedef(const char *args);
void cmd_iconv(const char *args);
void cmd_script(const char *args);
void cmd_mcookie(const char *args);
void cmd_shar(const char *args);

void cmd_timeout(const char *args);
void cmd_stdbuf(const char *args);
void cmd_b2sum(const char *args);
int cmd_base32(int argc, char **argv);
void cmd_basenc(const char *args);
void cmd_chcon(const char *args);
void cmd_runcon(const char *args);
void cmd_csplit(const char *args);
void cmd_dircolors(const char *args);
void cmd_install(const char *args);
void cmd_numfmt(const char *args);
void cmd_ptx(const char *args);
void cmd_sha1sum(const char *args);
void cmd_sha224sum(const char *args);
void cmd_sha384sum(const char *args);
void cmd_sha512sum(const char *args);
void cmd_colrm(const char *args);
void cmd_column(const char *args);
void cmd_crontab(const char *args);
void cmd_diff3(const char *args);
void cmd_lsattr(const char *args);
void cmd_lessecho(const char *args);
void cmd_lesskey(const char *args);
void cmd_ipcrm(const char *args);
void cmd_ipcs(const char *args);
void cmd_isosize(const char *args);

/* Phase 11+ — recently added utility commands (missing from earlier phases) */
void cmd_host(const char *args);
void cmd_col(const char *args);

/* Phase 12 — system utility commands */
void cmd_moc(const char *args);
void cmd_neofetch(void);
void cmd_neofetch_wrapper(const char *args); /* wrapper: void → const char * */
void cmd_pmap(const char *args);
void cmd_pwdx(void);
void cmd_flock(const char *args);
void cmd_lslocks(const char *args);
void cmd_nsenter(const char *args);
void cmd_unshare(const char *args);
void cmd_setsid(const char *args);
void cmd_stdbuf_pipe(const char *args);
void cmd_time_verbose(const char *args);

/* Phase 13 — new commands */
int cmd_ar(int argc, char **argv);
int cmd_chvt(int argc, char **argv);
int cmd_cpio(int argc, char **argv);
int cmd_dc(int argc, char **argv);
int cmd_devmem(int argc, char **argv);
int cmd_dnsdomainname(int argc, char **argv);
int cmd_dos2unix(int argc, char **argv);
int cmd_eject(int argc, char **argv);
int cmd_fgconsole(int argc, char **argv);

/* IOMMU, nftables, and CET commands */
void cmd_iommu(void);
void cmd_nft(const char *args);
void cmd_cet(void);

/* SSH commands */
void cmd_ssh(const char *args);
void cmd_sshd(const char *args);

/* Module commands (M21-M22) */
void cmd_insmod(const char *args);
void cmd_rmmod(const char *args);
void cmd_lsmod(void);
void cmd_modinfo(const char *args);
void cmd_modprobe(const char *args);

/* User privilege commands (U31) */
void cmd_sudo(const char *args);

/* System logger daemon (U7) */
void cmd_syslogd(const char *args);

/* Getty serial login daemon (U12) */
void cmd_getty(const char *args);

/* DHCP client daemon (U8) */
void cmd_dhcpcd(const char *args);

/* Internet super-server daemon (U9) */
void cmd_inetd(const char *args);

/* Cron daemon (U10) */
void cmd_crond(const char *args);

/* Filesystem table / mount command (U25) */
void cmd_mount(const char *args);

/* Device node manager (U11) */
void cmd_mdev(const char *args);

/* Nano text editor (U48) */
void cmd_nano(const char *args);

/* Online filesystem integrity check (Item 277) */
void cmd_fsck(const char *args);

/* S-plan new commands */
void cmd_mkdosfs(const char *args);
void mkdosfs_init(void);

/* New S-plan commands */
void cmd_ntpdate(const char *args);
void cmd_tftpd(const char *args);
void ntpdate_init(void);
void tftpd_init(void);

/* S-plan Boot/Init commands */
void cmd_journald(const char *args);
void cmd_journalctl(const char *args);
void cmd_systemctl(const char *args);
void cmd_udevd(const char *args);

/* S-plan Sound server */
void cmd_pulse(const char *args);

/* New performance and tracing commands */
void cmd_perf(const char *args);
void cmd_trace(const char *args);
void cmd_bpftrace(const char *args);
void cmd_losetup_wrapper(const char *args);
void cmd_lvcreate(const char *args);
void cmd_pvs(const char *args);
void cmd_fstrim_wrapper(const char *args);

#endif
