#!/usr/bin/env bash
# Install a release and switch the live site to it, as root:
#     install-release.sh <release-id> <source.tar.gz> [<wheel>]
# With a wheel (from CI), it is installed as is; without one, the C++ extension is
# built here from the source (slow on a t4g.small, and memory hungry).
#
#   /opt/me/releases/<id>/{src,venv}   built in place (a venv can't be moved)
#   /opt/me/current -> releases/<id>     what me-web.service runs
#
# Before the switch, deploy/sandbox_check.py runs against the NEW release (as
# me-web, through isolate, a real backtest plus hostile strategies), so a broken
# build never goes live. After the switch, the site must answer its health check,
# or the previous release is restored. The last 3 releases are kept; roll back by
# hand with:  ln -sfn /opt/me/releases/<id> /opt/me/current && systemctl restart me-web
#
# Installed by provision.sh as /usr/local/sbin/me-install-release. Deploys from
# CI run that installed copy, never one from the bundle they upload.
set -euo pipefail
ID="$1"; SOURCE="$2"; WHEEL="${3:-}"
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }
[[ "$ID" =~ ^[0-9A-Za-z._-]{1,64}$ ]] || { echo "bad release id"; exit 2; }

exec 9>/run/me-install-release.lock
flock -w 600 9 || { echo "another install is running"; exit 1; }

REL="/opt/me/releases/$ID"
current="$(readlink -f /opt/me/current 2>/dev/null || true)"
if [ "$current" = "$REL" ]; then
    echo "release $ID is already live"
    exit 0
fi
rm -rf "$REL"
mkdir -p "$REL/src"
tar -xzf "$SOURCE" -C "$REL/src" --no-same-owner

echo "==> virtualenv"
python3 -m venv "$REL/venv"
"$REL/venv/bin/pip" install -q --upgrade pip
if [ -n "$WHEEL" ]; then
    "$REL/venv/bin/pip" install -q "${WHEEL}[web]"
else
    # One compile job at a time: the extension builds with LTO, which is memory hungry on 2 GB.
    CMAKE_BUILD_PARALLEL_LEVEL=1 "$REL/venv/bin/pip" install -q "$REL/src[web]"
fi
"$REL/venv/bin/python" -c "import matching_engine._core, matching_engine.backtest, streamlit; print('import ok')"
chmod -R a+rX "$REL"   # me-web, and the isolate boxes, read it

echo "==> sandbox check against the new release"
memory="$(systemctl show me-web.service -p Environment --value | tr ' ' '\n' | sed -n 's/^ME_MEMORY_MB=//p')"
if ! (cd /tmp && runuser -u me-web -- env ME_DATA_DIR=/srv/me/data ME_MEMORY_MB="${memory:-1024}" \
        "$REL/venv/bin/python" "$REL/src/deploy/sandbox_check.py"); then
    echo "sandbox check failed: $ID not switched in (still live: ${current:-none})"
    rm -rf "$REL"
    exit 1
fi

echo "==> switch"
# The service definition ships with the release; pick up changes to it.
if ! cmp -s "$REL/src/deploy/systemd/me-web.service" /etc/systemd/system/me-web.service; then
    install -m 644 "$REL/src/deploy/systemd/me-web.service" /etc/systemd/system/me-web.service
    systemctl daemon-reload
fi
ln -sfn "$REL" /opt/me/current.tmp && mv -T /opt/me/current.tmp /opt/me/current
systemctl restart me-web.service
for _ in $(seq 1 30); do
    curl -sf http://127.0.0.1:8501/_stcore/health >/dev/null && break
    sleep 1
done
if ! curl -sf http://127.0.0.1:8501/_stcore/health >/dev/null; then
    echo "new release is not healthy"
    if [ -n "$current" ] && [ -d "$current" ]; then
        echo "rolling back to $current"
        install -m 644 "$current/src/deploy/systemd/me-web.service" /etc/systemd/system/me-web.service
        systemctl daemon-reload
        ln -sfn "$current" /opt/me/current.tmp && mv -T /opt/me/current.tmp /opt/me/current
        systemctl restart me-web.service
    fi
    exit 1
fi
echo "live: $REL"
ls -1dt /opt/me/releases/* | tail -n +4 | xargs -r rm -rf
