# Syscall Reference

This document lists all system calls implemented in the Hermes OS kernel. The syscall ABI is Linux-compatible where possible, using the x86-64 `syscall` instruction via `MSR_LSTAR`.

## Calling Convention

| Register | Purpose |
|----------|---------|
| RAX      | Syscall number |
| RDI      | arg1 |
| RSI      | arg2 |
| RDX      | arg3 |
| R10      | arg4 |
| R8       | arg5 |
| R9       | arg6 |

Return value in RAX (negative = errno). Compatible with Linux x86-64 syscall ABI.

## Syscall Number Table

### Core Syscalls (Linux-compatible, 0–13)

| # | Name | Signature | Description |
|---|------|-----------|-------------|
| 0 | `SYS_READ` | `read(fd, buf, count)` | Read from file descriptor |
| 1 | `SYS_WRITE` | `write(fd, buf, count)` | Write to file descriptor |
| 2 | `SYS_OPEN` | `open(path, flags, mode)` | Open file |
| 3 | `SYS_CLOSE` | `close(fd)` | Close file descriptor |
| 4 | `SYS_EXIT` | `exit(status)` | Terminate process |
| 5 | `SYS_GETPID` | `getpid()` | Get process ID |
| 6 | `SYS_KILL` | `kill(pid, sig)` | Send signal |
| 7 | `SYS_BRK` | `brk(addr)` | Change data segment size |
| 8 | `SYS_STAT` | `stat(path, buf)` | Get file status |
| 9 | `SYS_MKDIR` | `mkdir(path, mode)` | Create directory |
| 10 | `SYS_UNLINK` | `unlink(path)` | Remove file |
| 11 | `SYS_TIME` | `time(tloc)` | Get time in seconds |
| 12 | `SYS_YIELD` | `yield()` | Yield CPU |
| 13 | `SYS_UPTIME` | `uptime()` | Get system uptime |

### Extended Filesystem Syscalls (100–118)

| # | Name | Signature | Description |
|---|------|-----------|-------------|
| 78 | `SYS_GETDENTS64` | `getdents64(fd, dirp, count)` | Get directory entries |
| 100 | `SYS_FS_FORMAT` | `fs_format(dev, fstype)` | Format filesystem |
| 101 | `SYS_FS_CREATE` | `fs_create(path)` | Create file |
| 102 | `SYS_FS_WRITE` | `fs_write(path, data, len)` | Write file |
| 103 | `SYS_FS_READ` | `fs_read(path, buf, len)` | Read file |
| 104 | `SYS_FS_DELETE` | `fs_delete(path)` | Delete file |
| 105 | `SYS_FS_LIST` | `fs_list(path, buf)` | List directory |
| 106 | `SYS_FS_STAT` | `fs_stat(path, buf)` | Get file status |
| 107 | `SYS_FS_STAT_EX` | `fs_stat_ex(path, buf)` | Extended stat |
| 108 | `SYS_FS_CHMOD` | `fs_chmod(path, mode)` | Change permissions |
| 109 | `SYS_FS_CHOWN` | `fs_chown(path, uid, gid)` | Change owner |
| 110 | `SYS_FS_GET_USAGE` | `fs_get_usage()` | Get filesystem usage |
| 111 | `SYS_FS_LIST_NAMES` | `fs_list_names(buf, len)` | List files by name |
| 112 | `SYS_ATA_PRESENT` | `ata_present()` | Check ATA presence |
| 113 | `SYS_VFS_READ` | `vfs_read(path, buf, len)` | VFS read |
| 114 | `SYS_VFS_WRITE` | `vfs_write(path, buf, len)` | VFS write |
| 115 | `SYS_VFS_STAT` | `vfs_stat(path, buf)` | VFS stat |
| 116 | `SYS_VFS_CREATE` | `vfs_create(path)` | VFS create |
| 117 | `SYS_VFS_UNLINK` | `vfs_unlink(path)` | VFS unlink |
| 118 | `SYS_VFS_READDIR` | `vfs_readdir(path, buf)` | VFS readdir |

