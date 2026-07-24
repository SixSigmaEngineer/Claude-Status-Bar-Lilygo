#!/usr/bin/env bash
# Headless console bridge (macOS / Linux). Sets up a local venv on first run.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$HERE/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
    echo "Setting up Python environment (first run)..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install pyserial
fi
exec "$VENV/bin/python" -u "$HERE/claude_bar_bridge.py" "$@"
