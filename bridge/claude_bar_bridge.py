#!/usr/bin/env python3
"""
Claude Status Bar bridge — PC side.

Tails Claude session transcripts (Claude Desktop / Cowork and Claude Code)
and streams live status to a LilyGo T-Display S3 Long over USB serial as
newline-delimited JSON.

Usage:
    python claude_bar_bridge.py              # normal operation
    python claude_bar_bridge.py --scan       # show which transcript files were found
    python claude_bar_bridge.py --demo       # fake data, test the display without Claude
    python claude_bar_bridge.py --no-serial  # print packets to console instead of serial
    python claude_bar_bridge.py --port COM5  # force a serial port

Requires: pip install pyserial
"""

import argparse
import glob
import json
import os
import platform
import subprocess
import sys
import time
from collections import deque
from datetime import datetime, timezone

# ---------------------------------------------------------------- config

IS_WINDOWS = platform.system() == "Windows"
IS_MAC = platform.system() == "Darwin"

APPDATA = os.environ.get("APPDATA", os.path.expanduser("~\\AppData\\Roaming"))
LOCALAPPDATA = os.environ.get("LOCALAPPDATA", os.path.expanduser("~\\AppData\\Local"))
HOME = os.path.expanduser("~")

# CLAUDE_CONFIG_DIR relocates the whole ~/.claude tree (all platforms)
CLAUDE_DIR = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(HOME, ".claude")


def default_roots():
    roots = [os.path.join(CLAUDE_DIR, "projects")]   # Claude Code, all platforms
    if IS_WINDOWS:
        roots.append(os.path.join(APPDATA, "Claude", "local-agent-mode-sessions"))
        # The MSIX-packaged Claude Desktop app virtualizes its AppData writes to
        # AppData\Local\Packages\Claude_*\LocalCache\Roaming\Claude\...
        pkg_root = os.path.join(LOCALAPPDATA, "Packages")
        if os.path.isdir(pkg_root):
            for name in os.listdir(pkg_root):
                if "claude" in name.lower():
                    roots.append(os.path.join(
                        pkg_root, name, "LocalCache", "Roaming", "Claude",
                        "local-agent-mode-sessions"))
    elif IS_MAC:
        roots.append(os.path.join(HOME, "Library", "Application Support",
                                  "Claude", "local-agent-mode-sessions"))
    else:  # Linux
        roots.append(os.path.join(
            os.environ.get("XDG_CONFIG_HOME", os.path.join(HOME, ".config")),
            "Claude", "local-agent-mode-sessions"))
    return roots


DEFAULT_CONFIG = {
    "roots": default_roots(),
    "port": "",                  # "" = auto-detect (ESP32-S3 native USB)
    "baud": 115200,
    "max_sessions": 8,
    "active_window_min": 30,     # sessions modified within N minutes are shown
    "idle_after_s": 120,         # no file writes for this long -> idle/done
    "wait_tool_s": 20,           # pending tool call older than this -> "waiting on you"
    "context_limit": 200000,
    "done_after_s": 30,              # no new events for this long -> turn is done
    "est_cap_5h_tokens": 8000000,    # only used if OAuth usage API unavailable
    "est_cap_7d_tokens": 60000000,
    "send_interval_s": 1.0,
}


def data_dir():
    """Where config.json / logo.bin live. Next to the scripts normally;
    a per-user app-data dir when running as a packaged exe."""
    if getattr(sys, "frozen", False):
        if IS_WINDOWS:
            d = os.path.join(APPDATA, "ClaudeStatusBar")
        elif IS_MAC:
            d = os.path.join(HOME, "Library", "Application Support", "ClaudeStatusBar")
        else:
            d = os.path.join(
                os.environ.get("XDG_CONFIG_HOME", os.path.join(HOME, ".config")),
                "ClaudeStatusBar")
        os.makedirs(d, exist_ok=True)
        return d
    return os.path.dirname(os.path.abspath(__file__))


def load_config():
    cfg = dict(DEFAULT_CONFIG)
    path = os.path.join(data_dir(), "config.json")
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                cfg.update(json.load(f))
        except Exception as e:
            print(f"[warn] bad config.json ignored: {e}")
    return cfg


# ---------------------------------------------------------------- helpers