### Process and IPC Syscalls (119–250)

| # | Name | Signature | Description |
|---|------|-----------|-------------|
| 119 | `SYS_WAITPID` | `waitpid(pid, status, options)` | Wait for process |
| 120 | `SYS_SLEEP_TICKS` | `sleep_ticks(ticks)` | Sleep for ticks |
| 121 | `SYS_ATA_SECTORS` | `ata_sectors()` | Get ATA sector count |
| 122 | `SYS_AHCI_PRESENT` | `ahci_present()` | Check AHCI presence |
| 123 | `SYS_AHCI_SECTORS` | `ahci_sectors()` | Get AHCI sector count |
| 124 | `SYS_NET_PRESENT` | `net_present()` | Check network presence |
| 125 | `SYS_NET_GET_MAC` | `net_get_mac(buf)` | Get MAC address |
| 126 | `SYS_NET_GET_IP` | `net_get_ip()` | Get IP address |
| 127 | `SYS_NET_GET_GW` | `net_get_gw()` | Get gateway |
| 128 | `SYS_NET_GET_MASK` | `net_get_mask()` | Get netmask |
| 129 | `SYS_NET_DNS` | `net_dns(hostname, ip)` | DNS resolution |
| 130 | `SYS_NET_PING` | `net_ping(ip)` | ICMP ping |
| 131 | `SYS_NET_UDP_SEND` | `net_udp_send(dst, port, data, len)` | Send UDP packet |
| 132 | `SYS_NET_HTTP_GET` | `net_http_get(url, buf)` | HTTP GET |
| 133 | `SYS_NET_ARP_LIST` | `net_arp_list(buf)` | List ARP table |
| 134 | `SYS_PROC_LIST` | `proc_list(buf)` | List processes |
| 135 | `SYS_PCI_LIST` | `pci_list(buf)` | List PCI devices |
| 136 | `SYS_USB_LIST` | `usb_list(buf)` | List USB devices |
| 137 | `SYS_HWINFO_PRINT` | `hwinfo_print(buf)` | Print hardware info |
| 138 | `SYS_USER_FIND` | `user_find(name)` | Find user |
| 139 | `SYS_USER_ADD` | `user_add(name, uid, gid)` | Add user |
| 140 | `SYS_USER_DELETE` | `user_delete(name)` | Delete user |
| 141 | `SYS_USER_PASSWD` | `user_passwd(name, passwd)` | Change password |
| 142 | `SYS_SESSION_LOGIN` | `session_login(user, passwd)` | Login |
| 143 | `SYS_SESSION_LOGOUT` | `session_logout()` | Logout |
| 144 | `SYS_SESSION_GET` | `session_get()` | Get session info |
| 145 | `SYS_USERS_COUNT` | `users_count()` | Count users |
| 146 | `SYS_USERS_GET_BY_INDEX` | `users_get_by_index(idx, buf)` | Get user by index |
| 147 | `SYS_SPEAKER_BEEP` | `speaker_beep(freq, dur)` | PC speaker beep |
| 148 | `SYS_RTC_GET_TIME` | `rtc_get_time(buf)` | Get RTC time |
| 149 | `SYS_ACPI_SHUTDOWN` | `acpi_shutdown()` | ACPI power off |
| 150 | `SYS_MOUSE_GET_STATE` | `mouse_get_state(buf)` | Get mouse state |
| 151 | `SYS_SERIAL_READ` | `serial_read(buf, count)` | Read serial port |
| 152 | `SYS_SERIAL_WRITE` | `serial_write(buf, count)` | Write serial port |
| 153 | `SYS_CMOS_READ_BYTE` | `cmos_read_byte(reg)` | Read CMOS register |
| 154 | `SYS_PMM_GET_STATS` | `pmm_get_stats(buf)` | Get PMM stats |
| 155 | `SYS_ELF_EXEC` | `elf_exec(path, argv)` | Execute ELF binary |
| 156 | `SYS_SCRIPT_EXEC` | `script_exec(path)` | Execute script |
| 157 | `SYS_FAT_MOUNT` | `fat_mount(dev)` | Mount FAT volume |
| 158 | `SYS_FAT_IS_MOUNTED` | `fat_is_mounted(dev)` | Check FAT mount |
| 159 | `SYS_FAT_LIST_DIR` | `fat_list_dir(buf)` | List FAT directory |
| 160 | `SYS_FAT_READ_FILE` | `fat_read_file(path, buf, len)` | Read FAT file |
| 161 | `SYS_FAT_FILE_SIZE` | `fat_file_size(path)` | Get FAT file size |
| 162 | `SYS_SHELL_HISTORY_SHOW` | `shell_history_show()` | Show shell history |
| 163 | `SYS_SHELL_READ_LINE` | `shell_read_line(buf, len)` | Read shell line |
| 164 | `SYS_SHELL_VAR_SET` | `shell_var_set(name, val)` | Set shell variable |
| 165 | `SYS_SHELL_EXEC_CMD` | `shell_exec_cmd(cmd)` | Execute shell command |
| 166 | `SYS_VGA_SET_COLOR` | `vga_set_color(fg, bg)` | Set VGA color |
| 167 | `SYS_VGA_GET_FB_INFO` | `vga_get_fb_info(buf)` | Get framebuffer info |
| 168 | `SYS_CC_COMPILE` | `cc_compile(src, obj)` | Compile C source |
| 169 | `SYS_KEYBOARD_GETCHAR` | `keyboard_getchar()` | Get keyboard char |
| 170 | `SYS_SHELL_HISTORY_ADD` | `shell_history_add(entry)` | Add to history |
| 171 | `SYS_SHELL_HISTORY_COUNT` | `shell_history_count()` | History count |
| 172 | `SYS_SHELL_HISTORY_ENTRY` | `shell_history_entry(idx, buf)` | Get history entry |
| 173 | `SYS_SHELL_TAB_COMPLETE` | `shell_tab_complete(buf)` | Tab completion |
| 174 | `SYS_VGA_PUT_ENTRY_AT` | `vga_put_entry_at(c, fg, bg, x, y)` | Put VGA char |
| 175 | `SYS_VGA_SET_CURSOR` | `vga_set_cursor(x, y)` | Set VGA cursor |
| 176 | `SYS_VGA_CLEAR` | `vga_clear()` | Clear VGA screen |
| 177 | `SYS_GUI_SHELL_RUN` | `gui_shell_run()` | Run GUI shell |
| 178 | `SYS_PROC_SET_CAP_PROFILE` | `proc_set_cap_profile(pid, profile)` | Set capabilities |
| 179 | `SYS_MALLOC` | `malloc(size)` | Userspace malloc |
| 180 | `SYS_FREE` | `free(ptr)` | Userspace free |
| 181 | `SYS_REALLOC` | `realloc(ptr, size)` | Userspace realloc |
| 182 | `SYS_CALLOC` | `calloc(nmemb, size)` | Userspace calloc |
| 183 | `SYS_NET_TCP_LISTEN` | `net_tcp_listen(port)` | TCP listen |
| 184 | `SYS_NET_TCP_ACCEPT` | `net_tcp_accept()` | TCP accept |
| 185 | `SYS_NET_TCP_SEND_CONN` | `net_tcp_send_conn(conn_id, data, len)` | TCP send |
| 186 | `SYS_NET_TCP_RECV_CONN` | `net_tcp_recv_conn(conn_id, buf, len)` | TCP recv |
| 187 | `SYS_NET_TCP_CLOSE_CONN` | `net_tcp_close_conn(conn_id)` | TCP close |
| 188 | `SYS_NET_TCP_UNLISTEN` | `net_tcp_unlisten(port)` | TCP unlisten |
| 189 | `SYS_NET_TCP_CONNECT` | `net_tcp_connect(ip, port)` | TCP connect |
| 190–193 | `SYS_MUTEX_*` | Mutex operations | Init, lock, unlock, destroy |
| 194–197 | `SYS_SEM_*` | Semaphore operations | Init, wait, post, destroy |
| 198–200 | `SYS_NET_UDP_*` | UDP server operations | Listen, recv, unlisten |
| 201–203 | `SYS_FS_SYMLINK/READLINK/LSTAT` | Symlink operations | Create, read, lstat |
| 204 | `SYS_CHDIR` | `chdir(path)` | Change directory |
| 205 | `SYS_GETCWD` | `getcwd(buf, len)` | Get current directory |
| 206 | `SYS_SETPRIORITY` | `setpriority(which, who, prio)` | Set priority |
| 207–210 | `SYS_SHM_*` | Shared memory | Get, attach, detach, free |
| 211 | `SYS_FORK` | `fork()` | Create child process |
| 212 | `SYS_NET_CONNLIST` | `net_connlist(buf)` | List TCP connections |
| 213 | `SYS_SIGNAL` | `signal(sig, handler)` | Set signal handler |
| 214 | `SYS_LSEEK` | `lseek(fd, offset, whence)` | Seek in file |
| 215 | `SYS_TRUNCATE` | `truncate(path, len)` | Truncate file |
| 216 | `SYS_RAW_SEND` | `raw_send(frame, len)` | Send raw Ethernet |
| 217 | `SYS_FD_READ` | `fd_read(fd, buf, count)` | Read from fd |
| 218 | `SYS_FD_WRITE` | `fd_write(fd, buf, count)` | Write to fd |
| 219 | `SYS_SETPRIORITY_PID` | `setpriority_pid(pid, prio)` | Set process priority |
| 220 | `SYS_GETPRIORITY` | `getpriority(pid)` | Get process priority |
| 221 | `SYS_SETPGID` | `setpgid(pid, pgid)` | Set process group |
| 222 | `SYS_GETPGID` | `getpgid(pid)` | Get process group |
| 223 | `SYS_KILLPG` | `killpg(pgrp, sig)` | Kill process group |
| 224 | `SYS_AC97_PRESENT` | `ac97_present()` | Check AC97 audio |
| 225 | `SYS_AC97_BEEP` | `ac97_beep(freq, dur)` | AC97 beep |
| 226 | `SYS_FAT_WRITE_FILE` | `fat_write_file(path, data, len)` | Write FAT file |
| 227 | `SYS_FAT_SYNC` | `fat_sync()` | Flush FAT cache |
| 228 | `SYS_DOOM_RUN` | `doom_run()` | Start DOOM game |
| 229 | `SYS_CC_COMPILE_OBJ` | `cc_compile_obj(src, obj)` | Compile to .o |
| 230 | `SYS_CC_LINK` | `cc_link(objs, out)` | Link ELF binary |
| 235 | `SYS_MMAP` | `mmap(addr, len, prot)` | Memory map |
| 236 | `SYS_MUNMAP` | `munmap(addr, len)` | Unmap memory |
| 237 | `SYS_MPROTECT` | `mprotect(addr, len, prot)` | Change memory protection |
| 238 | `SYS_SCHED_SETAFFINITY` | `sched_setaffinity(pid, mask)` | Set CPU affinity |
| 239 | `SYS_SCHED_GETAFFINITY` | `sched_getaffinity(pid, mask)` | Get CPU affinity |
| 240 | `SYS_DUP` | `dup(oldfd)` | Duplicate fd |
| 241 | `SYS_DUP2` | `dup2(oldfd, newfd)` | Duplicate fd to target |
| 242 | `SYS_FCNTL` | `fcntl(fd, cmd, arg)` | File control |
| 243 | `SYS_SELECT` | `select(nfds, rfds, wfds, efds, timeout)` | I/O multiplexing |
| 244 | `SYS_SETITIMER` | `setitimer(which, val, old)` | Set interval timer |
| 245 | `SYS_GETITIMER` | `getitimer(which, val)` | Get interval timer |
| 246 | `SYS_NANOSLEEP` | `nanosleep(req, rem)` | High-res sleep |
| 247 | `SYS_SYSCONF` | `sysconf(name)` | System configuration |
| 248 | `SYS_UNAME` | `uname(buf)` | Get system info |
| 249 | `SYS_PIPE` | `pipe(fds)` | Create pipe |
| 250 | `SYS_GETPPID` | `getppid()` | Get parent PID |

