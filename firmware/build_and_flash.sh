#!/usr/bin/env bash
# ============================================================
# Claude Status Bar - one-click build & flash  (macOS / Linux)
# for LilyGo T-Display S3 Long (ESP32-S3)
#
# Usage:  ./build_and_flash.sh [--port /dev/cu.usbmodemXXXX] [--compile-only]
#
# What it does (all self-contained in ~/ClaudeBarBuild, nothing system-wide):
#   1. Downloads arduino-cli for your OS/arch
#   2. Installs the ESP32 board package (pinned 2.0.17 - big download, first run only)
#   3. Downloads LilyGo's official T-Display-S3-Long repo and copies the
#      AXS15231B display driver + pin config into the sketch
#   4. Installs LVGL 8.3.11 + Adafruit GFX + ArduinoJson libraries
#   5. Compiles the firmware
#   6. Flashes it to the device over USB
#
# Windows users: use build_and_flash.ps1 instead.
# ============================================================
set -euo pipefail

PORT=""
COMPILE_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-p)      PORT="$2"; shift 2 ;;
        --compile-only) COMPILE_ONLY=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$ROOT/claude_statusbar"
BUILD="$HOME/ClaudeBarBuild"
CLI="$BUILD/arduino-cli"
CFG="$BUILD/arduino-cli.yaml"
OUT="$BUILD/out"
mkdir -p "$BUILD"

step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

# ---------- 0. platform sanity ----------
OS="$(uname -s)"
ARCH="$(uname -m)"
case "$OS-$ARCH" in
    Darwin-arm64)          CLI_PKG="macOS_ARM64" ;;
    Darwin-x86_64)         CLI_PKG="macOS_64bit" ;;
    Linux-x86_64)          CLI_PKG="Linux_64bit" ;;
    Linux-aarch64|Linux-arm64) CLI_PKG="Linux_ARM64" ;;
    *) die "Unsupported platform: $OS $ARCH" ;;
esac

if [[ "$OS" == "Linux" ]]; then
    # serial-port permissions: user must be in the dialout group
    if ! id -nG | grep -qw dialout; then
        echo "NOTE: you are not in the 'dialout' group; flashing will fail with"
        echo "      'permission denied' on the serial port. Fix once with:"
        echo "        sudo usermod -aG dialout \$USER   # then log out and back in"
    fi
    # brltty (a screen-reader daemon preinstalled on some Ubuntu versions)
    # can grab USB-serial devices the moment they enumerate
    if command -v brltty >/dev/null 2>&1; then
        echo "NOTE: 'brltty' is installed and may steal the serial port."
        echo "      If the device vanishes on plug-in: sudo apt remove brltty"
    fi
fi

# ---------- 1. arduino-cli ----------
if [[ ! -x "$CLI" ]]; then
    step "Downloading arduino-cli ($CLI_PKG)..."
    curl -fsSL "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_${CLI_PKG}.tar.gz" \
        -o "$BUILD/arduino-cli.tar.gz"
    tar -xzf "$BUILD/arduino-cli.tar.gz" -C "$BUILD" arduino-cli
    rm -f "$BUILD/arduino-cli.tar.gz"
fi

# isolated config so we don't touch any existing Arduino setup
cat > "$CFG" <<EOF
board_manager:
  additional_urls:
    - https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
directories:
  data: $BUILD/data
  downloads: $BUILD/downloads
  user: $BUILD/user
EOF

# ---------- 2. ESP32 core ----------
step "Updating board index..."
"$CLI" --config-file "$CFG" core update-index
if ! "$CLI" --config-file "$CFG" core list 2>/dev/null | grep -q "esp32:esp32"; then
    step "Installing ESP32 board package 2.0.17 (about 1.5 GB, first run only - grab a coffee)..."
    ok=0
    for attempt in 1 2 3; do
        if "$CLI" --config-file "$CFG" core install esp32:esp32@2.0.17; then
            ok=1; break
        fi
        echo "  download hiccup (attempt $attempt/3), retrying in 5s..."
        sleep 5
    done
    [[ $ok == 1 ]] || die "ESP32 core install failed after 3 attempts"
fi

# ---------- 3. LilyGo display driver ----------
if [[ ! -f "$SKETCH/AXS15231B.h" ]]; then
    step "Downloading LilyGo T-Display-S3-Long repo (official display driver)..."
    REPO_ZIP="$BUILD/lilygo.zip"
    ok=0
    for branch in master main; do
        if curl -fsSL "https://github.com/Xinyuan-LilyGO/T-Display-S3-Long/archive/refs/heads/$branch.zip" -o "$REPO_ZIP"; then
            ok=1; break
        fi
        echo "  branch '$branch' not found, trying next..."
    done
    [[ $ok == 1 ]] || die "Could not download the LilyGo repo from GitHub. Check your internet connection, or tell Claude."
    REPO_DIR="$BUILD/lilygo"
    rm -rf "$REPO_DIR"; mkdir -p "$REPO_DIR"
    # python, not unzip: the repo zip contains non-ASCII filenames that make
    # macOS unzip prompt interactively / error out
    python3 - "$REPO_ZIP" "$REPO_DIR" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    z.extractall(sys.argv[2])
