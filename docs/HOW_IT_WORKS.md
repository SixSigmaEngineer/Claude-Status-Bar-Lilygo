# How it works (and the quirks that cost us an evening)

This documents the architecture and every hardware/software gotcha discovered while building this, so the next person doesn't re-derive them the hard way.

## Architecture

Three pieces:

1. **Transcript tailing (PC).** Claude Desktop (Cowork) and Claude Code both write session transcripts as JSONL — one JSON record per message/event, appended live while Claude works. The bridge stats known files every second, reads only appended bytes (remembering per-file offsets), and feeds each line through a per-session state machine.

2. **State derivation.** Records of `type: assistant` carry the model id, token usage (`input_tokens`, `output_tokens`, `cache_read_input_tokens`, `cache_creation_input_tokens`), and content blocks. A `tool_use` block opens a pending tool; the matching `tool_result` (in a later `user` record) closes it. From this we derive: `run` (Clauding…), `tool` (name shown), `wait` (pending tool older than ~20s with no file writes = permission prompt; or an assistant message ending in "?" = Claude asked something), `done`, `idle`. Context % = latest `input + cache_read + cache_creation` over the window size. Session names come from `summary` records or the first user message.

3. **Rendering (ESP32-S3).** The bridge sends one compact JSON line per second over USB CDC serial. The firmware draws into a 640×180 canvas (Adafruit GFX) and pushes it to the panel via LilyGo's AXS15231B QSPI driver.

## Where transcripts live

- Claude Code (all platforms): `~/.claude/projects/<cwd-slug>/<uuid>.jsonl` — same layout everywhere; `CLAUDE_CONFIG_DIR` relocates the tree and the bridge honors it.
- Claude Desktop (Cowork) on macOS: `~/Library/Application Support/Claude/local-agent-mode-sessions/...`; on Linux: `~/.config/Claude/local-agent-mode-sessions/...`.

### Windows specifics

- Claude Code: `%USERPROFILE%\.claude\projects\<cwd-slug>\<uuid>.jsonl`
- Claude Desktop (Cowork): **not** where you'd expect. The app is MSIX-packaged, so its writes to `%APPDATA%\Claude\...` are silently virtualized to:
  `%LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\Claude\local-agent-mode-sessions\<workspace>\<user>\local_<session>\.claude\projects\<slug>\<uuid>.jsonl`
- The transcripts sit inside a hidden `.claude` folder — Python's `glob("**")` does not descend into dot-directories, so the bridge uses `os.walk`.
- Each Cowork session dir also has an `audit.jsonl` (encrypted — note the `.audit-key` beside it) and `subagents/agent-*.jsonl` files (helper agents; filtered out).

## Usage limits

With a Claude Code login present (`~\.claude\.credentials.json`), the bridge calls `https://api.anthropic.com/api/oauth/usage` (Bearer token + `anthropic-beta: oauth-2025-04-20`) and gets real 5h/7d utilization and reset times, cached 60s. Without it, it estimates by summing transcript tokens in rolling windows against configurable caps.

## Hardware quirks of the LilyGo T-Display S3 Long

Hard-won knowledge, in the order we hit it:

1. **The backlight is not turned on by the display init.** `axs15231_init()` leaves `TFT_BL` (GPIO1) alone; drive it HIGH yourself or the screen stays black.

2. **The panel is addressed portrait: 180 wide × 640 tall.** Even though you use it landscape. Address windows with x > 179 produce garbage. Render landscape, then transpose in software (the factory firmware does the same via LVGL's `sw_rotate`).

3. **RGB565 bytes must be swapped** (LilyGo's `lv_conf.h` sets `LV_COLOR_16_SWAP 1`). Big-endian on the wire.

4. **`lcd_PushColors()` is asynchronous and queues at most 3 DMA chunks per call.** For a full frame you must keep calling `lcd_PushColors(0,0,0,0,NULL)` to pump the remaining chunks until the driver's `transfer_num` and `lcd_PushColors_len` both hit zero — exactly like LilyGo's TFT example. If you don't, you get a partial frame and a stuck panel.

5. **GPIO16 is BOTH the LCD reset and the touch reset.** Pulse it to "reset the touch controller" and you kill the display until the next `axs15231_init()`. (LilyGo's own `boardSleep()` exploits this to power the touch down.)

6. **The SY6970 battery charger (I2C 0x6A) needs configuring when running battery-less** — disable the ILIM pin (`reg 0x00 = 0x3F`) and turn off the BATFET (`reg 0x09 = 0x64`), per LilyGo's 2025 GFX example.

7. **Touch, the saga.** The documented protocol (I2C 0x3B, 11-byte command `B5 AB A5 5A ... 08 ...`, 8-byte reply) simply never ACKs on some board revisions — including while touched. What *does* work: the touch INT line (GPIO11) reliably fires edges when a finger lands. But the controller then keeps pulsing INT at ~2Hz indefinitely (unread-data begging), so neither the line level nor pulse duration measures how long a finger stayed down. The firmware therefore treats "INT pulse after a quiet period" as a single tap gesture and re-arms after ~1.2s of quiet. Hence: tap = switch page (only touch gesture), and the BOOT button covers session-cycling (short press) and display-flip (long press). If your unit's 0x3B does ACK, the full gesture set could be restored — see git history for the protocol code.

8. **ESP32 + Windows practicalities:** the toolchain must live in a short path (`C:\ClaudeBarBuild`) or compiler command lines exceed Windows' 32K limit; PowerShell 5 chokes on non-ASCII in .ps1 files saved without BOM; the ESP32-S3's native USB serial needs `CDCOnBoot=cdc`; and the default 256-byte CDC RX buffer overflows during 50ms screen redraws, silently corrupting inbound JSON (`Serial.setRxBufferSize(16384)` first).

## Serial protocol

PC → device, newline-delimited JSON:

```jsonc
{"t":"s",                     // status packet, 1/sec
 "ses":[{"nm":"name","md":"Fable 5","st":"tool","tl":"Bash","ef":"medium",
          "el":74,"ti":56500,"to":4700,"cx":6,"at":false}],
 "act":0,
 "us":{"p5":28,"p7":52,"r5":"36m","r7":"5d10h","est":false}}

{"t":"lg","off":0,"px":"<hex>","last":false}   // logo chunks (48x48 RGB565 LE)
{"t":"lgclr"}                                   // clear logo
{"t":"ping"}                                    // keepalive
```

Device → PC: debug lines (`[boot]`, `[beat]`, `[touch]`, `[rx]`), echoed by the bridge console for troubleshooting.