### POSIX and Extended Syscalls (251–384)

| # | Name | Signature | Description |
|---|------|-----------|-------------|
| 251 | `SYS_ALARM` | `alarm(secs)` | Schedule alarm |
| 252 | `SYS_PAUSE` | `pause()` | Wait for signal |
| 253 | `SYS_ACCESS` | `access(path, mode)` | Check file access |
| 254 | `SYS_GETUID` | `getuid()` | Get user ID |
| 255 | `SYS_GETEUID` | `geteuid()` | Get effective UID |
| 256 | `SYS_GETGID` | `getgid()` | Get group ID |
| 257 | `SYS_GETEGID` | `getegid()` | Get effective GID |
| 258 | `SYS_RMDIR` | `rmdir(path)` | Remove directory |
| 259 | `SYS_RENAME` | `rename(old, new)` | Rename file |
| 260 | `SYS_CHMOD` | `chmod(path, mode)` | Change mode |
| 261 | `SYS_FSYNC` | `fsync(fd)` | Sync file |
| 262 | `SYS_SIGPROCMASK` | `sigprocmask(how, set, old)` | Signal mask |
| 263 | `SYS_SIGPENDING` | `sigpending(set)` | Pending signals |
| 264 | `SYS_READV` | `readv(fd, iov, iovcnt)` | Scatter read |
| 265 | `SYS_WRITEV` | `writev(fd, iov, iovcnt)` | Gather write |
| 266 | `SYS_GETRANDOM` | `getrandom(buf, count, flags)` | Random bytes |
| 267 | `SYS_REBOOT` | `reboot()` | Reboot system |
| 268 | `SYS_SETHOSTNAME` | `sethostname(name, len)` | Set hostname |
| 269 | `SYS_GETHOSTNAME` | `gethostname(name, len)` | Get hostname |
| 270 | `SYS_UMASK` | `umask(mask)` | Set file creation mask |
| 271 | `SYS_MKNOD` | `mknod(path, mode, dev)` | Create device node |
| 272 | `SYS_PRLIMIT64` | `prlimit64(pid, res, new, old)` | Process resource limits |
| 273 | `SYS_FUTEX` | `futex(uaddr, op, val, timeout)` | Fast userspace mutex |
| 274 | `SYS_ARCH_PRCTL` | `arch_prctl(code, addr)` | Architecture prctl |
| 275 | `SYS_POLL` | `poll(fds, nfds, timeout)` | Poll file descriptors |
| 276 | `SYS_EVENTFD` | `eventfd(initval, flags)` | Event fd |
| 277 | `SYS_SENDFILE` | `sendfile(out, in, off, count)` | Zero-copy file transfer |
| 278 | `SYS_IOCTL` | `ioctl(fd, cmd, arg)` | Device control |
| 279 | `SYS_SYSLOG` | `syslog(opt, buf, len)` | System log control |
| 280 | `SYS_PRCTL` | `prctl(op, a2, a3, a4, a5)` | Process control |
| 281 | `SYS_MOUNT` | `mount(src, target, type, flags, data)` | Mount filesystem |
| 282 | `SYS_UMOUNT` | `umount(target)` | Unmount filesystem |
| 283 | `SYS_FTRUNCATE` | `ftruncate(fd, len)` | Truncate file |
| 284 | `SYS_READDIR` | `readdir(fd, buf, count)` | Read directory |
| 285 | `SYS_EXECVEAT` | `execveat(dirfd, path, argv, envp, flags)` | Execute at dirfd |
| 286 | `SYS_SCHED_SETSCHEDULER` | `sched_setscheduler(pid, policy, param)` | Set scheduler |
| 287 | `SYS_SCHED_GETSCHEDULER` | `sched_getscheduler(pid)` | Get scheduler |
| 288–294 | *at syscalls | `*,at(*)` | openat, mkdirat, fstatat, unlinkat, renameat, symlinkat, readlinkat |
| 296 | `SYS_MLOCK` | `mlock(addr, len)` | Lock memory |
| 297 | `SYS_MLOCKALL` | `mlockall(flags)` | Lock all memory |
| 298 | `SYS_MUNLOCK` | `munlock(addr, len)` | Unlock memory |
| 299 | `SYS_MUNLOCKALL` | `munlockall()` | Unlock all memory |
| 300 | `SYS_MINCORE` | `mincore(addr, len, vec)` | Memory residency |
| 301 | `SYS_MADVISE` | `madvise(addr, len, advice)` | Memory advice |
| 302 | `SYS_FALLOCATE` | `fallocate(fd, mode, off, len)` | Preallocate space |
| 303–305 | `SYS_TIMERFD_*` | timerfd operations | Create, settime, gettime |
| 306 | `SYS_SIGNALFD` | `signalfd(fd, mask, flags)` | Signal fd |
| 307 | `SYS_SPLICE` | `splice(in, off_in, out, off_out, len, flags)` | Splice data |
| 308 | `SYS_TEE` | `tee(in, out, len, flags)` | Duplicate pipe content |
| 309 | `SYS_SENDMMSG` | `sendmmsg(sockfd, msgvec, vlen, flags)` | Send multiple messages |
| 310 | `SYS_RECVMMSG` | `recvmmsg(sockfd, msgvec, vlen, flags, timeout)` | Receive multiple messages |
| 311 | `SYS_SYNC` | `sync()` | Sync all filesystems |
| 312 | `SYS_SYNCFS` | `syncfs(fd)` | Sync filesystem |
| 313 | `SYS_SETSID` | `setsid()` | Create session |
| 314 | `SYS_GETSID` | `getsid(pid)` | Get session ID |
| 315 | `SYS_SIGALTSTACK` | `sigaltstack(ss, old)` | Signal stack |
| 316 | `SYS_PERSONALITY` | `personality(persona)` | Set personality |
| 317–328 | BSD Socket API | `socket/bind/listen/accept/...` | Full socket API |
| 329–332 | epoll | `epoll_create1/ctl/wait/pwait` | I/O event notification |
| 333–340 | POSIX Clocks & Timers | `clock_gettime/timer_create/...` | Clock and timer ops |
| 341–345 | Modern FD ops | `dup3/pipe2/mkdtemp/utimensat/futimens` | Extended fd operations |
| 346–349 | Filesystem & System Info | `statfs/fstatfs/getrusage/sysinfo` | System info |
| 350–355 | Credentials & Scheduling | `getresuid/setresuid/.../sched_getparam` | UID/GID and sched params |
| 356–359 | POSIX Message Queues | `mq_open/send/receive/unlink` | Message queue operations |
| 360 | `SYS_GETCPU` | `getcpu(cpu, node, cache)` | Get CPU and NUMA node |
| 361 | `SYS_PREADV` | `preadv(fd, iov, iovcnt, off)` | Scatter read at offset |
| 362 | `SYS_PWRITEV` | `pwritev(fd, iov, iovcnt, off)` | Gather write at offset |
| 363 | `SYS_SIGWAITINFO` | `sigwaitinfo(set, info)` | Synchronous signal wait |
| 364 | `SYS_SIGTIMEDWAIT` | `sigtimedwait(set, info, timeout)` | Timed signal wait |
| 365 | `SYS_MEMFD_CREATE` | `memfd_create(name, flags)` | Anonymous mem fd |
| 366 | `SYS_INIT_MODULE` | `init_module(path, params)` | Load module |
| 367 | `SYS_FINIT_MODULE` | `finit_module(fd, params, flags)` | Load module from fd |
| 368 | `SYS_DELETE_MODULE` | `delete_module(name, flags)` | Unload module |
| 369 | `SYS_QUERY_MODULE` | `query_module(name, buf, size)` | Query module info |
| 370 | `SYS_MREMAP` | `mremap(addr, old, new, flags)` | Remap memory |
| 371 | `SYS_READAHEAD` | `readahead(fd, offset, count)` | Prefetch file data |
| 372 | `SYS_FADVISE64` | `fadvise64(fd, off, len, advice)` | File access advice |
| 373 | `SYS_MEMBARRIER` | `membarrier(cmd, flags, cpu_id)` | Memory barrier |
| 374 | `SYS_PIVOT_ROOT` | `pivot_root(new, old)` | Change root |
| 377 | `SYS_CHROOT` | `chroot(path)` | Chroot |
| 378 | `SYS_COPY_FILE_RANGE` | `copy_file_range(in, off_in, out, off_out, len, flags)` | Zero-copy data transfer |
| 379 | `SYS_NAME_TO_HANDLE_AT` | `name_to_handle_at(dirfd, path, handle, mount_id, flags)` | File handle by path |
| 380 | `SYS_OPEN_BY_HANDLE_AT` | `open_by_handle_at(mount_fd, handle, flags)` | Open by handle |
| 381–383 | Inotify | `inotify_init1/add_watch/rm_watch` | File monitoring |
| 384 | `SYS_USERFAULTFD` | `userfaultfd(cmd, arg)` | Userfault fd |

