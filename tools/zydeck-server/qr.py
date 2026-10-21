#!/usr/bin/env python3
"""Prints a QR code in the terminal: the text from the command line, or from stdin (so it stays out of the
process list when it holds a token). Two modules per character cell (▀ ▄ █), with the quiet zone.
  python3 qr.py 'zydeck://pair?...'      echo 'zydeck://pair?...' | python3 qr.py
Uses qrcodegen.py (Project Nayuki, MIT) from the same folder."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qrcodegen import QrCode  # noqa: E402

text = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read().strip()
if not text:
    sys.exit("qr.py: nothing to encode")
qr = QrCode.encode_text(text, QrCode.Ecc.LOW)
n, q = qr.get_size(), 2   # quiet zone: 2 modules (terminals add their own margin)


def dark(x, y):
    return 0 <= x < n and 0 <= y < n and qr.get_module(x, y)


# light modules drawn (a dark terminal: the code's light parts must be the bright ones)
for y in range(-q, n + q, 2):
    line = []
    for x in range(-q, n + q):
        top, bottom = not dark(x, y), not dark(x, y + 1)
        line.append("█" if top and bottom else "▀" if top else "▄" if bottom else " ")
    print("".join(line))
