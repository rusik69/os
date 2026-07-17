# AGENTS.md — rusik69/os Project

This file guides AI agents working on the OS kernel project.

## CI & Build

- **Self-hosted runner only.** All CI workflows run on `[self-hosted, linux, x64]` — the machine Hermes runs on. Do not add `ubuntu-latest` or any GitHub-hosted runner label to any job.
- **Zero-warnings mandatory.** The project builds with `-Wall -Wextra -Werror`. Every commit must compile cleanly.
- **CI must stay green.** After any change, run a local build (`make -j$(nproc) CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld OBJCOPY=x86_64-linux-gnu-objcopy`) and verify zero errors.
- **Periodic CI health check.** The `os-ci-auto-fix` cron job runs every 3 minutes — it builds, format-checks (`make format-check`), whitespace-checks (`make check-whitespace`), and fixes any failures it finds. Do not modify or disable this cron job.
- **Format and lint.** Always run `make verify` before pushing. If the format check fails, run `git clang-format --style=file` to auto-fix.

## Build Commands

```bash
# Full build (kernel + userspace + disk image)
make -j$(nproc) CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld OBJCOPY=x86_64-linux-gnu-objcopy

# Userspace only
make -C userspace -j$(nproc) CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld

# Pre-merge verification (format + lint + app boundary)
make verify CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld OBJCOPY=x86_64-linux-gnu-objcopy

# Format check only
make format-check

# Trailing whitespace check
make check-whitespace
```

## Project Structure

- `src/` — Kernel source (core kernel, drivers, filesystems, networking, memory, IPC)
- `userspace/` — Userspace programs, libc, init, shell, GUI, doom, DOS emulator
- `tests/` — Unit tests (host_libc, unit) and e2e test scripts
- `build/` — Build output (kernel.bin, disk.img)
- `.github/workflows/` — CI workflow definitions (ci.yml, ci-pr.yml, docker-ci-image.yml)

## Commit Style

- Use conventional commits: `feat:`, `fix:`, `ci:`, `docs:`, `refactor:`, `test:`, `perf:`, `sec:`
- Reference item numbers for plan-driven work: `P01 item 5: FAT32 buffer overflow check`
- Never commit broken code. Revert with `git checkout -- .` if a fix can't be completed.

## Commit & Push

```bash
# Stage all changes
git add -A

# Commit with a descriptive message
git commit -m "type: description of what changed"

# Push to origin main
git push origin main
```

Always push after committing so CI picks up the latest state. The self-hosted runner listens for new commits and runs automatically.

## Temporary Files

Remove temporary files from the working tree before committing:

- **Build artifacts** — `build/`, `build_test/`, `build_check*`, `build_analyze/` — gitignored, but don't `git add -A` with stray build output outside those dirs
- **Editor swap files** — `*.swp`, `*.swo`, `*~` — remove with `find . -name '*.swp' -o -name '*~' -delete`
- **`.o`/`.elf`/`.ko` files** — should all be under build dirs. If any appear in the source tree, delete them
- **Compiler temp files** — `*.i`, `*.s` preprocessor/assembly output
- **Plan files** — `OS-IMPROVEMENT-PLAN.md`, `os-improvements-progress.json`, `bug-hunt-progress.json`, and anything under `.hermes/plans/` — `.hermes/` is gitignored, so these files stay local and are never committed. Do not `git add -f` them.
- **Progress state files** — tracked plan progress lives under `.hermes/plans/` and must NOT be added to git
- Check with `git status --short` before every commit — unexpected untracked files are likely temp artifacts