### Other Syscalls

| # | Name | Signature | Description |
|---|------|-----------|-------------|
| 555 | `SYS_IOPRIO_SET` | `ioprio_set(which, who, ioprio)` | Set I/O priority |
| 556 | `SYS_IOPRIO_GET` | `ioprio_get(which, who)` | Get I/O priority |
| 572 | `SYS_FDATASYNC` | `fdatasync(fd)` | Sync file data |
| 573 | `SYS_SET_ROBUST_LIST` | `set_robust_list(head, len)` | Set robust futex list |
| 574 | `SYS_GET_ROBUST_LIST` | `get_robust_list(pid, head, len)` | Get robust futex list |
| 575 | `SYS_SHMCTL` | `shmctl(id, cmd, arg)` | Shared memory control |

## Error Handling

All syscalls return a negative errno value on failure (Linux-compatible convention). Common errno values:

| Error | Value | Description |
|-------|-------|-------------|
| EPERM | 1 | Operation not permitted |
| ENOENT | 2 | No such file or directory |
| ESRCH | 3 | No such process |
| EINTR | 4 | Interrupted system call |
| EIO | 5 | I/O error |
| ENXIO | 6 | No such device or address |
| EBADF | 9 | Bad file descriptor |
| ENOMEM | 12 | Out of memory |
| EACCES | 13 | Permission denied |
| EFAULT | 14 | Bad address |
| EBUSY | 16 | Device or resource busy |
| EEXIST | 17 | File exists |
| ENODEV | 19 | No such device |
| EINVAL | 22 | Invalid argument |
| ENFILE | 23 | File table overflow |
| EMFILE | 24 | Too many open files |
| ENOSYS | 38 | Function not implemented |
| ENOTSUP | 95 | Operation not supported |

## ABI Compatibility

The syscall interface is designed to be Linux-compatible at the ABI level:
- Syscall numbers follow Linux conventions where applicable (read=0, write=1, open=2, close=3, etc.)
- `struct` layouts match Linux's (stat, timespec, rusage, etc.)
- The `syscall` instruction is used with `MSR_LSTAR` (same as Linux)
- Argument passing via registers: RAX=number, RDI, RSI, RDX, R10, R8, R9
- Return value in RAX (negative = -errno)

See `src/include/syscall.h` and `src/include/syscall_new.h` for the current definitions.
