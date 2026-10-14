#!/usr/bin/env python3
"""
Shrink images by a percentage or by a number of pixels (aspect ratio is kept).

  pip install pillow

Exactly one size option:
  --percent P        shrink BY P percent       (25 -> 75 % of the original size)
  --pixels N         shrink the longer side BY N pixels, the other side follows
  --width W          shrink TO W pixels wide   (height follows)
  --height H         shrink TO H pixels high   (width follows)
  --width W --height H   shrink TO exactly W x H (may distort)

Inputs can be files and/or folders (all images in the folder; --recursive for subfolders).
Results go to --out (default: ./shrunk), keeping the file names, or overwrite with --in-place.

Examples
  python shrink_images.py photo.jpg --percent 50
  python shrink_images.py D:/Renders/Shot010 --pixels 200 --out D:/Renders/Shot010_small
  python shrink_images.py *.png --width 1280 --in-place
"""

from __future__ import annotations

import argparse
import os
import sys

from PIL import Image, ImageOps

EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".gif", ".tga"}


def target_size(w: int, h: int, percent=None, pixels=None, width=None, height=None) -> tuple[int, int]:
    """New (width, height) for an image of w x h. Raises ValueError if it would not shrink."""
    if percent is not None:
        if not 0 < percent < 100:
            raise ValueError("--percent must be between 0 and 100")
        s = 1.0 - percent / 100.0
        nw, nh = round(w * s), round(h * s)
    elif pixels is not None:
        longer = max(w, h)
        if not 0 < pixels < longer:
            raise ValueError(f"--pixels must be between 0 and {longer} (longer side)")
        s = (longer - pixels) / longer
        nw, nh = (longer - pixels, round(h * s)) if w >= h else (round(w * s), longer - pixels)
    elif width is not None and height is not None:
        nw, nh = width, height
    elif width is not None:
        nw, nh = width, round(h * width / w)
    elif height is not None:
        nw, nh = round(w * height / h), height
    else:
        raise ValueError("no size option given")

    nw, nh = max(1, nw), max(1, nh)
    if nw > w or nh > h:
        raise ValueError(f"{nw}x{nh} would be larger than the original {w}x{h}")
    return nw, nh


def collect(inputs: list[str], recursive: bool) -> list[tuple[str, str]]:
    """(path, path relative to its input folder) for every image."""
    files = []
    for inp in inputs:
        if os.path.isdir(inp):
            for dirpath, dirnames, names in os.walk(inp):
                if not recursive:
                    dirnames.clear()
                for n in sorted(names):
                    if os.path.splitext(n)[1].lower() in EXTENSIONS:
                        p = os.path.join(dirpath, n)
                        files.append((p, os.path.relpath(p, inp)))
        elif os.path.isfile(inp):
            files.append((inp, os.path.basename(inp)))
        else:
            print(f"skip (not found): {inp}", file=sys.stderr)
    return files


def shrink_file(src: str, dst: str, quality: int, **size) -> tuple[tuple[int, int], tuple[int, int]]:
    with Image.open(src) as im:
        fmt = im.format
        info = {k: im.info[k] for k in ("icc_profile", "dpi") if k in im.info}
        im = ImageOps.exif_transpose(im)
        old = im.size
        new = target_size(*old, **size)
        out = im.resize(new, Image.Resampling.LANCZOS) if new != old else im.copy()
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    if fmt in ("JPEG", "WEBP"):
        info["quality"] = quality
    out.save(dst, format=fmt, **info)
    return old, new


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Shrink images by a percentage or a number of pixels.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("inputs", nargs="+", help="image files and/or folders")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("-p", "--percent", type=float, help="shrink BY this many percent")
    mode.add_argument("-n", "--pixels", type=int, help="shrink the longer side BY this many pixels")
    ap.add_argument("-W", "--width", type=int, help="target width in pixels")
    ap.add_argument("-H", "--height", type=int, help="target height in pixels")
    ap.add_argument("-o", "--out", default="shrunk", help="output folder (default: ./shrunk)")
    ap.add_argument("--in-place", action="store_true", help="overwrite the originals")
    ap.add_argument("-r", "--recursive", action="store_true", help="include subfolders")
    ap.add_argument("-q", "--quality", type=int, default=92, help="JPEG/WebP quality (default 92)")
    a = ap.parse_args(argv)

    if (a.percent is not None or a.pixels is not None) and (a.width or a.height):
        ap.error("--percent/--pixels cannot be combined with --width/--height")
    if a.percent is None and a.pixels is None and not a.width and not a.height:
        ap.error("give one of --percent, --pixels, --width, --height")

    files = collect(a.inputs, a.recursive)
    if not files:
        print("no images found", file=sys.stderr)
        return 1

    size = dict(percent=a.percent, pixels=a.pixels, width=a.width, height=a.height)
    failed = 0
    for src, rel in files:
        dst = src if a.in_place else os.path.join(a.out, rel)
        try:
            (ow, oh), (nw, nh) = shrink_file(src, dst, a.quality, **size)
            print(f"{src}: {ow}x{oh} -> {nw}x{nh}  ({dst})")
        except Exception as e:  # keep going with the other files
            failed += 1
            print(f"{src}: FAILED - {e}", file=sys.stderr)
    print(f"done: {len(files) - failed} ok, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
