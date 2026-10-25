#!/usr/bin/env python3
"""Fetch the panel's current LVGL screen (via behemoth, same LAN) and save it as PNG.
usage: pt_shot.py out.png [panel_ip]   - the firmware serves raw RGB565 at :8081/shot (src/ks/ks_debug.cpp)"""
import subprocess, sys
from PIL import Image

out = sys.argv[1]
ip = sys.argv[2] if len(sys.argv) > 2 else "192.168.0.225"
if len(sys.argv) > 3:      # navigate first: pt_shot.py out.png <ip> <panel|home|back>
    subprocess.run(["ssh", "-o", "BatchMode=yes", "tommaso@100.103.234.2",
                    f"curl -s -m 8 http://{ip}:8081/" + (sys.argv[3] if sys.argv[3] in ("home", "back") else "open?p=" + sys.argv[3])],
                   capture_output=True)
    import time; time.sleep(1.5)
raw = subprocess.run(["ssh", "-o", "BatchMode=yes", "tommaso@100.103.234.2",
                      f"curl -s -m 15 http://{ip}:8081/shot"], capture_output=True, check=True).stdout
w, h = 800, 480
if len(raw) != w * h * 2:
    sys.exit(f"unexpected size {len(raw)} (want {w*h*2})")
px = bytearray(w * h * 3)
for i in range(w * h):
    v = raw[2 * i] | (raw[2 * i + 1] << 8)
    px[3 * i] = ((v >> 11) & 31) * 255 // 31
    px[3 * i + 1] = ((v >> 5) & 63) * 255 // 63
    px[3 * i + 2] = (v & 31) * 255 // 31
Image.frombytes("RGB", (w, h), bytes(px)).save(out)
print("saved", out)
