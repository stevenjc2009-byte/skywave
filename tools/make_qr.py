#!/usr/bin/env python3
"""Generates the install QR code for the current release.

FBI on the console can install a CIA straight from a URL it scans with the
camera, so the QR is the shortest path from "I saw this on GitHub" to "it is on
my 3DS" - no SD card, no card reader, no file manager.

The URL is built from source/version.h rather than passed in, for the same
reason tools/release.sh does it: a QR that points at a release which does not
exist, or at the previous one, is worse than no QR at all because it fails
silently at the console rather than here.

The image is written with a wide quiet zone and error correction M. Both matter
on a 3DS: the camera is 640x480 with no autofocus, and a code scanned off a
phone screen at arm's length loses modules at the edges.

    python tools/make_qr.py            # writes qr.png
    python tools/make_qr.py --verify   # ... and decodes it back

--verify needs opencv; without it the script still writes the image and says
that it could not check it, because an unverified QR is exactly the thing this
project does not ship quietly.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import qrcode
from qrcode.constants import ERROR_CORRECT_M

ROOT = Path(__file__).resolve().parent.parent
OWNER = "stevenjc2009-byte"
REPO = "skywave"


def version() -> str:
    text = (ROOT / "source" / "version.h").read_text(encoding="utf-8")
    m = re.search(r'SKYWAVE_VERSION\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("could not read SKYWAVE_VERSION out of source/version.h")
    return m.group(1)


def asset_url(ver: str) -> str:
    return (f"https://github.com/{OWNER}/{REPO}/releases/download/"
            f"v{ver}/{REPO}{ver}.cia")


def build(url: str, out: Path) -> None:
    qr = qrcode.QRCode(
        version=None,              # smallest that fits, chosen by the library
        error_correction=ERROR_CORRECT_M,
        box_size=10,               # 10 px per module - large enough to photograph
        border=6,                  # quiet zone; the spec's minimum of 4 is tight
    )
    qr.add_data(url)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    img.save(out)


def verify(url: str, path: Path) -> bool:
    try:
        import cv2
    except ImportError:
        print("opencv not installed - image written but NOT verified")
        return False

    img = cv2.imread(str(path))
    got, _, _ = cv2.QRCodeDetector().detectAndDecode(img)
    if got == url:
        print(f"decoded OK: {got}")
        return True

    print(f"DECODE MISMATCH\n  wanted: {url}\n  got:    {got!r}")
    return False


def main() -> int:
    ver = version()
    url = asset_url(ver)
    out = ROOT / "qr.png"

    build(url, out)
    print(f"v{ver} -> {out.name} ({out.stat().st_size} bytes)")
    print(f"encodes: {url}")

    if "--verify" in sys.argv:
        return 0 if verify(url, out) else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
