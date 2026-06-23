# Shell Subsystem

**Path:** `userspace/kmods/shell/` (main shell code)
**Kernel hooks:** `src/kernel/shell_hooks.c`
**Headers:** `src/include/shell.h`, `src/include/shell_cmds.h`, `src/include/shell_cmd_table.h`

The shell subsystem provides a built-in command-line interpreter with 356+
commands, scripting support, job control, command completion, persistent
history, and shell variables with array support. The shell runs as a
userspace module (`kmod`) but integrates deeply with kernel process
management via the `shell_init()`/`shell_run()` hooks called during boot.

## Architecture

```
shell_init() called from kernel_main (boot sequence step 27)
    │
    └── shell_run() — main REPL loop
         │
         ├── readline with tab completion (shell_completion.inc)
         ├── parser: tokenize, expand variables, parse redirections
         ├── interpreter: execute builtins, pipelines, scripts
         └── job control: foreground/background, SIGTSTP handling
```

## File Descriptions

### Core Shell (`userspace/kmods/shell/`)

| File | Description |
|------|-------------|
| `shell.c` | Main shell — REPL loop, command parsing, pipeline execution (64KB double-buffered I/O), builtin dispatch, tab completion, input/output redirection, command table lookup |
| `shell_cmd_table.c` | Command table — auto-generated registration of 356+ commands, provides lookup by name for builtin dispatch |
| `shell_vars.c` | Shell variables — variable set/get/expand, ${var:-default}, ${var:+alt}, ${#var}, indexed arrays arr=(a b c) |
| `script.c` | Script execution — runs shell scripts from files, shebang handling, command-line argument passing |
| `syntax.c` | Shell syntax parser — tokenization, command-line parsing, operator precedence, quote handling |
| `editor.c` | Line editor — command-line editing, cursor movement, insert/delete, history search |
| `job_control.c` | Job control — background/foreground process management, SIGTSTP/SIGCONT handling, pipeline process groups |
| `history_persist.c` | History persistence — saves/loads command history to/from disk across sessions |
| `shell_history.inc` | History data — inline history storage (HISTORY_SIZE = 16 entries by default) |
| `shell_completion.inc` | Tab completion — command name and file path completion logic |

### Kernel Integration

| File | Description |
|------|-------------|
| `src/kernel/shell_hooks.c` | Kernel-side shell hooks — shell_init() called from kernel_main, initializes console and launches shell on /dev/console |
| `src/include/shell.h` | Shell API header — exposes shell_init, shell_run, shell_exec_cmd, shell_var_*, shell_alias_*, history, and stdin APIs |
| `src/include/shell_cmds.h` | Command registration infrastructure — macros for adding builtin commands to the command table |
| `src/include/shell_cmd_table.h` | Command table header — auto-generated table of all registered commands |

### Commands (`userspace/kmods/shell/cmds/`)

The shell provides 356+ builtin commands covering:

- **File operations:** cat, cp, mv, rm, ln, mkdir, rmdir, touch, chmod,
  chown, ls, find, stat, dd, tee, head, tail, less, strings, sort, uniq,
  split, tac, tr, tsort, truncate, basename, dirname, readlink, realpath
- **Text processing:** grep, sed, awk, cut, paste, join, wc, diff, patch,
  comm, printf, echo, hexdump, xxd, nl, od, fold, expand, unexpand
- **Compression:** gzip, gunzip, bzip2, bunzip2, xz, unxz, lzma, unlzma,
  zcat, zdiff, zgrep, zip, unzip, zipcloak, zipnote, zipsplit
- **Network:** ping, telnet, ssh, wget, curl, nc, ifconfig, route, iptables,
  nft, dhcp, tcpdump, socat, inetd, sshd, tftpd, syslogd, udevd, dns
- **Process & system:** ps, top, kill, nice, renice, timeout, ulimit, umask,
  uptime, vmstat, lsof, fuser, pidof, pgrep, pkill, taskset, setsid
- **Users:** who, whoami, su, sudo, useradd, userdel, passwd, chsh, id, logname
- **Development:** cc, make, as, ld, ar, nm, objdump, strip, size, strings
- **Filesystem:** mount, umount, mkfs.ext2, mkfs.fat, fsck, dd, sync, df, du
- **Device mapper:** dmsetup, lvcreate, lvremove, pvcreate, vgcreate
- **Other:** bc, time, sleep, test, true, false, yes, seq, env, printenv,
  alias, type, eval, read, dirs/pushd/popd, trap, source, exec, exit

## Key Features

- **Pipelines:** Commands connected via `|` with 64KB double-buffered I/O
- **Redirection:** stdin/stdout/stderr redirection with `>`, `>>`, `<`, `2>`,
  `2>&1`, heredocs (`<<`), and here-strings (`<<<`)
- **Variable expansion:** `${var}`, `${var:-default}`, `${var:+alt}`,
  `${#var}`, `${var/pattern/replacement}`
- **Indexed arrays:** `arr=(a b c)`, `${arr[1]}`, `${#arr[@]}`, associative
  arrays planned
- **Job control:** Ctrl-Z suspend, bg/fg, job listing, SIGTSTP/SIGCONT
- **Tab completion:** Completes command names, file paths, and variable names
- **Persistent history:** Command history saved across sessions
- **Scripting:** Shebang-aware script execution, positional parameters,
  return values, source, subshells via `$(...)` and backticks
