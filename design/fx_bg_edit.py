#!/usr/bin/env python3
"""Groove Bank FX: clear the parts of the Stitch background that the FX layout puts live controls on.
    python3 design/fx_bg_edit.py images/stitch/bg_0.png
- top-left display: the 'SLOT // PRESET BANK' caption and the 'SOURCE / CLOCK / QUANTIZE' footer go (scanlines are
  copied from an empty band, period 2 px), so GENRE and GROOVE fit as two steppers;
- right panel: the 'SYNC MULTIPLIER' box is emptied for the MIDI IN CH stepper.
Run once on the original artwork (it is idempotent: running it again changes nothing)."""
import sys
from PIL import Image

path = sys.argv[1]
im = Image.open(path).convert("RGB")
band = im.crop((26, 140, 622, 160))          # 20 empty display rows
for y0 in (112, 132, 184):                     # even offsets keep the scanline phase
    im.paste(band, (26, y0))
im.paste((13, 14, 17), (976, 524, 1255, 563))  # SYNC MULTIPLIER box interior
im.save(path)

# v2 (chord source): the right-top panel's mock monitor (CHORD DETECT, NOTE/VEL, DYNAMICS, four small boxes) goes; each
# row is refilled from an empty strip just inside the panel's left edge (period 2 px), keeping the panel's gradient.
im = Image.open(path).convert("RGB")
for y in range(96, 307):
    a, b = im.getpixel((648, y)), im.getpixel((649, y))
    for x in range(648, 1261):
        im.putpixel((x, y), a if (x - 648) % 2 == 0 else b)
im.save(path)
