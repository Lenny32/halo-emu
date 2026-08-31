#!/usr/bin/env bash
# run-ui.sh — launch the emulator behind the web A/V bridge (ticket 0040):
# desktop UI + phone page on https://<host>:9564, emulator spawned headless.
#
#   ./run-ui.sh 0.8.8.bin                    then open https://127.0.0.1:9564/
#   ./run-ui.sh 0.8.8.bin --public-ip <ip>   extra args go to av_bridge.py
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <firmware.bin> [av_bridge args...]" >&2
    exit 2
fi

fw=$1
shift
here=$(dirname "$0")

# The bridge is a uv script (deps in its PEP 723 header). Prefer uv —
# also checking ~/.local/bin, where its installer puts it — and fall
# back to plain python3 for environments that installed the deps.
if command -v uv >/dev/null 2>&1; then
    exec uv run --script "$here/tools/av_bridge.py" -f "$fw" "$@"
elif [ -x "$HOME/.local/bin/uv" ]; then
    exec "$HOME/.local/bin/uv" run --script "$here/tools/av_bridge.py" \
        -f "$fw" "$@"
else
    exec python3 "$here/tools/av_bridge.py" -f "$fw" "$@"
fi
