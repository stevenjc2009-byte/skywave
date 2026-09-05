#!/usr/bin/env python3
"""Generates Skywave's icon, banner and banner jingle from scratch.

Everything this writes is drawn or synthesised here, from arithmetic - there is
no third-party artwork, font or audio anywhere in Skywave, and no broadcaster's
branding. That is deliberate: an app that indexes other people's stations should
not also be carrying their marks around.

The mark itself is a transmitter: a mast on a horizon with three arcs leaving
it, in the same night-blue and signal-amber the interface uses. It has to read
at 48x48 on the HOME menu, which is what dictates the heavy strokes and the
three-arc count - four arcs turned to mush at that size when tried.

    python tools/make_art.py

Writes:
    icon.png         48x48   HOME menu / 3dsx icon
    cia/banner.png   256x128 the banner shown above the icon
    cia/banner.wav   stereo 16-bit PCM, the sound that plays with it
"""

import math
import os
import struct
import wave

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The interface palette, so the icon and the app look like the same thing.
BG_TOP    = (0x0C, 0x14, 0x24)
BG_BOTTOM = (0x1E, 0x2D, 0x4C)
ACCENT    = (0xFF, 0xB3, 0x3C)
ACCENT_2  = (0xFF, 0xD2, 0x8A)
MAST      = (0xEC, 0xF1, 0xF8)
HORIZON   = (0x2A, 0x3C, 0x5E)


def vertical_gradient(size, top, bottom):
    """A plain top-to-bottom gradient. Both images sit on one."""
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def draw_transmitter(draw, cx, base_y, height, scale, arc_colours):
    """Draws the mast and its arcs.

    `scale` is a stroke multiplier rather than a resize, because scaling a
    finished 48px drawing up to banner size is what makes homebrew icons look
    like homebrew icons.
    """
    stroke = max(1, int(round(2 * scale)))

    top_y = base_y - height
    half_base = height * 0.30

    # The mast: two legs and a cross-brace, not a single line. A single line
    # reads as an antenna rather than a transmitter at any size.
    draw.line([(cx - half_base, base_y), (cx, top_y)], fill=MAST, width=stroke)
    draw.line([(cx + half_base, base_y), (cx, top_y)], fill=MAST, width=stroke)

    for f in (0.38, 0.68):
        y = base_y - height * f
        w = half_base * (1 - f)
        draw.line([(cx - w, y), (cx + w, y)], fill=MAST, width=max(1, stroke - 1))

    # Emitter.
    r = max(2, int(round(3 * scale)))
    draw.ellipse([cx - r, top_y - r, cx + r, top_y + r], fill=ACCENT_2)

    # Three arcs leaving the top, drawn as pairs of opposing arcs so the signal
    # reads as going outward on both sides rather than as a set of rings.
    for i, colour in enumerate(arc_colours):
        rad = height * (0.30 + 0.20 * i)
        box = [cx - rad, top_y - rad, cx + rad, top_y + rad]
        aw = max(1, int(round((2.4 - 0.4 * i) * scale)))
        draw.arc(box, start=196, end=254, fill=colour, width=aw)
        draw.arc(box, start=286, end=344, fill=colour, width=aw)


def fade(colour, amount):
    """Mixes `colour` toward the background - used to fade the outer arcs."""
    return tuple(int(colour[i] * amount + BG_BOTTOM[i] * (1 - amount)) for i in range(3))


def make_icon(path):
    # Drawn at 4x and reduced. PIL has no antialiased line drawing, so the only
    # way to get clean diagonals is to supersample - without this the mast legs
    # come out as visible staircases at 48px.
    s = 4
    size = 48 * s
    img = vertical_gradient((size, size), BG_TOP, BG_BOTTOM)
    d = ImageDraw.Draw(img)

    # Horizon line, low, so the mast has something to stand on.
    hy = int(size * 0.80)
    d.line([(0, hy), (size, hy)], fill=HORIZON, width=2 * s)

    # The mast height is what the arcs are measured from, and the outermost arc
    # reaches 0.70 of it above the emitter. 0.40 is the largest value that keeps
    # that arc inside the frame - anything more and the signal gets guillotined
    # by the top edge, which is what the first attempt did.
    draw_transmitter(
        d, cx=size // 2, base_y=hy, height=int(size * 0.40), scale=1.6 * s,
        arc_colours=[ACCENT, ACCENT, fade(ACCENT, 0.65)],
    )

    img = img.resize((48, 48), Image.LANCZOS)
    img.save(path)
    return img.size


