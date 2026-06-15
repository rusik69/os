# Syscall wrappers using the `syscall` instruction.
# Kernel ABI: syscall number in rax, args in rdi, rsi, rdx, r10, r8, r9.
# Return value in rax (negative = error, positive = success/pid).
# Plain GAS syntax (no macros, no C preprocessor).

.text

# write(fd, buf, count)
.globl write
.type write, @function
write:
    movl    $1, %eax
    syscall
    ret
.size write, . - write

# read(fd, buf, count)
.globl read
.type read, @function
read:
    movl    $0, %eax
    syscall
    ret
.size read, . - read

# open(path, flags, mode)
.globl open
.type open, @function
open:
    movl    $2, %eax
    syscall
    ret
.size open, . - open

# close(fd)
.globl close
.type close, @function
close:
    movl    $3, %eax
    syscall
    ret
.size close, . - close

# exit(status)
.globl exit
.type exit, @function
exit:
    movl    $4, %eax
    syscall
    ret
.size exit, . - exit

# getpid()
.globl getpid
.type getpid, @function
getpid:
    movl    $5, %eax
    syscall
    ret
.size getpid, . - getpid

# kill(pid, sig)
.globl kill
.type kill, @function
kill:
    movl    $6, %eax
    syscall
    ret
.size kill, . - kill

# brk(addr)
.globl brk
.type brk, @function
brk:
    movl    $7, %eax
    syscall
    ret
.size brk, . - brk

# stat(path, buf)
.globl stat
.type stat, @function
stat:
    movl    $8, %eax
    syscall
    ret
.size stat, . - stat

# mkdir(path, mode)
.globl mkdir
.type mkdir, @function
mkdir:
    movl    $9, %eax
    syscall
    ret
.size mkdir, . - mkdir

# unlink(path)
.globl unlink
.type unlink, @function
unlink:
    movl    $10, %eax
    syscall
    ret
.size unlink, . - unlink

# yield() — sched_yield
.globl yield
.type yield, @function
yield:
    movl    $12, %eax
    syscall
    ret
.size yield, . - yield

# getdents64(fd, buf, count)
.globl getdents64
.type getdents64, @function
getdents64:
    movl    $78, %eax
    syscall
    ret
.size getdents64, . - getdents64

# waitpid(pid, status, options)
.globl waitpid
.type waitpid, @function
waitpid:
    movl    $119, %eax
    syscall
    ret
.size waitpid, . - waitpid

# chdir(path)
.globl chdir
.type chdir, @function
chdir:
    movl    $204, %eax
    syscall
    ret
.size chdir, . - chdir

# getcwd(buf, size)
.globl getcwd
.type getcwd, @function
getcwd:
    movl    $205, %eax
    syscall
    ret
.size getcwd, . - getcwd

# fork()
.globl fork
.type fork, @function
fork:
    movl    $211, %eax
    syscall
    ret
.size fork, . - fork

# signal(signum, handler)
.globl signal
.type signal, @function
signal:
    movl    $213, %eax
    syscall
    ret
.size signal, . - signal

# lseek(fd, offset, whence)
.globl lseek
.type lseek, @function
lseek:
    movl    $214, %eax
    syscall
    ret
.size lseek, . - lseek

# setpgid(pid, pgid)
.globl setpgid
.type setpgid, @function
setpgid:
    movl    $221, %eax
    syscall
    ret
.size setpgid, . - setpgid

# getpgid(pid)
.globl getpgid
.type getpgid, @function
getpgid:
    movl    $222, %eax
    syscall
    ret
.size getpgid, . - getpgid

# execve(path, argv, envp) - kernel's ELF exec
.globl execve
.type execve, @function
execve:
    movl    $234, %eax
    syscall
    ret
.size execve, . - execve

# dup(oldfd)
.globl dup
.type dup, @function
dup:
    movl    $240, %eax
    syscall
    ret
.size dup, . - dup

# dup2(oldfd, newfd)
.globl dup2
.type dup2, @function
dup2:
    movl    $241, %eax
    syscall
    ret
.size dup2, . - dup2

# nanosleep(req, rem)
.globl nanosleep
.type nanosleep, @function
nanosleep:
    movl    $246, %eax
    syscall
    ret
.size nanosleep, . - nanosleep

# uname(buf)
.globl uname
.type uname, @function
uname:
    movl    $248, %eax
    syscall
    ret
.size uname, . - uname

# pipe(fds)
.globl pipe
.type pipe, @function
pipe:
    movl    $249, %eax
    syscall
    ret
