#!/usr/bin/env python3
"""
Claude Status Bar — desktop app.

Runs the bridge with a UI: live preview of what the hardware display shows,
logo uploader, serial status, minimize-to-system-tray, and a
start-with-Windows toggle.

    python claude_bar_app.py           # windowed
    python claude_bar_app.py --tray    # start minimized to tray

Requires: pip install pyserial pillow pystray
"""

import json
import os
import platform
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox

import claude_bar_bridge as bridge

HERE = os.path.dirname(os.path.abspath(__file__))
LOGO_BIN = os.path.join(bridge.data_dir(), "logo.bin")

# display colors (match firmware)
BG = "#000000"
PANEL = "#181c22"
TEXT = "#ebebE6"
DIM = "#8c8c96"
ORANGE = "#cb6a44"
GREEN = "#3cdc78"
YELLOW = "#f0c83c"
RED = "#eb5046"
BAR_BG = "#20242a"


class BridgeThread(threading.Thread):
    """The bridge main loop, feeding both the serial link and the UI."""

    def __init__(self):
        super().__init__(daemon=True)
        self.cfg = bridge.load_config()
        self.link = bridge.SerialLink(self.cfg["port"], self.cfg["baud"])
        self.usage = bridge.UsageTracker(self.cfg)
        self.sessions = {}
        self.last_pkt = None
        self.connected = False
        self.logo_dirty = True   # send logo on next connect
        self._last_ser = None
        self._stop = threading.Event()

    def send_logo_now(self):
        self.logo_dirty = True

    def _send_logo(self):
        if not os.path.exists(LOGO_BIN):
            return
        data = open(LOGO_BIN, "rb").read()
        if len(data) != 48 * 48 * 2:
            return
        for off in range(0, len(data), 768):
            part = data[off:off + 768]
            pkt = {"t": "lg", "off": off, "px": part.hex(),
                   "last": off + 768 >= len(data)}
            self.link.send(json.dumps(pkt, separators=(",", ":")) + "\n")
            time.sleep(0.05)

    def _send_logo_clear(self):
        self.link.send('{"t":"lgclr"}\n')

    def clear_logo(self):
        if os.path.exists(LOGO_BIN):
            os.remove(LOGO_BIN)
        self._send_logo_clear()

    def run(self):
        last_rescan = 0
        while not self._stop.is_set():
            now = time.time()
            if now - last_rescan > 15:
                last_rescan = now
                cutoff = now - 7 * 86400
                for path, mtime in bridge.find_transcripts(self.cfg["roots"]).items():
                    if mtime > cutoff and path not in self.sessions:
                        self.sessions[path] = bridge.Session(path)
                for path in list(self.sessions):
                    if not os.path.exists(path):
                        del self.sessions[path]

            new_usage = []
            for s in self.sessions.values():
                s.poll(new_usage)
            self.usage.add_events(new_usage)

            pkt = bridge.build_packet(self.sessions, self.cfg, self.usage)
            self.last_pkt = pkt
            line = json.dumps(pkt, separators=(",", ":")) + "\n"
            self.link.send(line)
            self.connected = self.link.ser is not None
            if self.link.ser is not None and (self.link.ser is not self._last_ser
                                              or self.logo_dirty):
                if self.link.ser is not self._last_ser:
                    self.link.send(bridge.input_cfg_packet(self.cfg))
                self._last_ser = self.link.ser
                self.logo_dirty = False
                self._send_logo()
            time.sleep(self.cfg["send_interval_s"])

    def stop(self):
        self._stop.set()


