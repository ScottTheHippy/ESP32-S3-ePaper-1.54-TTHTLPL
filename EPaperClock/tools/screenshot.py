#!/usr/bin/env python3
"""Grab a page screenshot from the e-paper clock over USB serial.

usage: screenshot.py PORT OUT.png [page] [--demo]    (needs: pip install pyserial pillow)

Sends the serial command "S<page>" (0 clock, 1 climate, 2 tides, 9 setup; default: the page
currently shown). The firmware draws that page into an in-memory canvas (the e-paper itself
cannot be read back) and replies "SHOT", a byte count, and 200x200 1-bit rows (1 = white).
The PNG is scaled up 3x so it is easy to inspect.

--demo first turns on the firmware's demo view (synthetic tides and a synthetic week of
readings; nothing is stored), which is how the README pictures were made.

Opening the port resets this board (USB-Serial/JTAG), so the script waits for it to boot.
"""
import sys, time
import serial
from PIL import Image

args = [a for a in sys.argv[1:] if not a.startswith("--")]
demo = "--demo" in sys.argv
port, out = args[0], args[1]
page = args[2] if len(args) > 2 else ""
W = H = 200
need = (W // 8) * H

with serial.Serial() as s:
    s.port, s.baudrate, s.timeout = port, 115200, 2
    s.dtr = s.rts = False
    s.open()
    time.sleep(8)  # the board reboots when the port opens; let it finish starting
    if demo:
        s.write(b"V\n")
        time.sleep(1.5)
    data = None
    for _ in range(4):
        s.reset_input_buffer()
        s.write(f"S{page}\n".encode())
        buf = b""
        end = time.time() + 8
        while time.time() < end:
            buf += s.read(4096)
            i = buf.find(b"SHOT")
            if i >= 0 and len(buf) >= i + 8 + need:
                data = buf[i + 8:i + 8 + need]
                break
        if data:
            break
if not data:
    sys.exit("no complete frame received")

img = Image.frombytes("1", (W, H), data)  # MSB first, 1 = white
img.convert("L").resize((W * 3, H * 3), Image.NEAREST).save(out)
print("saved", out)