.size pipe, . - pipe

# getppid()
.globl getppid
.type getppid, @function
getppid:
    movl    $250, %eax
    syscall
    ret
.size getppid, . - getppid

# alarm(seconds)
.globl alarm
.type alarm, @function
alarm:
    movl    $251, %eax
    syscall
    ret
.size alarm, . - alarm

# getuid()
.globl getuid
.type getuid, @function
getuid:
    movl    $254, %eax
    syscall
    ret
.size getuid, . - getuid

# geteuid()
.globl geteuid
.type geteuid, @function
geteuid:
    movl    $255, %eax
    syscall
    ret
.size geteuid, . - geteuid

# getgid()
.globl getgid
.type getgid, @function
getgid:
    movl    $256, %eax
    syscall
    ret
.size getgid, . - getgid

# getegid()
.globl getegid
.type getegid, @function
getegid:
    movl    $257, %eax
    syscall
    ret
.size getegid, . - getegid

# rmdir(path)
.globl rmdir
.type rmdir, @function
rmdir:
    movl    $258, %eax
    syscall
    ret
.size rmdir, . - rmdir

# umask(mask)
.globl umask
.type umask, @function
umask:
    movl    $270, %eax
    syscall
    ret
.size umask, . - umask

# ioctl(fd, cmd, arg)
.globl ioctl
.type ioctl, @function
ioctl:
    movl    $278, %eax
    syscall
    ret
.size ioctl, . - ioctl

# fstatat(dirfd, path, buf, flags)
.globl fstatat
.type fstatat, @function
fstatat:
    movl    $290, %eax
    syscall
    ret
.size fstatat, . - fstatat

# readlinkat(dirfd, path, buf, bufsize)
.globl readlinkat
.type readlinkat, @function
readlinkat:
    movl    $294, %eax
    syscall
    ret
.size readlinkat, . - readlinkat

# posix_spawn(path, argv, envp)
.globl posix_spawn
.type posix_spawn, @function
posix_spawn:
    movl    $777, %eax
    syscall
    ret
.size posix_spawn, . - posix_spawn

# rename(old, new)
.globl rename
.type rename, @function
rename:
    movl    $259, %eax
    syscall
    ret
.size rename, . - rename

# chmod(path, mode)
.globl chmod
.type chmod, @function
chmod:
    movl    $260, %eax
    syscall
    ret
.size chmod, . - chmod

# access(path, mode)
.globl access
.type access, @function
access:
    movl    $253, %eax
    syscall
    ret
.size access, . - access

# statfs(path, buf)
.globl statfs
.type statfs, @function
statfs:
    movl    $346, %eax
    syscall
    ret
.size statfs, . - statfs

# ftruncate(fd, length)
.globl ftruncate
.type ftruncate, @function
ftruncate:
    movl    $283, %eax
    syscall
    ret
.size ftruncate, . - ftruncate

# clock_gettime(clockid, tp)
.globl clock_gettime
.type clock_gettime, @function
clock_gettime:
    movl    $333, %eax
    syscall
    ret
.size clock_gettime, . - clock_gettime

# sysinfo(info)
.globl sysinfo
.type sysinfo, @function
sysinfo:
    movl    $349, %eax
    syscall
    ret
.size sysinfo, . - sysinfo

# utimensat(dirfd, path, times, flags)
# 4th arg (flags) comes in rcx, but kernel expects it in r10
.globl utimensat
.type utimensat, @function
utimensat:
    mov     %rcx, %r10
    movl    $344, %eax
    syscall
    ret
.size utimensat, . - utimensat

# sync()
.globl sync
.type sync, @function
sync:
    movl    $311, %eax
    syscall
    ret
.size sync, . - sync

# reboot()
.globl reboot
.type reboot, @function
reboot:
    movl    $267, %eax
    syscall
    ret
.size reboot, . - reboot

# gethostname(name, len)
.globl gethostname
.type gethostname, @function
gethostname:
    movl    $269, %eax
    syscall
    ret
.size gethostname, . - gethostname

# net_present() — return 1 if net available, else 0
.globl net_present
.type net_present, @function
net_present:
    movl    $124, %eax
    syscall
    ret
.size net_present, . - net_present

# net_get_mac(mac) — fill 6-byte MAC buffer
.globl net_get_mac
.type net_get_mac, @function
net_get_mac:
    movl    $125, %eax
    syscall
    ret
