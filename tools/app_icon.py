#!/usr/bin/env python3
"""An app icon from the game's artwork (config/<game>/app.env APP_ICON).

    tools/app_icon.py switch <artwork> <out.jpg>
    tools/app_icon.py android <artwork> <out.png>

switch: 256x256, a bare baseline JPEG (4:2:0, no EXIF, no progressive, no
ICC profile): what hbmenu and the home menu show. Anything more and the
home menu draws a '?' or nothing (NTON re-encodes the same way); hbmenu
ignores an icon that is not 256x256. Non-square artwork is cropped to the
middle square first.

android: the foreground of an adaptive launcher icon, 432x432 (108 dp at
xxxhdpi). Launchers cut the layer to their own shape (circle, squircle,
...) and show only its middle 72 dp; a circle cuts that to its inscribed
disc. So the artwork is 60 dp with rounded corners, the whole logo inside
even a circle, over itself enlarged, blurred and darkened for whatever a
shape shows around it. Full-bleed (or 76 dp) the cover's logo lost its
ends in a circle.

(The Mac's rounded .icns is tools/mac_icon.py, Windows' .ico
tools/windows/win_icon.py.)
"""
import os
import sys

from PIL import Image


def square(im):
    w, h = im.size
    s = min(w, h)
    return im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))


def switch(src, dst):
    im = square(Image.open(src).convert("RGB"))
    if im.size != (256, 256):
        im = im.resize((256, 256), Image.LANCZOS)
    clean = Image.frombytes("RGB", im.size, im.tobytes())   # pixels only: no EXIF, no ICC
    clean.save(dst, format="JPEG", quality=98, subsampling=2)
    print("  icon: %s -> %s (256x256 baseline JPEG)" % (os.path.relpath(src), os.path.relpath(dst)))


def android(src, dst):
    from PIL import ImageDraw, ImageFilter
    side, body = 432, round(432 * 60 / 108)
    im = square(Image.open(src).convert("RGB"))
    back = im.resize((side, side), Image.LANCZOS).filter(ImageFilter.GaussianBlur(18))
    back = Image.eval(back, lambda v: int(v * 0.55))               # darker: the art stands out
    art = im.resize((body, body), Image.LANCZOS)
    mask = Image.new("L", (body * 4, body * 4), 0)              # 4x: smooth corners
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, body * 4 - 1, body * 4 - 1),
                                           radius=round(body * 4 * 0.12), fill=255)
    mask = mask.resize((body, body), Image.LANCZOS)
    back.paste(art, ((side - body) // 2, (side - body) // 2), mask)
    back.save(dst, format="PNG", optimize=True)
    print("  icon: %s -> %s (Android adaptive foreground)" % (os.path.relpath(src), os.path.relpath(dst)))


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("switch", "android"):
        sys.exit(__doc__)
    (switch if sys.argv[1] == "switch" else android)(sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    main()