def parse_ts(s):
    """ISO timestamp -> unix seconds, tolerant."""
    if not s:
        return None
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


def pretty_model(model_id):
    if not model_id:
        return "Claude"
    m = model_id.lower().replace("claude-", "")
    # drop trailing date stamp like -20251001
    parts = [p for p in m.split("-") if not (len(p) == 8 and p.isdigit())]
    words, version = [], []
    for p in parts:
        if p.isdigit():
            version.append(p)
        else:
            words.append(p.capitalize())
    name = " ".join(words) if words else "Claude"
    if version:
        name += " " + ".".join(version)
    return name[:20]


def pretty_tool(name):
    """'mcp__workspace__bash' -> 'bash'; keep normal names as-is."""
    if not name:
        return name
    if name.startswith("mcp__"):
        name = name.split("__")[-1]
    name = name.replace("_", " ").strip()
    if len(name) > 1 and name == name.lower():
        name = name[0].upper() + name[1:]
    return name[:16]


def fmt_countdown(seconds):
    if seconds is None or seconds < 0:
        return ""
    s = int(seconds)
    if s < 3600:
        return f"{s // 60}m"
    if s < 86400:
        h, m = s // 3600, (s % 3600) // 60
        return f"{h}h{m:02d}m" if h < 10 else f"{h}h"
    d, h = s // 86400, (s % 86400) // 3600
    return f"{d}d{h}h"


# ---------------------------------------------------------------- session

class Session:
    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.name = ""
        self.model = ""
        self.tok_in = 0
        self.tok_out = 0
        self.ctx_tokens = 0
        self.effort = ""
        self.turn_start = None        # ts of last real user message
        self.last_event_ts = None
        self.last_role = ""           # user / assistant
        self.last_assistant_text = ""
        self.pending_tool = ""        # tool name awaiting result
        self.pending_tool_ts = None
        self.pending_ids = {}         # tool_use id -> (name, ts)
        self.first_seen = time.time()

    # ---- incremental parse ----
    def poll(self, usage_events):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return
        if size < self.offset:          # truncated/rotated
            self.offset = 0
        if size == self.offset:
            return
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                if self.offset == 0 and size > 20 * 1024 * 1024:
                    f.seek(size - 5 * 1024 * 1024)
                    f.readline()  # skip partial line
                else:
                    f.seek(self.offset)
                for line in f:
                    self._line(line, usage_events)
                self.offset = f.tell()
        except OSError:
            pass

    def _line(self, line, usage_events):
        line = line.strip()
        if not line or line[0] != "{":
            return
        try:
            rec = json.loads(line)
        except Exception:
            return
        rtype = rec.get("type", "")
        ts = parse_ts(rec.get("timestamp")) or time.time()

        if rtype == "summary":
            s = rec.get("summary", "")
            if s:
                self.name = s[:24]
            return

        if rec.get("isSidechain"):
            return

        msg = rec.get("message") or {}
        content = msg.get("content")
        if isinstance(content, str):
            content = [{"type": "text", "text": content}]
        content = content or []

        if rtype == "user":
            has_tool_result = False
            text = ""
            for b in content:
                if isinstance(b, dict):
                    if b.get("type") == "tool_result":
                        has_tool_result = True
                        self.pending_ids.pop(b.get("tool_use_id", ""), None)
                    elif b.get("type") == "text":
                        text += b.get("text", "")
            if not has_tool_result:
                self.turn_start = ts
                if not self.name and text:
                    self.name = text.strip().replace("\n", " ")[:24]
            self.last_role = "user"
            self.last_event_ts = ts

        elif rtype == "assistant":
            m = msg.get("model") or ""
            if m and not m.startswith("<"):   # skip "<synthetic>" system records
                self.model = m
            usage = msg.get("usage") or {}
            i = usage.get("input_tokens", 0) or 0
            o = usage.get("output_tokens", 0) or 0
            cr = usage.get("cache_read_input_tokens", 0) or 0
            cc = usage.get("cache_creation_input_tokens", 0) or 0
            if i or o or cr or cc:
                self.tok_in += i + cc
                self.tok_out += o
                self.ctx_tokens = i + cr + cc
                usage_events.append((ts, i + cc + o))
            for key in ("effort", "reasoningEffort", "thinkingEffort"):
                if rec.get(key):
                    self.effort = str(rec[key])[:10]
            text = ""
            for b in content:
                if not isinstance(b, dict):
                    continue
                if b.get("type") == "tool_use":
                    self.pending_ids[b.get("id", "")] = (b.get("name", "Tool"), ts)
                elif b.get("type") == "text":
                    text += b.get("text", "")
            if text:
                self.last_assistant_text = text[-400:]
            self.last_role = "assistant"
            self.last_event_ts = ts

    # ---- derived state ----
    def state(self, cfg):
        now = time.time()
        mtime = safe_mtime(self.path)
        fresh = (now - mtime) < cfg["idle_after_s"]

        pending = None
        if self.pending_ids:
            pending = sorted(self.pending_ids.values(), key=lambda v: v[1])[-1]

        if fresh:
            if pending:
                name, pts = pending
                if now - pts > cfg["wait_tool_s"] and now - mtime > cfg["wait_tool_s"]:
                    return "wait", name          # likely a permission prompt
                return "tool", name
            # no pending tools: if the last thing was an assistant message and
            # nothing new has been written for a while, the turn is over
            if self.last_role == "assistant" and now - mtime > cfg["done_after_s"]:
                if self.last_assistant_text.rstrip().endswith("?"):
                    return "wait", ""
                return "done", ""
            return "run", ""
        # stale
        if pending:
            return "wait", pending[0]
        if self.last_assistant_text.rstrip().endswith("?"):
            return "wait", ""
        if self.last_role == "assistant":
            return "done", ""
        return "idle", ""

    def to_packet(self, cfg):
        st, tool = self.state(cfg)
        el = 0
        if self.turn_start:
            end = self.last_event_ts or time.time()
            if st in ("run", "tool"):
                end = time.time()
            el = max(0, int(end - self.turn_start))
        ctx = int(round(100.0 * self.ctx_tokens / cfg["context_limit"]))
        display_name = self.name or os.path.basename(os.path.dirname(self.path))[:24]
        return {
            "nm": display_name,
            "md": pretty_model(self.model),
            "st": st,
            "tl": pretty_tool(tool),
            "ef": self.effort,
            "el": el,
            "ti": self.tok_in,
            "to": self.tok_out,
            "cx": min(ctx, 100),
            "at": st == "wait",
        }


