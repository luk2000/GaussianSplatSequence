#!/usr/bin/env python3
"""
Gaussian Splat Sequence - command line companion of the Unreal plugin.

Works on the frame folders the plugin exports (<root>/frame_0001/sparse/0/...).
Uses exactly the same math as Source/GaussianSplatSequence/Public/GSSMath.h.

  pip install numpy OpenEXR pillow

Commands
  convert  depth EXR (+ beauty) -> points3D.txt/.bin + points.ply  (also multilayer EXRs)
  check    reprojects points3D into the camera and compares against the depth map
  train    trains every frame with LichtFeld Studio (optionally init from previous frame)

Examples
  python gss_tools.py convert --root D:/Splats/Shot010 \
      --depth "D:/Renders/Shot010/Shot010.{frame}.exr" --depth-channel "FinalImage.WorldDepth.R" \
      --color "D:/Renders/Shot010/Shot010.{frame}.png"
  python gss_tools.py check --frame D:/Splats/Shot010/frame_0001 --depth "D:/Renders/Shot010/Shot010.0001.exr"
  python gss_tools.py train --root D:/Splats/Shot010 --exe "C:/LichtFeld/LichtFeld-Studio.exe" --iter 7000 --init-from-previous --max-splats 500000
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shlex
import struct
import subprocess
import sys
from dataclasses import dataclass

import numpy as np

# ----------------------------------------------------------------------------- COLMAP


@dataclass
class Camera:
    width: int
    height: int
    fx: float
    fy: float
    cx: float
    cy: float
    qvec: np.ndarray  # w, x, y, z (world -> camera)
    tvec: np.ndarray
    name: str

    @property
    def R_w2c(self) -> np.ndarray:
        w, x, y, z = self.qvec / np.linalg.norm(self.qvec)
        return np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
        ])

    @property
    def R_c2w(self) -> np.ndarray:
        return self.R_w2c.T

    @property
    def center(self) -> np.ndarray:
        return -self.R_c2w @ self.tvec

    def rescaled(self, w: int, h: int) -> "Camera":
        sx, sy = w / self.width, h / self.height
        return Camera(w, h, self.fx * sx, self.fy * sy, self.cx * sx, self.cy * sy, self.qvec, self.tvec, self.name)


def _data_lines(path: str):
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                yield line.split()


def read_camera(sparse_dir: str) -> Camera:
    cams = {}
    for t in _data_lines(os.path.join(sparse_dir, "cameras.txt")):
        model, w, h = t[1], int(t[2]), int(t[3])
        p = list(map(float, t[4:]))
        if model == "PINHOLE":
            cams[int(t[0])] = (w, h, p[0], p[1], p[2], p[3])
        elif model in ("SIMPLE_PINHOLE", "SIMPLE_RADIAL"):
            cams[int(t[0])] = (w, h, p[0], p[0], p[1], p[2])
        else:
            raise ValueError(f"Unsupported camera model {model}")
    for t in _data_lines(os.path.join(sparse_dir, "images.txt")):
        if len(t) != 10:  # POINTS2D lines always have a multiple of 3 tokens
            continue
        w, h, fx, fy, cx, cy = cams[int(t[8])]
        return Camera(w, h, fx, fy, cx, cy, np.array(list(map(float, t[1:5]))), np.array(list(map(float, t[5:8]))), t[9])
    raise ValueError(f"No image in {sparse_dir}/images.txt")


def write_points(sparse_dir: str, xyz: np.ndarray, rgb: np.ndarray, binary: bool = True) -> None:
    n = len(xyz)
    ids = np.arange(1, n + 1)
    with open(os.path.join(sparse_dir, "points3D.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# 3D point list with one line of data per point:\n")
        f.write("#   POINT3D_ID, X, Y, Z, R, G, B, ERROR, TRACK[] as (IMAGE_ID, POINT2D_IDX)\n")
        f.write(f"# Number of points: {n}, mean track length: 0\n")
        table = np.column_stack([ids, xyz, rgb, np.zeros(n)])
        np.savetxt(f, table, fmt=["%d", "%.6f", "%.6f", "%.6f", "%d", "%d", "%d", "%d"])
    if binary:
        dt = np.dtype([("id", "<u8"), ("xyz", "<f8", 3), ("rgb", "u1", 3), ("err", "<f8"), ("track", "<u8")])
        rec = np.zeros(n, dtype=dt)
        rec["id"], rec["xyz"], rec["rgb"] = ids, xyz, rgb
        with open(os.path.join(sparse_dir, "points3D.bin"), "wb") as f:
            f.write(struct.pack("<Q", n))
            f.write(rec.tobytes())


def write_ply(path: str, xyz: np.ndarray, rgb: np.ndarray) -> None:
    dt = np.dtype([("xyz", "<f4", 3), ("rgb", "u1", 3)])
    rec = np.zeros(len(xyz), dtype=dt)
    rec["xyz"], rec["rgb"] = xyz, rgb
    with open(path, "wb") as f:
        f.write((f"ply\nformat binary_little_endian 1.0\nelement vertex {len(xyz)}\n"
                 "property float x\nproperty float y\nproperty float z\n"
                 "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n").encode())
        f.write(rec.tobytes())


def read_ply_xyz(path: str) -> np.ndarray:
    with open(path, "rb") as f:
        n = 0
        while True:
            line = f.readline().decode().strip()
            if line.startswith("element vertex"):
                n = int(line.split()[-1])
            if line == "end_header":
                break
        rec = np.frombuffer(f.read(), dtype=np.dtype([("xyz", "<f4", 3), ("rgb", "u1", 3)]), count=n)
    return rec["xyz"].astype(np.float64)


# ----------------------------------------------------------------------------- images


def read_exr_channel(path: str, channel: str) -> np.ndarray:
    import OpenEXR  # OpenEXR >= 3.3 python bindings

    with OpenEXR.File(path, separate_channels=True) as f:
        names = []
        for part in f.parts:
            chans = part.channels
            names += list(chans.keys())
            if channel in chans:
                return np.asarray(chans[channel].pixels, dtype=np.float64)
            # convenience: "R" also matches "<layer>.R" if it is unique
            matches = [k for k in chans if k.split(".")[-1] == channel]
            if len(matches) == 1:
                return np.asarray(chans[matches[0]].pixels, dtype=np.float64)
    raise KeyError(f"Channel '{channel}' not in {path}. Available: {', '.join(names)}")


def linear_to_srgb(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def read_color(path: str) -> np.ndarray:
    """Returns HxWx3 uint8 sRGB."""
    if path.lower().endswith(".exr"):
        rgb = np.stack([read_exr_channel(path, c) for c in ("R", "G", "B")], axis=-1)
        return (linear_to_srgb(rgb) * 255.0 + 0.5).astype(np.uint8)
    from PIL import Image

    return np.asarray(Image.open(path).convert("RGB"))


# ----------------------------------------------------------------------------- core math


def unproject(cam: Camera, depth: np.ndarray, depth_type: str = "planar", unit_scale: float = 0.01,
              depth_to_ue: float = 1.0, min_depth: float = 1.0, max_depth: float = 1e5,
              stride: int = 1, edge_threshold: float = 0.05):
    """Same as GSSPipeline::ConvertDepthForFrame. Returns (world_xyz Nx3, pixel_xy Nx2)."""
    h, w = depth.shape
    k = cam.rescaled(w, h)
    d = depth * depth_to_ue  # Unreal units
    valid = np.isfinite(d) & (d >= min_depth) & (d <= max_depth)
    if edge_threshold > 0:
        edge = np.zeros_like(valid)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            nb = np.full_like(d, np.nan)
            ys = slice(max(dy, 0), h + min(dy, 0))
            yd = slice(max(-dy, 0), h + min(-dy, 0))
            xs = slice(max(dx, 0), w + min(dx, 0))
            xd = slice(max(-dx, 0), w + min(-dx, 0))
            nb[yd, xd] = d[ys, xs]
            inside = np.zeros_like(valid)
            inside[yd, xd] = True
            with np.errstate(invalid="ignore"):
                edge |= inside & (~np.isfinite(nb) | (np.abs(nb - d) > edge_threshold * d))
        valid &= ~edge
    mask = np.zeros_like(valid)
    mask[::stride, ::stride] = True
    ys, xs = np.nonzero(valid & mask)
    z = d[ys, xs] * unit_scale
    rays = np.stack([(xs + 0.5 - k.cx) / k.fx, (ys + 0.5 - k.cy) / k.fy, np.ones_like(z)], axis=-1)
    if depth_type == "radial":
        z = z / np.linalg.norm(rays, axis=-1)
    pts_cam = rays * z[:, None]
    world = pts_cam @ cam.R_c2w.T + cam.center
    return world, np.stack([xs, ys], axis=-1)


def project(cam: Camera, world: np.ndarray):
    pc = (world - cam.center) @ cam.R_c2w
    u = cam.fx * pc[:, 0] / pc[:, 2] + cam.cx
    v = cam.fy * pc[:, 1] / pc[:, 2] + cam.cy
    return u, v, pc[:, 2]


# ----------------------------------------------------------------------------- helpers


def read_ue_camera(frame_dir: str) -> dict:
    info = {}
    path = os.path.join(frame_dir, "ue_camera.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if "=" in line:
                key, value = line.strip().split("=", 1)
                info[key] = value
    return info


def frame_folders(root: str, prefix: str):
    out = []
    for d in os.listdir(root):
        m = re.fullmatch(re.escape(prefix) + r"(-?\d+)", d)
        if m and os.path.exists(os.path.join(root, d, "sparse", "0", "images.txt")):
            out.append((int(m.group(1)), os.path.join(root, d)))
    return sorted(out)


def resolve(pattern: str, frame: int, padding: int, offset: int, frame_name: str) -> str:
    rf = frame + offset
    return (pattern.replace("{frame}", f"{rf:0{padding}d}").replace("{frame_raw}", str(rf))
            .replace("{frame_name}", frame_name))


# ----------------------------------------------------------------------------- commands


def convert_frame(frame_dir: str, frame: int, args) -> int:
    sparse = os.path.join(frame_dir, "sparse", "0")
    cam = read_camera(sparse)
    name = os.path.basename(os.path.normpath(frame_dir))
    unit_scale = args.unit_scale if args.unit_scale else float(read_ue_camera(frame_dir).get("unit_scale", 0.01))

    depth = read_exr_channel(resolve(args.depth, frame, args.padding, args.offset, name), args.depth_channel)
    world, pix = unproject(cam, depth, args.depth_type, unit_scale, args.depth_to_ue,
                           args.min_depth, args.max_depth, args.stride, args.edge_threshold)

    if args.max_points and len(world) > args.max_points:
        keep = np.random.default_rng(1337).choice(len(world), args.max_points, replace=False)
        world, pix = world[keep], pix[keep]

    if args.color:
        color = read_color(resolve(args.color, frame, args.padding, args.offset, name))
        ch, cw = color.shape[:2]
        dh, dw = depth.shape
        cx = np.clip(((pix[:, 0] + 0.5) * cw / dw).astype(int), 0, cw - 1)
        cy = np.clip(((pix[:, 1] + 0.5) * ch / dh).astype(int), 0, ch - 1)
        rgb = color[cy, cx]
        if not args.no_copy:
            from PIL import Image

            os.makedirs(os.path.join(frame_dir, "images"), exist_ok=True)
            Image.fromarray(color).save(os.path.join(frame_dir, "images", cam.name))
    else:
        rgb = np.full((len(world), 3), 128, dtype=np.uint8)

    write_points(sparse, world, rgb, binary=not args.text_only)
    write_ply(os.path.join(frame_dir, "points.ply"), world, rgb)
    return len(world)


def cmd_convert(args) -> int:
    if args.frame:
        folders = [(args.frame_number, args.frame)]
    else:
        folders = frame_folders(args.root, args.prefix)
    if not folders:
        print("No frame folders found.")
        return 1
    failed = 0
    for frame, folder in folders:
        try:
            n = convert_frame(folder, frame, args)
            print(f"{os.path.basename(folder)}: {n} points")
        except Exception as e:  # keep going over long sequences
            failed += 1
            print(f"{os.path.basename(folder)}: ERROR {e}")
    return 1 if failed else 0


def cmd_check(args) -> int:
    sparse = os.path.join(args.frame, "sparse", "0")
    cam = read_camera(sparse)
    world = read_ply_xyz(os.path.join(args.frame, "points.ply"))
    u, v, z = project(cam, world)
    inside = (z > 0) & (u >= 0) & (u < cam.width) & (v >= 0) & (v < cam.height)
    print(f"camera centre (dataset units): {cam.center}")
    print(f"points: {len(world)}, in front of camera and inside image: {inside.mean() * 100:.2f}%")
    if args.depth:
        depth = read_exr_channel(args.depth, args.depth_channel)
        k = cam.rescaled(depth.shape[1], depth.shape[0])
        u, v, z = project(k, world)
        xi = np.clip(np.floor(u).astype(int), 0, k.width - 1)
        yi = np.clip(np.floor(v).astype(int), 0, k.height - 1)
        unit_scale = float(read_ue_camera(args.frame).get("unit_scale", 0.01))
        ref = depth[yi, xi] * args.depth_to_ue * unit_scale
        if args.depth_type == "radial":
            rays = np.stack([(xi + 0.5 - k.cx) / k.fx, (yi + 0.5 - k.cy) / k.fy, np.ones_like(u)], -1)
            ref = ref / np.linalg.norm(rays, axis=-1)
        err = np.abs(ref - z) / np.maximum(z, 1e-9)
        print(f"relative depth error: median {np.median(err):.2e}, 99th pct {np.percentile(err, 99):.2e}")
        print(f"sub-pixel residual (should be ~0.5): median |u-floor(u)-0.5| = {np.median(np.abs(u - np.floor(u) - 0.5)):.3f}")
    return 0


def find_splat(output_dir: str, name: str) -> str | None:
    exact = os.path.join(output_dir, f"{name}.ply")
    if os.path.exists(exact):
        return exact
    plys = sorted(glob.glob(os.path.join(output_dir, "**", "*.ply"), recursive=True), key=os.path.getmtime)
    return plys[-1] if plys else None


def cmd_train(args) -> int:
    """Trains into <root>/_lichtfeld_work/<frame>, copies the final splat flat to
    <root>/<trained>/<frame>.ply and deletes the work folder (checkpoints etc.)."""
    import shutil

    folders = frame_folders(args.root, args.prefix)
    trained_root = os.path.join(args.root, args.trained)
    work_root = os.path.join(args.root, "_lichtfeld_work")
    os.makedirs(trained_root, exist_ok=True)
    prev_final = None
    for i, (_, folder) in enumerate(folders):
        name = os.path.basename(folder)
        work = os.path.join(work_root, name)
        final = os.path.join(trained_root, f"{name}.ply")
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        cmd = [args.exe, "-d", folder, "-o", work, "-i", str(args.iter), "--headless",
               "--output-name", name] + shlex.split(args.extra)
        if args.strategy:
            cmd += ["--strategy", args.strategy]
        if args.max_splats:
            cmd += ["--max-cap", str(args.max_splats)]
        if args.sh_degree >= 0:
            cmd += ["--sh-degree", str(args.sh_degree)]
        if args.sh_degree_interval and args.sh_degree != 0:
            cmd += ["--sh-degree-interval", str(args.sh_degree_interval)]
        if args.init_from_previous and prev_final and os.path.exists(prev_final):
            cmd += ["--init", prev_final]
        print(f"[{i + 1}/{len(folders)}] {' '.join(cmd)}", flush=True)
        failed = subprocess.call(cmd) != 0
        splat = find_splat(work, name)
        if splat:
            shutil.copyfile(splat, final)
            print(f"  -> {final}")
        else:
            failed = True
            print(f"  WARNING: no .ply produced for {name}")
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)
        if failed and args.stop_on_error:
            return 1
        prev_final = final
    if not args.keep_work:
        try:
            os.rmdir(work_root)
        except OSError:
            pass
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def depth_args(sp):
        sp.add_argument("--depth-channel", default="R", help="EXR channel, e.g. R or FinalImage.WorldDepth.R")
        sp.add_argument("--depth-type", choices=["planar", "radial"], default="planar")
        sp.add_argument("--depth-to-ue", type=float, default=1.0, help="EXR value -> Unreal units (cm)")

    c = sub.add_parser("convert", help="depth EXR -> COLMAP points3D")
    g = c.add_mutually_exclusive_group(required=True)
    g.add_argument("--root", help="dataset root with frame folders")
    g.add_argument("--frame", help="single frame folder")
    c.add_argument("--frame-number", type=int, default=0, help="frame number for --frame")
    c.add_argument("--prefix", default="frame_")
    c.add_argument("--padding", type=int, default=4)
    c.add_argument("--offset", type=int, default=0, help="render frame = sequence frame + offset")
    c.add_argument("--depth", required=True, help="depth EXR pattern with {frame}")
    c.add_argument("--color", help="beauty pattern with {frame} (png/jpg/exr)")
    c.add_argument("--no-copy", action="store_true", help="do not copy the beauty image into images/")
    c.add_argument("--unit-scale", type=float, default=0.0, help="default: from ue_camera.txt")
    c.add_argument("--min-depth", type=float, default=1.0)
    c.add_argument("--max-depth", type=float, default=1e5)
    c.add_argument("--stride", type=int, default=2)
    c.add_argument("--edge-threshold", type=float, default=0.05)
    c.add_argument("--max-points", type=int, default=0, help="random uniform thinning to at most N points (0 = off)")
    c.add_argument("--text-only", action="store_true")
    depth_args(c)
    c.set_defaults(func=cmd_convert)

    k = sub.add_parser("check", help="verify point cloud / camera congruence")
    k.add_argument("--frame", required=True)
    k.add_argument("--depth", help="depth EXR of that frame")
    depth_args(k)
    k.set_defaults(func=cmd_check)

    t = sub.add_parser("train", help="train all frames with LichtFeld Studio")
    t.add_argument("--root", required=True)
    t.add_argument("--exe", required=True)
    t.add_argument("--prefix", default="frame_")
    t.add_argument("--trained", default="trained")
    t.add_argument("--iter", type=int, default=7000)
    t.add_argument("--init-from-previous", action="store_true")
    t.add_argument("--max-splats", type=int, default=0, help="max Gaussians per frame (--max-cap), 0 = default")
    t.add_argument("--sh-degree", type=int, default=0, choices=[-1, 0, 1, 2, 3],
                   help="max SH degree (0 = RGB only, recommended for single-view frames; -1 = LichtFeld default)")
    t.add_argument("--sh-degree-interval", type=int, default=0, help="iterations between SH degree steps (0 = default)")
    t.add_argument("--strategy", default="mcmc", help="LichtFeld strategy (mcmc recommended for --max-splats)")
    t.add_argument("--extra", default="", help="extra LichtFeld arguments, e.g. \"--sh-degree 3\"")
    t.add_argument("--stop-on-error", action="store_true")
    t.add_argument("--keep-work", action="store_true", help="keep LichtFeld work folders (checkpoints)")
    t.set_defaults(func=cmd_train)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