PYEOF
    rm -f "$REPO_ZIP"

    DRV_CPP="$(find "$REPO_DIR" -name AXS15231B.cpp | head -1)"
    [[ -n "$DRV_CPP" ]] || die "Could not find AXS15231B.cpp in the LilyGo repo - repo layout may have changed. Tell Claude!"
    DRV_DIR="$(dirname "$DRV_CPP")"
    step "Copying driver files from $DRV_DIR"
    cp "$DRV_DIR/AXS15231B.cpp" "$DRV_DIR/AXS15231B.h" "$SKETCH/"
    PINS="$(find "$REPO_DIR" -name pins_config.h | head -1)"
    [[ -n "$PINS" ]] && cp "$PINS" "$SKETCH/"
    # copy any other headers the driver depends on from the same folder
    find "$DRV_DIR" -maxdepth 1 -name '*.h' ! -name 'AXS15231B.h' -exec cp {} "$SKETCH/" \;
fi

# ---------- 3a. CST3530 touch firmware blob (new hw revision) ----------
# Newer boards (~May 2026 on) have a Hynitron CST3530 touch chip that needs
# its firmware verified/uploaded by the host. The blob comes from LilyGo's
# cst3530 branch. Optional: without it, old-revision boards and CST boards
# with healthy firmware still work.
if [[ ! -f "$SKETCH/cst3530_fw.h" ]]; then
    step "Downloading CST3530 touch firmware blob..."
    curl -fsSL "https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Display-S3-Long/T-Display-S3-Long-cst3530/lib/hyn_driver_for_esp32/hyn_chips/cst3530_fw.h" \
        -o "$SKETCH/cst3530_fw.h" \
        || echo "  (download failed - continuing without touch-fw upload support)"
fi

# ---------- 3b. LVGL (the LilyGo driver #includes it) ----------
if [[ ! -d "$BUILD/user/libraries/lvgl" ]]; then
    step "Installing LVGL 8.3.11 (required by LilyGo's display driver)..."
    "$CLI" --config-file "$CFG" lib install "lvgl@8.3.11"
fi
LV_CONF="$BUILD/user/libraries/lv_conf.h"
if [[ ! -f "$LV_CONF" ]]; then
    step "Setting up lv_conf.h..."
    SRC="$(find "$BUILD/lilygo" -name lv_conf.h 2>/dev/null | head -1 || true)"
    if [[ -n "$SRC" ]]; then
        cp "$SRC" "$LV_CONF"
    else
        sed 's/#if 0/#if 1/' "$BUILD/user/libraries/lvgl/lv_conf_template.h" > "$LV_CONF"
    fi
fi

# ---------- 4. libraries ----------
step "Installing libraries (Adafruit GFX, ArduinoJson)..."
"$CLI" --config-file "$CFG" lib install "Adafruit GFX Library" "Adafruit BusIO" "ArduinoJson"

# ---------- 5. compile ----------
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=115200"
step "Compiling ($FQBN)..."
if ! "$CLI" --config-file "$CFG" compile --fqbn "$FQBN" --output-dir "$OUT" "$SKETCH"; then
    die $'\nCOMPILE FAILED. Copy the error above and paste it to Claude to fix.'
fi
printf '\033[32mCompiled OK. Binaries in %s\033[0m\n' "$OUT"
[[ $COMPILE_ONLY == 1 ]] && exit 0

# ---------- 6. flash ----------
if [[ -z "$PORT" ]]; then
    step "Looking for the device..."
    "$CLI" --config-file "$CFG" board list || true
    if [[ "$OS" == "Darwin" ]]; then
        # prefer the cu.* device (tty.* blocks on open waiting for carrier)
        PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
    else
        PORT="$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)"
        [[ -z "$PORT" ]] && PORT="$(ls /dev/ttyUSB* 2>/dev/null | head -1 || true)"
    fi
    if [[ -z "$PORT" ]]; then
        echo "No serial port found. Plug in the display via USB-C."
        echo "If it still doesn't show up: hold the BOOT button, plug in USB, release BOOT."
        read -rp "Enter port manually (e.g. /dev/ttyACM0): " PORT
    fi
fi
step "Flashing to $PORT ..."
if ! "$CLI" --config-file "$CFG" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"; then
    die $'\nUPLOAD FAILED. Try: hold BOOT, replug USB while holding it, release BOOT, run again.'
fi
printf '\n\033[32mDONE! Unplug and replug the USB cable. The display should show "Claude Status Bar".\033[0m\n'
printf '\033[32mNext: run the bridge  ->  bridge/run_bridge.sh\033[0m\n'
