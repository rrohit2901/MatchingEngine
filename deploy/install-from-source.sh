#!/usr/bin/env bash
# Build a release on the VM from a source tarball and switch to it.
#     sudo bash install-from-source.sh /tmp/me-src.tar.gz <release-id>
# Layout:  /opt/me/releases/<id>/{src,venv}   built in place (a venv can't be moved)
#          /opt/me/current -> releases/<id>     what me-web.service runs
# The previous release stays; the last 3 are kept. Roll back by pointing
# /opt/me/current at an older release and restarting me-web.
# Until the CI deploy exists (docs/ui-plan.md, U4b), this is how a version goes live.
set -euo pipefail
TARBALL="$1"
ID="${2:-$(date -u +%Y%m%d%H%M%S)}"
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }

REL="/opt/me/releases/$ID"
rm -rf "$REL"
mkdir -p "$REL/src"
tar -xzf "$TARBALL" -C "$REL/src"

python3 -m venv "$REL/venv"
"$REL/venv/bin/pip" install -q --upgrade pip
# One compile job at a time: the extension builds with LTO, which is memory hungry on 2 GB.
CMAKE_BUILD_PARALLEL_LEVEL=1 "$REL/venv/bin/pip" install -q "$REL/src[web]"
"$REL/venv/bin/python" -c "import matching_engine._core, matching_engine.backtest, streamlit; print('import ok')"
chmod -R a+rX "$REL"   # me-web, and the isolate boxes, read it

previous="$(readlink -f /opt/me/current 2>/dev/null || true)"
ln -sfn "$REL" /opt/me/current.tmp && mv -T /opt/me/current.tmp /opt/me/current
systemctl restart me-web.service
for _ in $(seq 1 30); do
    curl -sf http://127.0.0.1:8501/_stcore/health >/dev/null && break
    sleep 1
done
if ! curl -sf http://127.0.0.1:8501/_stcore/health >/dev/null; then
    echo "new release is not healthy"
    if [ -n "$previous" ] && [ "$previous" != "$REL" ]; then
        echo "rolling back to $previous"
        ln -sfn "$previous" /opt/me/current.tmp && mv -T /opt/me/current.tmp /opt/me/current
        systemctl restart me-web.service
    fi
    exit 1
fi
echo "live: $REL"
ls -1dt /opt/me/releases/* | tail -n +4 | xargs -r rm -rf
