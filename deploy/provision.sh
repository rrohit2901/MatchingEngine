#!/usr/bin/env bash
# One-time setup of the backtester VM (Ubuntu 24.04, ARM or x86). Run as root:
#     sudo bash provision.sh
# Safe to re-run: every step checks before it changes anything.
# See docs/ui-plan.md (U4) and docs/deploy.md.
set -euo pipefail

ISOLATE_REF="${ISOLATE_REF:-v2.7}"     # https://github.com/ioi/isolate
SWAP_GB="${SWAP_GB:-2}"

log() { printf '\n==> %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo bash $0)"; exit 1; }

log "System packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get upgrade -yq
apt-get install -yq --no-install-recommends \
    build-essential cmake ninja-build git pkg-config \
    python3 python3-venv python3-dev \
    libcap-dev libsystemd-dev libseccomp-dev \
    fail2ban unattended-upgrades curl rsync ca-certificates
systemctl enable --now unattended-upgrades fail2ban

log "Swap (${SWAP_GB} GB): a safety net for the OS and the app; sandboxed runs get none"
if ! swapon --show | grep -q /swapfile; then
    fallocate -l "${SWAP_GB}G" /swapfile
    chmod 600 /swapfile
    mkswap /swapfile
    swapon /swapfile
    grep -q '^/swapfile ' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi
sysctl -q vm.swappiness=10
echo 'vm.swappiness=10' > /etc/sysctl.d/90-me-swap.conf

log "isolate ${ISOLATE_REF} (the IOI sandbox: namespaces + cgroup v2)"
if [ "$(cat /usr/local/etc/isolate.ref 2>/dev/null)" != "${ISOLATE_REF}" ]; then
    rm -rf /usr/local/src/isolate
    git clone --depth 1 --branch "${ISOLATE_REF}" https://github.com/ioi/isolate.git /usr/local/src/isolate
    # `install` builds the programs, default.cf and the systemd units (not the manpages,
    # which would need asciidoc) and installs the units under /usr/local/lib/systemd.
    make -C /usr/local/src/isolate install
    echo "${ISOLATE_REF}" > /usr/local/etc/isolate.ref
fi
# Sandboxes run as UIDs/GIDs taken from the subordinate ID range of the `isolate`
# user (default.cf: subid_user = isolate); 1,000 IDs is 1,000 possible boxes.
if ! id isolate >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin isolate
fi
grep -q '^isolate:' /etc/subuid || usermod --add-subuids 2000000-2000999 isolate
grep -q '^isolate:' /etc/subgid || usermod --add-subgids 2000000-2000999 isolate
systemctl daemon-reload
# isolate.service runs isolate-cg-keeper, which owns the cgroup subtree the boxes live in.
systemctl enable --now isolate.service

log "App user and directories"
# me-web runs the UI and starts the isolate boxes; no shell, no SSH.
id me-web >/dev/null 2>&1 || useradd --system --home-dir /var/lib/me-web --shell /usr/sbin/nologin me-web
install -d -o root -g root -m 755 /opt/me /srv/me
install -d -o root -g me-web -m 750 /srv/me/data   # Parquet files: readable by the app, never writable
if [ -f "$(dirname "$0")/systemd/me-web.service" ]; then
    install -m 644 "$(dirname "$0")/systemd/me-web.service" /etc/systemd/system/me-web.service
    systemctl daemon-reload
    systemctl enable me-web.service   # started once a release is installed (install-from-source.sh)
fi

log "isolate environment check (report only)"
# Without --execute it only reports. Its fixes (no ASLR, no swap, no transparent
# huge pages) are for reproducible contest timings and would weaken this server.
TERM=dumb isolate-check-environment || true