def make_banner(path):
    s = 3
    w, h = 256 * s, 128 * s
    img = vertical_gradient((w, h), BG_TOP, BG_BOTTOM)
    d = ImageDraw.Draw(img)

    hy = int(h * 0.82)
    d.line([(0, hy), (w, hy)], fill=HORIZON, width=2 * s)

    # The mast sits left of centre with the wordmark to its right, because the
    # banner is wide and a centred mark on a 2:1 image leaves two empty halves.
    # Same height constraint as the icon: the outer arc reaches 0.70 of the
    # mast height above the emitter and must not touch the top edge.
    draw_transmitter(
        d, cx=int(w * 0.17), base_y=hy, height=int(h * 0.44), scale=2.2 * s,
        arc_colours=[ACCENT, ACCENT, fade(ACCENT, 0.6)],
    )

    # The wordmark is drawn as blocks rather than typed, so that no font file is
    # involved and the banner needs nothing installed to reproduce. The unit is
    # solved for rather than picked: SKYWAVE is seven 5-wide glyphs with six
    # 1-wide gaps, so a guessed size runs off the right edge - which is exactly
    # what the first attempt did.
    word_units = 7 * 5 + 6
    left = int(w * 0.34)
    unit = (w - left - int(w * 0.05)) // word_units
    draw_wordmark(d, x=left, y=(h - 7 * unit) // 2, unit=unit)

    img = img.resize((256, 128), Image.LANCZOS)
    img.save(path)
    return img.size


# A 5x7 block alphabet, only the letters "SKYWAVE" needs. Written out rather
# than generated because it is the one part of this that has to be legible, and
# legibility is not something to leave to a formula.
GLYPHS = {
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
}


def draw_wordmark(d, x, y, unit):
    word = "SKYWAVE"
    gap = unit
    cx = x
    for ch in word:
        rows = GLYPHS[ch]
        for ry, row in enumerate(rows):
            for rxi, bit in enumerate(row):
                if bit == "1":
                    px = cx + rxi * unit
                    py = y + ry * unit
                    d.rectangle([px, py, px + unit - 1, py + unit - 1], fill=MAST)
        cx += 5 * unit + gap


def make_jingle(path):
    """A three-note rising chime - the sound of tuning in, roughly.

    bannertool wants 16-bit PCM stereo. Notes are pure sines with a soft attack
    and an exponential decay; a hard start on a sine is a click, and a click is
    the one thing a HOME-menu sound must not be.
    """
    rate = 32000
    dur = 2.4
    n = int(rate * dur)

    # A, C#, E - a major triad, arriving one note at a time and left ringing.
    notes = [(440.0, 0.00), (554.37, 0.18), (659.25, 0.36)]

    left = [0.0] * n
    right = [0.0] * n

    for freq, start in notes:
        s0 = int(start * rate)
        for i in range(s0, n):
            t = (i - s0) / rate
            env = (1 - math.exp(-t * 60)) * math.exp(-t * 1.9)
            v = math.sin(2 * math.pi * freq * t) * env * 0.24
            # A few cents of detune between the ears, which is what gives it a
            # little width on headphones without sounding like an effect.
            vr = math.sin(2 * math.pi * (freq * 1.0015) * t) * env * 0.24
            left[i] += v
            right[i] += vr

    # Fade the last 200 ms to true silence so the loop point cannot tick.
    tail = int(0.2 * rate)
    for i in range(n - tail, n):
        g = (n - i) / tail
        left[i] *= g
        right[i] *= g

    frames = bytearray()
    for i in range(n):
        l = max(-1.0, min(1.0, left[i]))
        r = max(-1.0, min(1.0, right[i]))
        frames += struct.pack("<hh", int(l * 32767), int(r * 32767))

    with wave.open(path, "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(bytes(frames))

    return n, rate


def main():
    os.makedirs(os.path.join(ROOT, "cia"), exist_ok=True)

    icon = os.path.join(ROOT, "icon.png")
    banner = os.path.join(ROOT, "cia", "banner.png")
    jingle = os.path.join(ROOT, "cia", "banner.wav")

    print("icon.png       ", make_icon(icon))
    print("cia/banner.png ", make_banner(banner))
    print("cia/banner.wav ", make_jingle(jingle))


if __name__ == "__main__":
    main()
