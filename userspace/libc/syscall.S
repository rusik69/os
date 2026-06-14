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

# brk(addr)
.globl brk
.type brk, @function
brk:
    movl    $7, %eax
    syscall
    ret
.size brk, . - brk

# waitpid(pid, status, options)
.globl waitpid
.type waitpid, @function
waitpid:
    movl    $119, %eax
    syscall
    ret
.size waitpid, . - waitpid

# fork()
.globl fork
.type fork, @function
fork:
    movl    $211, %eax
    syscall
    ret
.size fork, . - fork

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

# nanosleep(req, rem)
.globl nanosleep
.type nanosleep, @function
nanosleep:
    movl    $246, %eax
    syscall
    ret
.size nanosleep, . - nanosleep

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

# execve(path, argv, envp) - kernel's ELF exec
.globl execve
.type execve, @function
execve:
    movl    $155, %eax
    syscall
    ret
.size execve, . - execve

# posix_spawn(path, argv, envp)
.globl posix_spawn
.type posix_spawn, @function
posix_spawn:
    movl    $211, %eax
    syscall
    ret
.size posix_spawn, . - posix_spawn