.size net_get_mac, . - net_get_mac

# net_get_ip(ip) — fill 4-byte IP buffer
.globl net_get_ip
.type net_get_ip, @function
net_get_ip:
    movl    $126, %eax
    syscall
    ret
.size net_get_ip, . - net_get_ip

# net_get_gw() — returns gateway IP as uint32
.globl net_get_gw
.type net_get_gw, @function
net_get_gw:
    movl    $127, %eax
    syscall
    ret
.size net_get_gw, . - net_get_gw

# net_get_mask() — returns netmask as uint32
.globl net_get_mask
.type net_get_mask, @function
net_get_mask:
    movl    $128, %eax
    syscall
    ret
.size net_get_mask, . - net_get_mask

# net_dns(host) — resolve hostname, returns IP or -1
.globl net_dns
.type net_dns, @function
net_dns:
    movl    $129, %eax
    syscall
    ret
.size net_dns, . - net_dns

# net_ping(ip) — ping an IP address
.globl net_ping
.type net_ping, @function
net_ping:
    movl    $130, %eax
    syscall
    ret
.size net_ping, . - net_ping

# net_udp_send(dst_ip, src_port, dst_port, data, len)
.globl net_udp_send
.type net_udp_send, @function
net_udp_send:
    movl    $131, %eax
    syscall
    ret
.size net_udp_send, . - net_udp_send

# net_http_get(host, port, path, buf, bufsz)
.globl net_http_get
.type net_http_get, @function
net_http_get:
    movl    $132, %eax
    syscall
    ret
.size net_http_get, . - net_http_get

# net_arp_list()
.globl net_arp_list
.type net_arp_list, @function
net_arp_list:
    movl    $133, %eax
    syscall
    ret
.size net_arp_list, . - net_arp_list

# net_tcp_listen(port)
.globl net_tcp_listen
.type net_tcp_listen, @function
net_tcp_listen:
    movl    $183, %eax
    syscall
    ret
.size net_tcp_listen, . - net_tcp_listen

# net_tcp_accept(port, timeout_ticks)
.globl net_tcp_accept
.type net_tcp_accept, @function
net_tcp_accept:
    movl    $184, %eax
    syscall
    ret
.size net_tcp_accept, . - net_tcp_accept

# net_tcp_send_conn(conn_id, buf, len)
.globl net_tcp_send_conn
.type net_tcp_send_conn, @function
net_tcp_send_conn:
    movl    $185, %eax
    syscall
    ret
.size net_tcp_send_conn, . - net_tcp_send_conn

# net_tcp_recv_conn(conn_id, buf, bufsz)
.globl net_tcp_recv_conn
.type net_tcp_recv_conn, @function
net_tcp_recv_conn:
    movl    $186, %eax
    syscall
    ret
.size net_tcp_recv_conn, . - net_tcp_recv_conn

# net_tcp_close_conn(conn_id)
.globl net_tcp_close_conn
.type net_tcp_close_conn, @function
net_tcp_close_conn:
    movl    $187, %eax
    syscall
    ret
.size net_tcp_close_conn, . - net_tcp_close_conn

# net_tcp_unlisten(port)
.globl net_tcp_unlisten
.type net_tcp_unlisten, @function
net_tcp_unlisten:
    movl    $188, %eax
    syscall
    ret
.size net_tcp_unlisten, . - net_tcp_unlisten

# net_tcp_connect(ip, port)
.globl net_tcp_connect
.type net_tcp_connect, @function
net_tcp_connect:
    movl    $189, %eax
    syscall
    ret
.size net_tcp_connect, . - net_tcp_connect

# net_udp_listen(port)
.globl net_udp_listen
.type net_udp_listen, @function
net_udp_listen:
    movl    $198, %eax
    syscall
    ret
.size net_udp_listen, . - net_udp_listen

# net_udp_recv(port, buf, bufsz, src_ip, src_port)
.globl net_udp_recv
.type net_udp_recv, @function
net_udp_recv:
    movl    $199, %eax
    syscall
    ret
.size net_udp_recv, . - net_udp_recv

# net_udp_unlisten(port)
.globl net_udp_unlisten
.type net_udp_unlisten, @function
net_udp_unlisten:
    movl    $200, %eax
    syscall
    ret
.size net_udp_unlisten, . - net_udp_unlisten

# net_connlist()
.globl net_connlist
.type net_connlist, @function
net_connlist:
    movl    $212, %eax
    syscall
    ret
.size net_connlist, . - net_connlist
