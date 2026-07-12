# Claude Status Bar

![License](https://img.shields.io/badge/License-MIT-green)
![Python](https://img.shields.io/badge/Python-3.10--3.13-blue)
![Platform](https://img.shields.io/badge/Platform-Windows-blue)
![ESP32-S3](https://img.shields.io/badge/MCU-ESP32--S3-orange)

![clauding](https://img.shields.io/badge/clauding-24%2F7-blueviolet)
![firmware flashes](https://img.shields.io/badge/firmware_flashes_to_get_here-17-red)
![touch protocol](https://img.shields.io/badge/touch_protocol-defeated_by_interrupt-yellow)
![reset pins](https://img.shields.io/badge/reset_pins-shared_(pain)-orange)
![context](https://img.shields.io/badge/context-100%25_(send_help)-critical)
![duck lamp](https://img.shields.io/badge/duck_lamp-not_included-lightgrey)

A tiny desk display that shows what Claude is doing, live: current model, the tool it's running ("Bash", "Read", "Percolating…"), elapsed time, tokens in/out, context usage, and your 5-hour / 7-day rate-limit bars. Works with **Claude Desktop (Cowork)** and **Claude Code**, up to 4 sessions at once.

![Live session status](docs/images/LilyGo1.png)

![Usage limits page](docs/images/LilyGo2.png)

Mounts wherever you like - desk stand or perched on top of your monitor. One USB-C cable to your PC is both power and data:

![Mounted on a laptop screen](docs/images/Lilygo3.png)

## Hardware

| Part | Notes |
|---|---|
| [LilyGo T-Display S3 Long](https://lilygo.cc/products/t-display-s3-long) | ESP32-S3, 3.4" 640×180 LCD (AXS15231B), capacitive touch, USB-C. ~$25 on AliExpress. |
| USB-C **data** cable | Powers the display and carries the status feed. That's the whole BOM. |

No battery needed — it lives plugged into your PC. (The board has a JST battery connector + charger if you ever want one, but this project streams over USB anyway.)

## How it works

```
Claude Desktop / Claude Code
        │  writes JSONL transcripts on disk
        ▼
bridge/claude_bar_bridge.py   (Python, runs on your PC)
        │  tails transcripts → derives per-session state
        │  + polls Anthropic usage API (real limits) or estimates locally
        ▼  JSON lines over USB serial @ 115200
firmware/claude_statusbar.ino  (ESP32-S3)
        │  renders 640×180 UI, touch + button input
        ▼
your eyeballs
```

The bridge watches these locations (auto-detected, including the MSIX-virtualized path Claude Desktop actually writes to on Windows):

- `%USERPROFILE%\.claude\projects\**\*.jsonl` — Claude Code
- `%LOCALAPPDATA%\Packages\Claude_*\LocalCache\Roaming\Claude\local-agent-mode-sessions\**\.claude\projects\**\*.jsonl` — Claude Desktop / Cowork

Per-session state machine: `run` (Clauding…) → `tool` (shows tool name) → `wait` (orange "Waiting on you" — pending permission or Claude asked a question) → `done` / `idle`. Sessions auto-follow the most recently active one, but jump to any session that's waiting on you, with an orange alert banner.

## Setup

**Prereq:** Python 3.10+ ([python.org](https://python.org), check "Add to PATH").

1. **Flash the firmware** — plug the display in via USB-C, then run in PowerShell from `firmware\`:
   ```
   powershell -ExecutionPolicy Bypass -File .\build_and_flash.ps1
   ```
   Fully self-contained: downloads arduino-cli, the ESP32 toolchain (~1.5 GB, first run only), LilyGo's official display driver, and all libraries into `C:\ClaudeBarBuild`, then compiles and flashes. If the wrong COM port is picked, add `-Port COM5`. If upload fails: hold **BOOT** while plugging in USB, release, retry.

2. **Test:** `bridge\run_bridge.bat --demo` — fake data, verifies the whole pipeline.

3. **Run the app:** `bridge\run_app.bat` — a small desktop app that runs the bridge with a **live preview of the display**, a **logo uploader** (pick any JPG/PNG; it's converted to 48×48, streamed to the device, and saved in its flash — try the 15 ready-made icons in `logos\starter-pack`), **minimize-to-system-tray**, and a **Start with Windows** checkbox. Prefer headless? `run_bridge.bat` runs the bare console bridge and `install_autostart.bat` sets it to start hidden at login; `set_logo.bat` uploads a logo from the command line.

## Customize the badge

The image in the top-left corner of the screen is yours to change: open the app → **Set logo…** → pick any JPG/PNG (your company logo, avatar, pet, whatever). It's converted to 48×48, sent to the display, and saved in the device's flash so it survives reboots. A starter pack of 15 ready-made icons ships in `logos/starter-pack` — sparkles, terminal, robot, space invader, coffee, and friends. `Clear logo` in the app removes it.

## Controls

| Action | Result |
|---|---|
| Tap screen | Switch page (status ↔ usage) |
| BOOT button, short press | Cycle session (A/B/C/D) |
| BOOT button, hold 1s | Flip display 180° (saved) |

After a tap the screen needs a couple of seconds before the next tap registers — see [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) for why (touch hardware quirk).

## Usage limits page

If you're logged into Claude Code, the bridge reads your **real** 5-hour/7-day utilization and reset times from Anthropic's usage API (refreshed every 60s). Otherwise it shows a local estimate from transcript token counts — tune `est_cap_5h_tokens` / `est_cap_7d_tokens` in `bridge\config.json` (copy from `config.example.json`).

## Configuration

Copy `bridge\config.example.json` → `bridge\config.json`. Everything is optional: serial port override, context window size (set `1000000` for 1M-context plans), idle/wait timing thresholds, usage caps, extra transcript roots.

## Troubleshooting

| Symptom | Fix |
|---|---|
| "Bridge offline" on display | Bridge not running, or wrong port: `run_bridge.bat --port COM5` |
| "No sessions" | `run_bridge.bat --scan` shows which transcripts were found |
| Blue/orange colors swapped | Set `SWAP_BYTES 0` in the .ino, re-run build script |
| Display upside down | Hold BOOT ~1s |
| Flash fails | Hold BOOT while plugging USB, release, retry |
| Compile error | Open an issue with the error text |

## Project structure

```
firmware/
  build_and_flash.ps1        one-click toolchain + build + flash (Windows)
  claude_statusbar/          Arduino sketch (driver files auto-copied by script)
bridge/
  claude_bar_app.py          desktop app: bridge + live preview + tray + logo UI
  claude_bar_bridge.py       core bridge (transcript tailer + serial feeder)
  set_logo.py / .bat         command-line logo uploader (48x48 RGB565)
  run_app.bat                launch the desktop app
  run_bridge.bat             headless console bridge
  install_autostart.bat      headless autostart at login
  config.example.json
docs/
  HOW_IT_WORKS.md            architecture + hardware quirks (worth reading!)
```

## License

MIT — see [LICENSE](LICENSE).