# ---------------------------------------------------------------- discovery

def find_transcripts(roots):
    # os.walk (not glob) — transcripts live inside hidden ".claude" folders,
    # which glob's ** refuses to enter. Skip audit logs (encrypted, not
    # transcripts).
    found = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirs, files in os.walk(root):
            if "subagents" in dirs:
                dirs.remove("subagents")   # helper agents aren't top-level sessions
            for fn in files:
                if fn.endswith(".jsonl") and fn != "audit.jsonl":
                    path = os.path.join(dirpath, fn)
                    try:
                        found[path] = os.path.getmtime(path)
                    except OSError:
                        continue
    return found


# ---------------------------------------------------------------- usage

class UsageTracker:
    """Real numbers from the Claude OAuth usage API when available,
    otherwise a local estimate from transcript token counts."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.events = deque()      # (ts, tokens)
        self.api_cache = None
        self.api_checked = 0

    def add_events(self, evs):
        cutoff = time.time() - 8 * 86400
        for e in evs:
            if e[0] > cutoff:
                self.events.append(e)
        while self.events and self.events[0][0] < cutoff:
            self.events.popleft()

    @staticmethod
    def _oauth_token():
        """Claude Code login token: .credentials.json on Windows/Linux,
        the login Keychain on macOS. The file is checked first on every
        platform since macOS uses it as an override in SSH/tmux setups."""
        cred_path = os.path.join(CLAUDE_DIR, ".credentials.json")
        blob = None
        if os.path.exists(cred_path):
            with open(cred_path, "r", encoding="utf-8") as f:
                blob = json.load(f)
        elif IS_MAC:
            out = subprocess.run(
                ["security", "find-generic-password",
                 "-s", "Claude Code-credentials", "-w"],
                capture_output=True, text=True, timeout=10)
            if out.returncode == 0 and out.stdout.strip():
                blob = json.loads(out.stdout.strip())
        if not blob:
            raise FileNotFoundError("no Claude Code credentials")
        return blob["claudeAiOauth"]["accessToken"]

    def _try_api(self):
        if time.time() - self.api_checked < 60:
            return self.api_cache
        self.api_checked = time.time()
        try:
            token = self._oauth_token()
            import urllib.request
            req = urllib.request.Request(
                "https://api.anthropic.com/api/oauth/usage",
                headers={"Authorization": f"Bearer {token}",
                         "anthropic-beta": "oauth-2025-04-20"})
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read().decode())
            out = {}
            for key, tag in (("five_hour", "5"), ("seven_day", "7")):
                blk = data.get(key) or {}
                util = blk.get("utilization")
                resets = parse_ts(blk.get("resets_at"))
                out["p" + tag] = int(round(util)) if util is not None else -1
                out["r" + tag] = fmt_countdown(resets - time.time()) if resets else ""
            out["est"] = False
            self.api_cache = out
        except Exception:
            self.api_cache = None
        return self.api_cache

    def snapshot(self):
        api = self._try_api()
        if api:
            return api
        now = time.time()
        tok5 = sum(t for ts, t in self.events if ts > now - 5 * 3600)
        tok7 = sum(t for ts, t in self.events)
        first5 = min((ts for ts, _ in self.events if ts > now - 5 * 3600), default=None)
        r5 = fmt_countdown(first5 + 5 * 3600 - now) if first5 else ""
        return {
            "p5": min(100, int(100 * tok5 / self.cfg["est_cap_5h_tokens"])),
            "p7": min(100, int(100 * tok7 / self.cfg["est_cap_7d_tokens"])),
            "r5": r5, "r7": "",
            "est": True,
        }


# ---------------------------------------------------------------- serial

class SerialLink:
    VID_PID = [(0x303A, 0x1001)]   # Espressif native USB

    def __init__(self, port, baud):
        self.port_cfg = port
        self.baud = baud
        self.ser = None

    def _detect(self):
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for p in ports:
            if (p.vid, p.pid) in self.VID_PID:
                return p.device
        # fall back to anything that looks like a USB serial device
        # (avoids grabbing Bluetooth/debug ports on macOS)
        usb = [p for p in ports if any(k in p.device for k in
               ("usbmodem", "usbserial", "ttyACM", "ttyUSB", "COM"))]
        if len(usb) == 1:
            return usb[0].device
        return ports[0].device if len(ports) == 1 else None

    def send(self, text):
        import serial
        if self.ser is None:
            port = self.port_cfg or self._detect()
            if not port:
                return False
            try:
                self.ser = serial.Serial(port, self.baud, timeout=0, write_timeout=2)
                print(f"[serial] connected on {port}")
            except Exception as e:
                print(f"[serial] open failed on {port}: {e}")
                self.ser = None
                return False
        try:
            self.ser.write(text.encode("utf-8"))
            try:                       # echo anything the device says
                n = self.ser.in_waiting
                if n:
                    for ln in self.ser.read(n).decode(errors="replace").splitlines():
                        if ln.strip():
                            print(f"[device] {ln.strip()}")
            except Exception:
                pass
            return True
        except Exception as e:
            print(f"[serial] lost connection: {e}")
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            return False


# ---------------------------------------------------------------- main

def safe_mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0


def build_packet(sessions, cfg, usage):
    now = time.time()
    live = [s for s in sessions.values()
            if now - safe_mtime(s.path) < cfg["active_window_min"] * 60
            and (s.model or s.turn_start)]
    live.sort(key=lambda s: s.first_seen)
    live = live[-cfg["max_sessions"]:]
    act = 0
    if live:
        # auto-follow: prefer a waiting session, else most recently active
        waiting = [i for i, s in enumerate(live) if s.state(cfg)[0] == "wait"]
        if waiting:
            act = waiting[0]
        else:
            act = max(range(len(live)), key=lambda i: safe_mtime(live[i].path))
    return {
        "t": "s",
        "ses": [s.to_packet(cfg) for s in live],
        "act": act,
        "us": usage.snapshot(),
    }


def demo_packets():
    import itertools
    states = itertools.cycle([
        ("run", "", 6), ("tool", "Read", 6), ("tool", "Bash", 7),
        ("run", "", 8), ("wait", "Edit", 9), ("done", "", 9),
    ])
    t0 = time.time()
    while True:
        st, tl, cx = next(states)
        yield {
            "t": "s",
            "ses": [{"nm": "Demo", "md": "Fable 5", "st": st, "tl": tl,
                     "ef": "medium", "el": int(time.time() - t0),
                     "ti": 56500, "to": 4700, "cx": cx, "at": st == "wait"},
                    {"nm": "Second", "md": "Opus 4.8", "st": "run", "tl": "",
                     "ef": "xhigh", "el": 64, "ti": 37700, "to": 3200,
                     "cx": 55, "at": False}],
            "act": 0,
            "us": {"p5": 11, "p7": 34, "r5": "36m", "r7": "5d10h", "est": True},
        }


def main():
    ap = argparse.ArgumentParser(description="Claude Status Bar bridge")
    ap.add_argument("--scan", action="store_true", help="list transcript files found and exit")
    ap.add_argument("--demo", action="store_true", help="send fake data to test the display")
    ap.add_argument("--no-serial", action="store_true", help="print packets instead of sending")
    ap.add_argument("--port", default="", help="serial port override, e.g. COM5")
    ap.add_argument("--replay", default="", help="parse a single .jsonl file and print state")
    args = ap.parse_args()

    cfg = load_config()
    if args.port:
        cfg["port"] = args.port

    if args.scan:
        found = find_transcripts(cfg["roots"])
        if not found:
            print("No transcript files found. Checked roots:")
            for r in cfg["roots"]:
                print(f"  {r}  {'(exists)' if os.path.isdir(r) else '(missing)'}")
            return
        for path, mtime in sorted(found.items(), key=lambda kv: -kv[1])[:25]:
            age = (time.time() - mtime) / 60
            print(f"  {age:7.1f} min ago  {path}")
        print(f"\n{len(found)} transcript file(s) total.")
        return

    if args.replay:
        s = Session(args.replay)
        s.poll([])
        print(json.dumps(s.to_packet(cfg), indent=2))
        return

    link = None if args.no_serial else SerialLink(cfg["port"], cfg["baud"])
    usage = UsageTracker(cfg)
    sessions = {}
    last_rescan = 0
    last_ser = None   # send the logo once per (re)connect
    print("Claude Status Bar bridge running. Ctrl+C to stop.")

    def send_logo():
        path = os.path.join(data_dir(), "logo.bin")
        if not (link and os.path.exists(path)):
            return
        data = open(path, "rb").read()
        if len(data) != 48 * 48 * 2:
            print(f"[logo] logo.bin has wrong size ({len(data)}), run set_logo.bat again")
            return
        chunk = 768
        for off in range(0, len(data), chunk):
            part = data[off:off + chunk]
            pkt = {"t": "lg", "off": off, "px": part.hex(),
                   "last": off + chunk >= len(data)}
            link.send(json.dumps(pkt, separators=(",", ":")) + "\n")
            time.sleep(0.05)
        print("[logo] sent to display")

    if args.demo:
        for pkt in demo_packets():
            line = json.dumps(pkt, separators=(",", ":")) + "\n"
            if link:
                link.send(line)
            else:
                print(line, end="")
            time.sleep(1)
        return

    while True:
        now = time.time()
        if now - last_rescan > 15:
            last_rescan = now
            cutoff = now - 7 * 86400
            for path, mtime in find_transcripts(cfg["roots"]).items():
                if mtime > cutoff and path not in sessions:
                    sessions[path] = Session(path)
            # drop dead sessions
            for path in list(sessions):
                if not os.path.exists(path):
                    del sessions[path]

        new_usage = []
        for s in sessions.values():
            s.poll(new_usage)
        usage.add_events(new_usage)

        pkt = build_packet(sessions, cfg, usage)
        line = json.dumps(pkt, separators=(",", ":")) + "\n"
        if link:
            link.send(line)
            if link.ser is not None and link.ser is not last_ser:
                last_ser = link.ser
                send_logo()
        else:
            sys.stdout.write(line)
            sys.stdout.flush()
        time.sleep(cfg["send_interval_s"])


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
