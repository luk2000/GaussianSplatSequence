#!/usr/bin/env python3
"""
Test of Tools/shrink_images.py.

  pip install pillow
  python Tests/test_shrink_images.py
"""

import os
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "Tools"))
import shrink_images  # noqa: E402


def expect_error(**size):
    try:
        shrink_images.target_size(400, 200, **size)
    except ValueError:
        return
    raise AssertionError(f"expected ValueError for {size}")


def main():
    ts = shrink_images.target_size
    assert ts(400, 200, percent=25) == (300, 150)
    assert ts(400, 200, pixels=100) == (300, 150)
    assert ts(200, 400, pixels=100) == (150, 300)
    assert ts(400, 200, width=100) == (100, 50)
    assert ts(400, 200, height=50) == (100, 50)
    assert ts(400, 200, width=100, height=100) == (100, 100)
    expect_error(percent=0)
    expect_error(percent=100)
    expect_error(pixels=400)
    expect_error(width=500)

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in")
        os.makedirs(os.path.join(src, "sub"))
        Image.new("RGB", (400, 200), (255, 0, 0)).save(os.path.join(src, "a.png"))
        Image.new("RGB", (640, 480), (0, 255, 0)).save(os.path.join(src, "sub", "b.jpg"))
        out = os.path.join(tmp, "out")

        assert shrink_images.main([src, "-r", "--percent", "50", "-o", out]) == 0
        with Image.open(os.path.join(out, "a.png")) as im:
            assert im.size == (200, 100) and im.format == "PNG"
        with Image.open(os.path.join(out, "sub", "b.jpg")) as im:
            assert im.size == (320, 240) and im.format == "JPEG"

        a = os.path.join(src, "a.png")
        assert shrink_images.main([a, "--pixels", "40", "--in-place"]) == 0
        with Image.open(a) as im:
            assert im.size == (360, 180)

    print("OK")


if __name__ == "__main__":
    main()
