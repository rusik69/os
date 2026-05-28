#!/usr/bin/env bash
# Runner Security Hardening Script
# Applies all security measures to the GitHub Actions self-hosted runner
# Run as root: sudo bash scripts/runner-hardening/harden.sh

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[+]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err() { echo -e "${RED}[-]${NC} $1" >&2; }

# Check root
if [[ $EUID -ne 0 ]]; then
    err "This script must be run as root"
    exit 1
fi

# Check if runner exists
RUNNER_DIR="${RUNNER_DIR:-/home/ubuntu/actions-runner}"
if [[ ! -d "$RUNNER_DIR" ]]; then
    err "Runner directory not found at $RUNNER_DIR"
    exit 1
fi

echo "=== GitHub Actions Runner Security Hardening ==="
echo ""

# --- 1. Network isolation with iptables ---
log "Applying network isolation..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "$SCRIPT_DIR/runner-network-isolation.sh"
echo ""

# --- 2. File system hardening ---
log "Applying file system hardening..."
bash "$SCRIPT_DIR/runner-fs-hardening.sh"
echo ""

# --- 3. Process hardening ---
log "Applying process hardening..."
bash "$SCRIPT_DIR/runner-process-hardening.sh"
echo ""

log "=== Hardening complete ==="
echo ""
echo "Security measures applied:"
echo "  - Network: outbound blocked (except GitHub, DNS)"
echo "  - File system: runner dir read-only (except work dirs)"
echo "  - Process: cgroup limits, capability drops"
echo ""
echo "Verify with: sudo bash $SCRIPT_DIR/runner-verify.sh"
