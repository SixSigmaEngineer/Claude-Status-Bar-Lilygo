#!/usr/bin/env bash
# Command-line logo uploader (macOS / Linux): ./set_logo.sh path/to/image.png
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$HERE/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
    echo "Setting up Python environment (first run)..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install pyserial
fi
"$VENV/bin/python" -c "import PIL" 2>/dev/null || "$VENV/bin/pip" -q install pillow
exec "$VENV/bin/python" "$HERE/set_logo.py" "$@"