class App:
    W, H = 640, 180

    def __init__(self, start_in_tray=False):
        self.bt = BridgeThread()
        self.bt.start()
        self.page = 0
        self.tray_icon = None
        self.logo_img = None

        self.root = tk.Tk()
        self.root.title("Claude Status Bar")
        self.root.configure(bg="#111318")
        self.root.resizable(False, False)

        self.canvas = tk.Canvas(self.root, width=self.W, height=self.H,
                                bg=BG, highlightthickness=1,
                                highlightbackground="#333")
        self.canvas.pack(padx=14, pady=(14, 8))
        self.canvas.bind("<Button-1>", lambda e: self.toggle_page())

        bar = tk.Frame(self.root, bg="#111318")
        bar.pack(fill="x", padx=14, pady=(0, 6))

        def btn(text, cmd):
            return tk.Button(bar, text=text, command=cmd, bg="#22262e",
                             fg="#ddd", activebackground="#333",
                             activeforeground="#fff", relief="flat", padx=10)

        btn("Set logo...", self.pick_logo).pack(side="left", padx=(0, 6))
        btn("Clear logo", self.clear_logo).pack(side="left", padx=(0, 6))
        btn("Switch page", self.toggle_page).pack(side="left", padx=(0, 6))
        btn("Minimize to tray", self.to_tray).pack(side="right")

        opts = tk.Frame(self.root, bg="#111318")
        opts.pack(fill="x", padx=14, pady=(0, 8))
        self.autostart_var = tk.BooleanVar(value=self.autostart_installed())
        login_label = {"Windows": "Start with Windows (in tray)",
                       "Darwin": "Start at login (in tray)"}.get(
            platform.system(), "Start at login (in tray)")
        tk.Checkbutton(opts, text=login_label,
                       variable=self.autostart_var, command=self.set_autostart,
                       bg="#111318", fg="#aaa", selectcolor="#22262e",
                       activebackground="#111318",
                       activeforeground="#ddd").pack(side="left")
        self.status_lbl = tk.Label(opts, text="", bg="#111318", fg="#888")
        self.status_lbl.pack(side="right")

        self.root.protocol("WM_DELETE_WINDOW", self.to_tray)
        self.load_logo_preview()
        self.tick()
        if start_in_tray:
            self.root.withdraw()
            self.make_tray()

    # ---------------- logo ----------------

    def load_logo_preview(self):
        self.logo_img = None
        if not os.path.exists(LOGO_BIN):
            return
        try:
            from PIL import Image, ImageTk
            data = open(LOGO_BIN, "rb").read()
            img = Image.new("RGB", (48, 48))
            px = img.load()
            for i in range(48 * 48):
                v = int.from_bytes(data[i * 2:i * 2 + 2], "little")
                px[i % 48, i // 48] = (((v >> 11) & 0x1F) << 3,
                                       ((v >> 5) & 0x3F) << 2,
                                       (v & 0x1F) << 3)
            self.logo_img = ImageTk.PhotoImage(img)
        except Exception:
            pass

    def pick_logo(self):
        path = filedialog.askopenfilename(
            title="Choose a logo image",
            filetypes=[("Images", "*.jpg *.jpeg *.png *.bmp *.gif"),
                       ("All files", "*.*")])
        if not path:
            return
        try:
            from PIL import Image
        except ImportError:
            messagebox.showerror("Missing library",
                                 "Pillow not installed.\nRun: pip install pillow")
            return
        img = Image.open(path).convert("RGB")
        w, h = img.size
        side = min(w, h)
        img = img.crop(((w - side) // 2, (h - side) // 2,
                        (w + side) // 2, (h + side) // 2))
        img = img.resize((48, 48), Image.LANCZOS)
        data = bytearray()
        for r, g, b in img.getdata():
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            data += v.to_bytes(2, "little")
        with open(LOGO_BIN, "wb") as f:
            f.write(data)
        self.load_logo_preview()
        self.bt.send_logo_now()

    def clear_logo(self):
        self.bt.clear_logo()
        self.load_logo_preview()

    # ---------------- autostart ----------------

    def _launch_cmd(self):
        """[interpreter, script?, --tray] used to relaunch the app at login."""
        if getattr(sys, "frozen", False):   # packaged exe
            return [sys.executable, "--tray"]
        py = sys.executable
        if platform.system() == "Windows":
            py = py.replace("python.exe", "pythonw.exe")
        return [py, os.path.join(HERE, "claude_bar_app.py"), "--tray"]

    def shortcut_path(self):
        sysname = platform.system()
        if sysname == "Windows":
            return os.path.join(os.environ["APPDATA"],
                                r"Microsoft\Windows\Start Menu\Programs\Startup",
                                "ClaudeStatusBar.lnk")
        if sysname == "Darwin":
            return os.path.expanduser(
                "~/Library/LaunchAgents/com.claudestatusbar.app.plist")
        xdg = os.environ.get("XDG_CONFIG_HOME",
                             os.path.expanduser("~/.config"))
        return os.path.join(xdg, "autostart", "claudestatusbar.desktop")

    def autostart_installed(self):
        return os.path.exists(self.shortcut_path())

    def set_autostart(self):
        try:
            path = self.shortcut_path()
            if not self.autostart_var.get():
                if os.path.exists(path):
                    if platform.system() == "Darwin":
                        subprocess.run(["launchctl", "unload", path],
                                       capture_output=True)
                    os.remove(path)
                return
            cmd = self._launch_cmd()
            sysname = platform.system()
            if sysname == "Windows":
                target, rest = cmd[0], subprocess.list2cmdline(cmd[1:])
                ps = ("$ws = New-Object -ComObject WScript.Shell; "
                      f"$s = $ws.CreateShortcut('{path}'); "
                      f"$s.TargetPath = '{target}'; "
                      f"$s.Arguments = '{rest}'; "
                      f"$s.WorkingDirectory = '{HERE}'; $s.Save()")
                subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                               capture_output=True)
            elif sysname == "Darwin":
                import plistlib
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    plistlib.dump({
                        "Label": "com.claudestatusbar.app",
                        "ProgramArguments": cmd,
                        "WorkingDirectory": HERE,
                        "RunAtLoad": True,
                    }, f)
                subprocess.run(["launchctl", "load", path],
                               capture_output=True)
            else:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                exec_line = " ".join(
                    f'"{c}"' if " " in c else c for c in cmd)
                with open(path, "w", encoding="utf-8") as f:
                    f.write("[Desktop Entry]\nType=Application\n"
                            "Name=Claude Status Bar\n"
                            f"Exec={exec_line}\n"
                            f"Path={HERE}\n"
                            "X-GNOME-Autostart-enabled=true\n")
        except Exception as e:
            messagebox.showerror("Autostart", str(e))

    # ---------------- tray ----------------

    def make_tray(self):
        if self.tray_icon:
            return
        try:
            import pystray
            from PIL import Image, ImageDraw
        except ImportError:
            messagebox.showinfo(
                "Tray support",
                "Install tray support with:\n\npip install pystray pillow\n\n"
                "Window will minimize normally instead.")
            self.root.iconify()
            self.root.deiconify()
            return
        img = Image.new("RGB", (64, 64), "#1a1a1a")
        d = ImageDraw.Draw(img)
        d.polygon([(32, 8), (40, 26), (58, 32), (40, 38), (32, 56),
                   (24, 38), (6, 32), (24, 26)], fill="#cb6a44")
        menu = pystray.Menu(
            pystray.MenuItem("Show", self.from_tray, default=True),
            pystray.MenuItem("Quit", self.quit_app))
        self.tray_icon = pystray.Icon("ClaudeStatusBar", img,
                                      "Claude Status Bar", menu)
        self.tray_icon.run_detached()

    def to_tray(self):
        self.root.withdraw()
        self.make_tray()

    def from_tray(self, *_):
        self.root.after(0, self.root.deiconify)

    def quit_app(self, *_):
        if self.tray_icon:
            self.tray_icon.stop()
        self.bt.stop()
        self.root.after(0, self.root.destroy)

    # ---------------- preview rendering ----------------

    def toggle_page(self):
        self.page = (self.page + 1) % 2

    def tick(self):
        self.draw()
        self.root.after(500, self.tick)

    def draw(self):
        c = self.canvas
        c.delete("all")
        pkt = self.bt.last_pkt
        self.status_lbl.config(
            text="display connected" if self.bt.connected else "display not found")

        if not pkt or not pkt.get("ses"):
            c.create_text(self.W / 2, 80, text="No sessions", fill=TEXT,
                          font=("Segoe UI", 22, "bold"))
            c.create_text(self.W / 2, 118, text="waiting for Claude activity...",
                          fill=DIM, font=("Segoe UI", 11))
            self.draw_header(pkt)
            return

        if self.page == 1:
            self.draw_usage(pkt)
        else:
            self.draw_status(pkt)
        self.draw_header(pkt)

    def draw_header(self, pkt):
        c = self.canvas
        c.create_oval(self.W - 28, 6, self.W - 22, 12,
                      fill=ORANGE if self.page == 0 else DIM, outline="")
        c.create_oval(self.W - 16, 6, self.W - 10, 12,
                      fill=ORANGE if self.page == 1 else DIM, outline="")
        if pkt and pkt.get("ses"):
            act = pkt.get("act", 0)
            n = len(pkt["ses"])
            c.create_text(self.W - 40, 9, text=f"{chr(65 + act)} {act + 1}/{n}",
                          fill=DIM, font=("Consolas", 8), anchor="e")
            waiting = [i for i, s in enumerate(pkt["ses"])
                       if s.get("at") and i != act]
            if waiting:
                c.create_rectangle(0, 0, self.W, 14, fill=ORANGE, outline="")
                c.create_text(6, 7, text=f"! session {chr(65 + waiting[0])} waiting",
                              fill=BG, font=("Consolas", 8, "bold"), anchor="w")

    def draw_status(self, pkt):
        c = self.canvas
        s = pkt["ses"][pkt.get("act", 0) % len(pkt["ses"])]

        # left column
        if self.logo_img:
            c.create_image(14, 12, image=self.logo_img, anchor="nw")
        else:
            c.create_text(14, 30, text="**", fill=ORANGE, anchor="w",
                          font=("Segoe UI", 18, "bold"))
        c.create_text(10, 108, text="CONTEXT", fill=DIM,
                      font=("Consolas", 8), anchor="w")
        cx = s.get("cx", 0)
        ctx_col = RED if cx >= 80 else (YELLOW if cx >= 50 else GREEN)
        c.create_text(10, 140, text=f"{cx}%", fill=ctx_col,
                      font=("Segoe UI", 20, "bold"), anchor="w")
        c.create_line(178, 18, 178, 166, fill=PANEL)

        # center: project / title / state / detail
        z0 = 196
        pj = s.get("pj") or s.get("nm") or "Claude"
        c.create_text(z0, 32, text=pj, fill=TEXT, anchor="w",
                      font=("Segoe UI", 15, "bold"))
        mline = s.get("md", "Claude")
        if s.get("ef"):
            mline += f" · {s['ef']}"
        c.create_text(self.W - 10, 32, text=mline, fill=DIM, anchor="e",
                      font=("Segoe UI", 10))
        if s.get("pj") and s.get("nm"):
            c.create_text(z0, 58, text=s["nm"], fill=DIM, anchor="w",
                          font=("Segoe UI", 10))

        st = s.get("st", "idle")
        word, col = {
            "tool": (s.get("tl") or "Tool", TEXT),
            "run": ("Running", TEXT),
            "wait": ("Waiting on you", ORANGE),
            "done": ("Done", GREEN),
        }.get(st, ("Idle", DIM))
        prefix = "✦ " if st in ("run", "tool") else ""
        c.create_text(z0 + 8, 100, text=prefix + word, fill=col, anchor="w",
                      font=("Segoe UI", 24, "bold"))

        dline = ""
        if st == "wait" and s.get("tl") and s.get("tl") != "Question":
            dline = f"approve: {s['tl']}" + (f" · {s['td']}" if s.get("td") else "")
        elif s.get("td"):
            dline = s["td"]
        if s.get("sa"):
            dline += ("· " if not dline else " · ") + \
                     f"{s['sa']} subagent{'s' if s['sa'] > 1 else ''}"
        if dline:
            c.create_text(z0 + 10, 130, text=dline, fill=DIM, anchor="w",
                          font=("Segoe UI", 10))

        def fmt_k(v):
            v = v or 0
            if v >= 1e6:
                return f"{v / 1e6:.1f}M"
            if v >= 1e3:
                return f"{v / 1e3:.1f}k"
            return str(v)

        el = s.get("el", 0)
        eb = f"{el}s" if el < 180 else f"{el // 60}m{el % 60:02d}s"
        c.create_text(z0, 158, text=f"{eb}   ▲{fmt_k(s.get('ti'))}   ▼{fmt_k(s.get('to'))}",
                      fill=DIM, anchor="w", font=("Segoe UI", 11))

    def draw_usage(self, pkt):
        c = self.canvas
        us = pkt.get("us") or {}
        title = "CLAUDE USAGE (estimated)" if us.get("est") else "CLAUDE USAGE"
        c.create_text(12, 10, text=title, fill=ORANGE,
                      font=("Consolas", 9, "bold"), anchor="w")
        for i, (label, pk, rk) in enumerate((("5-HOUR", "p5", "r5"),
                                             ("7-DAY", "p7", "r7"))):
            y = 44 + i * 72
            pct = us.get(pk, -1)
            c.create_text(12, y + 13, text=label, fill=DIM,
                          font=("Segoe UI", 11), anchor="w")
            c.create_rectangle(120, y, 500, y + 26, fill=BAR_BG, outline="")
            if pct is not None and pct >= 0:
                col = RED if pct >= 90 else (YELLOW if pct >= 70 else GREEN)
                c.create_rectangle(120, y, 120 + 380 * min(pct, 100) / 100,
                                   y + 26, fill=col, outline="")
                c.create_text(514, y + 13, text=f"{pct}%", fill=TEXT,
                              font=("Segoe UI", 13, "bold"), anchor="w")
            resets = us.get(rk) or "?"
            c.create_text(120, y + 36, text=f"resets in {resets}", fill=DIM,
                          font=("Consolas", 8), anchor="w")

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    App(start_in_tray="--tray" in sys.argv).run()
