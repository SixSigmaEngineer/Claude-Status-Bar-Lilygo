#!/usr/bin/env bash
# Desktop app: bridge + live preview + logo uploader (macOS / Linux).
# Sets up a local venv on first run. Tkinter must come from the system
# Python (macOS: brew install python-tk; Ubuntu: sudo apt install python3-tk).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$HERE/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
    echo "Setting up Python environment (first run)..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install pyserial
fi
if ! "$VENV/bin/python" -c "import PIL, pystray" 2>/dev/null; then
    "$VENV/bin/pip" -q install pillow pystray
fi
if ! "$VENV/bin/python" -c "import tkinter" 2>/dev/null; then
    echo "tkinter is missing."
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "  Fix: brew install python-tk  (then delete $VENV and rerun)"
    else
        echo "  Fix: sudo apt install python3-tk  (then delete $VENV and rerun)"
    fi
    exit 1
fi
exec "$VENV/bin/python" "$HERE/claude_bar_app.py" "$@"
