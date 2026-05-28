# GitHub Actions Security Setup

This document explains how to configure the PR approval gates for the CI
workflow running on self-hosted runners.

## Problem

Self-hosted runners in public repositories are vulnerable to:
- Forked PRs running arbitrary code on your runner
- Malicious scripts accessing your network, files, or data
- Resource exhaustion or denial of service

## Solution: Environment Protection Rules

GitHub Environments allow you to require manual approval before jobs can
run. This prevents untrusted PRs from executing on your self-hosted runner
without explicit approval.

## Setup Instructions

### 1. Create the `ci-approval` Environment

Go to your repository settings:

1. **Settings** → **Environments** → **New environment**
2. Name: `ci-approval`
3. Enable **Required reviewers**
4. Add yourself (and any other maintainers) as required reviewers
5. Optionally enable:
   - **Wait timer**: Add a delay before deployment proceeds
   - **Deployment branches**: Restrict which branches can use this environment

### 2. Configure Branch Protection Rules

Go to **Settings** → **Branches** → **Add rule** for `main`:

1. ✅ Require status checks to pass before merging
2. ✅ Require branches to be up to date before merging
3. Add these required status checks:
   - `build` (from ci-pr.yml)
   - `build-strict` (from ci-pr.yml)
   - `test` (from ci-pr.yml)
   - `e2e` (from ci-pr.yml)
4. ✅ Require conversation resolution before merging
5. ✅ Require linear history (optional, prevents merge commits)

### 3. Configure Pull Request Settings

Go to **Settings** → **General** → **Pull Requests**:

1. ✅ Allow squash merging (default)
2. ✅ Allow auto-merge
3. ✅ Automatically delete head branches

### 4. Configure Actions Permissions

Go to **Settings** → **Actions** → **General**:

1. **Workflow permissions**: Read and write permissions
2. **Fork pull request workflows**: **Require approval for first-time contributors**
   (this is the GitHub-provided safety net)

## How It Works

### Push to main/master
- Runs `ci.yml` directly on the self-hosted runner
- No approval required (trusted code)
- All jobs run immediately

### Pull Request (untrusted)
- Runs `ci-pr.yml` with environment protection
- `security-check` job runs immediately (on GitHub-hosted runner)
- All other jobs require approval via the `ci-approval` environment
- A maintainer must approve before jobs can run on self-hosted runners
- Once approved, jobs run on the self-hosted runner

### Security Flow

```
Fork PR
  ↓
Pull Request opened
  ↓
ci-pr.yml triggers
  ↓
security-check runs (ubuntu-latest, no secrets)
  ↓
build/build-strict/test/e2e wait for approval
  ↓
Maintainer reviews PR code
  ↓
Maintainer approves "ci-approval" environment
  ↓
Jobs execute on self-hosted runner
  ↓
Results posted back to PR
```

## Additional Security Measures

See [scripts/runner-hardening/](scripts/runner-hardening/) for:
- Network isolation (iptables)
- Filesystem hardening (immutable config, module blacklist)
- Process hardening (cgroups, capability drops)
- Audit logging

## Monitoring

Check runner activity:
```bash
# View recent runner jobs
journalctl -u actions.runner.rusik69-os.aws-selfhosted.service --since "1 hour ago"

# Check audit logs
ausearch -k runner_binary --recent
ausearch -k privilege_escalation --recent

# Monitor network connections
ss -tuln | grep -v "127.0.0.1"
```

## Emergency Stop

If you suspect the runner has been compromised:

```bash
# Stop the runner immediately
sudo systemctl stop actions.runner.rusik69-os.aws-selfhosted.service

# Check for suspicious processes
ps aux | grep -v "grep\|systemd\|ssh\|bash"

# Review recent commands
history
```
