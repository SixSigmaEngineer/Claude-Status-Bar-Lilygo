#!/usr/bin/env python3
"""
Logo uploader for the Claude Status Bar display.

Converts a JPG/PNG into the 48x48 RGB565 format the display uses and saves it
as logo.bin next to the bridge. The bridge streams it to the display on every
connect, and the display remembers it in flash.

Usage:
    python set_logo.py               # opens a file picker
    python set_logo.py mylogo.jpg    # direct
    python set_logo.py --clear       # remove the logo
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "logo.bin")
SIZE = 48


def pick_file():
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        path = filedialog.askopenfilename(
            title="Choose a logo image",
            filetypes=[("Images", "*.jpg *.jpeg *.png *.bmp *.gif"), ("All files", "*.*")])
        root.destroy()
        return path
    except Exception:
        return input("Path to logo image: ").strip('" ')


def main():
    if "--clear" in sys.argv:
        if os.path.exists(OUT):
            os.remove(OUT)
        print("Logo removed. Restart the bridge, then the display will show the")
        print("default mark after its next 'lgclr'... or just re-run with a new image.")
        return

    path = next((a for a in sys.argv[1:] if not a.startswith("-")), "") or pick_file()
    if not path or not os.path.exists(path):
        print("No image selected.")
        return

    try:
        from PIL import Image
    except ImportError:
        print("Installing Pillow (image library)...")
        os.system(f'"{sys.executable}" -m pip install pillow')
        from PIL import Image

    img = Image.open(path).convert("RGB")
    # cover-crop to square, then resize
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2,
                    (w + side) // 2, (h + side) // 2))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)

    data = bytearray()
    for r, g, b in img.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        data += v.to_bytes(2, "little")

    with open(OUT, "wb") as f:
        f.write(data)
    print(f"Saved {OUT} ({len(data)} bytes).")
    print("Restart the bridge (or replug the display) and the logo will appear")
    print("in the top-left corner. It's saved on the device, so it survives reboots.")


if __name__ == "__main__":
    main()
