#!/usr/bin/env bash
# One-time setup of the backtester VM (Ubuntu 24.04, ARM or x86). Run as root:
#     sudo bash provision.sh
# Safe to re-run: every step checks before it changes anything.
# See docs/ui-plan.md (U4) and docs/deploy.md.
set -euo pipefail

ISOLATE_REF="${ISOLATE_REF:-v2.7}"     # https://github.com/ioi/isolate
# Public name for HTTPS, e.g. 52-65-150-242.sslip.io. Empty: no Caddy (SSH tunnel only).
SITE_ADDRESS="${SITE_ADDRESS:-}"
# Public key for CI deploys (one line). Empty: leave me-deploy's key as it is.
DEPLOY_PUBKEY="${DEPLOY_PUBKEY:-}"
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
    fail2ban unattended-upgrades curl rsync ca-certificates nftables
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
install -d -o root -g root -m 755 /opt/me
# /srv/me is closed to other local users; inside it, the data is world-readable so
# that isolate boxes (which run under their own UIDs, with the data bind-mounted
# read-only at /data) can read it. Nothing under it is writable except by root.
install -d -o root -g me-web -m 750 /srv/me
install -d -o root -g root -m 755 /srv/me/data
find /srv/me/data -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
if [ -f "$(dirname "$0")/systemd/me-web.service" ]; then
    install -m 644 "$(dirname "$0")/systemd/me-web.service" /etc/systemd/system/me-web.service
    systemctl daemon-reload
    systemctl enable me-web.service   # started once a release is installed (install-release.sh)
fi

log "Release installer and the CI deploy user"
HERE="$(dirname "$0")"
install -m 755 "$HERE/install-release.sh" /usr/local/sbin/me-install-release
install -m 755 "$HERE/me-deploy-receive" /usr/local/sbin/me-deploy-receive
install -m 755 "$HERE/me-deploy-entry" /usr/local/bin/me-deploy-entry
id me-deploy >/dev/null 2>&1 || useradd --create-home --shell /bin/sh me-deploy
passwd -l me-deploy >/dev/null   # no password: key only
# One sudo rule, checked before it is installed (a broken sudoers file locks out sudo).
visudo -cf "$HERE/sudoers.me-deploy"
install -m 440 "$HERE/sudoers.me-deploy" /etc/sudoers.d/me-deploy
if [ -n "$DEPLOY_PUBKEY" ]; then
    install -d -o me-deploy -g me-deploy -m 700 /home/me-deploy/.ssh
    # restrict: no pty, no forwarding; command=: the key can only run me-deploy-entry.
    printf 'restrict,command="/usr/local/bin/me-deploy-entry" %s\n' "$DEPLOY_PUBKEY" \
        > /home/me-deploy/.ssh/authorized_keys
    chown me-deploy:me-deploy /home/me-deploy/.ssh/authorized_keys
    chmod 600 /home/me-deploy/.ssh/authorized_keys
fi
# SSH hardening, checked with sshd -t before it takes effect.
install -m 644 "$HERE/sshd.me.conf" /etc/ssh/sshd_config.d/90-me.conf
if sshd -t; then
    systemctl reload ssh || systemctl restart ssh
else
    rm -f /etc/ssh/sshd_config.d/90-me.conf
    echo "sshd config rejected; removed it" >&2
    exit 1
fi

log "Per-IP connection limits on the web ports"
install -d -m 755 /etc/me
nft -c -f "$HERE/ratelimit.nft"   # check before loading
install -m 644 "$HERE/ratelimit.nft" /etc/me/ratelimit.nft
install -m 644 "$HERE/systemd/me-ratelimit.service" /etc/systemd/system/me-ratelimit.service
systemctl daemon-reload
systemctl enable me-ratelimit.service
systemctl restart me-ratelimit.service

if [ -n "$SITE_ADDRESS" ]; then
    log "Caddy: HTTPS for ${SITE_ADDRESS}"
    if ! command -v caddy >/dev/null; then
        apt-get install -yq debian-keyring debian-archive-keyring apt-transport-https gnupg
        curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
            | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
        curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt \
            > /etc/apt/sources.list.d/caddy-stable.list
        apt-get update -q
        apt-get install -yq caddy
    fi
    sed "s|^SITE_ADDRESS {|${SITE_ADDRESS} {|" "$(dirname "$0")/Caddyfile" > /etc/caddy/Caddyfile
    caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
    # validate runs as root and opens the access log; the service runs as caddy.
    install -d -o caddy -g caddy -m 750 /var/log/caddy
    chown -R caddy:caddy /var/log/caddy
    systemctl enable caddy
    systemctl reload caddy || systemctl restart caddy
fi

log "isolate environment check (report only)"
# Without --execute it only reports. Its fixes (no ASLR, no swap, no transparent
# huge pages) are for reproducible contest timings and would weaken this server.
TERM=dumb isolate-check-environment || true
