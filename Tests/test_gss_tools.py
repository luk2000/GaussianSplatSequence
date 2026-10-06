#!/usr/bin/env python3
"""
End-to-end test of Tools/gss_tools.py without Unreal:
ray-casts a synthetic scene in *Unreal* conventions (left-handed, Z up, cm),
writes a depth EXR + COLMAP camera exactly like the plugin, converts it and checks
that every point lands on the ground-truth geometry in dataset space.

  pip install numpy OpenEXR pillow
  python Tests/test_gss_tools.py
"""

import os
import subprocess
import sys
import tempfile

import numpy as np
import OpenEXR

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "Tools", "gss_tools.py")
sys.path.insert(0, os.path.dirname(TOOLS))
import gss_tools  # noqa: E402

W, H, HFOV = 320, 180, 70.0
UNIT = 0.01
WORLD_AXES = np.array([[0, 1, 0], [0, 0, -1], [1, 0, 0]], dtype=float)  # OpenCV_YDown
UE_TO_CV_CAM = np.array([[0, 1, 0], [0, 0, -1], [1, 0, 0]], dtype=float)


def ue_axes(pitch, yaw, roll):
    d = np.pi / 180
    sp, cp, sy, cy, sr, cr = np.sin(pitch * d), np.cos(pitch * d), np.sin(yaw * d), np.cos(yaw * d), np.sin(roll * d), np.cos(roll * d)
    fwd = np.array([cp * cy, cp * sy, sp])
    right = np.array([sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp])
    up = np.array([-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp])
    return fwd, right, up


def rot_to_quat(r):
    t = np.trace(r)
    if t > 0:
        s = np.sqrt(t + 1) * 2
        q = [0.25 * s, (r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s]
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = np.sqrt(1 + r[0, 0] - r[1, 1] - r[2, 2]) * 2
        q = [(r[2, 1] - r[1, 2]) / s, 0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s]
    elif r[1, 1] > r[2, 2]:
        s = np.sqrt(1 + r[1, 1] - r[0, 0] - r[2, 2]) * 2
        q = [(r[0, 2] - r[2, 0]) / s, (r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s]
    else:
        s = np.sqrt(1 + r[2, 2] - r[0, 0] - r[1, 1]) * 2
        q = [(r[1, 0] - r[0, 1]) / s, (r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s]
    q = np.array(q)
    return q * np.sign(q[0])


def export_frame(frame_dir, loc, fwd, right, up):
    """Python port of GSSPipeline ExportFrame (PoseFromUnreal + ToColmapExtrinsics)."""
    fx = 0.5 * W / np.tan(np.radians(HFOV) / 2)
    r_c2w = WORLD_AXES @ np.column_stack([fwd, right, up]) @ UE_TO_CV_CAM.T
    c = WORLD_AXES @ loc * UNIT
    r_w2c = r_c2w.T
    t = -r_w2c @ c
    q = rot_to_quat(r_w2c)
    sparse = os.path.join(frame_dir, "sparse", "0")
    os.makedirs(sparse, exist_ok=True)
    with open(os.path.join(sparse, "cameras.txt"), "w") as f:
        f.write(f"1 PINHOLE {W} {H} {fx:.10f} {fx:.10f} {W / 2:.10f} {H / 2:.10f}\n")
    with open(os.path.join(sparse, "images.txt"), "w") as f:
        f.write("1 " + " ".join(f"{v:.12f}" for v in (*q, *t)) + f" 1 {os.path.basename(frame_dir)}.png\n\n")
    with open(os.path.join(frame_dir, "ue_camera.txt"), "w") as f:
        f.write(f"unit_scale={UNIT}\n")
    return fx


def render_depth(loc, fwd, right, up, fx, radial):
    """Ray-cast in Unreal space: floor at Z=0 and a wall at X=3000. Returns (depth, hit_points_ue)."""
    ys, xs = np.mgrid[0:H, 0:W]
    a = (xs + 0.5 - W / 2) / fx
    b = -(ys + 0.5 - H / 2) / fx  # image down = UE up negative
    dirs = fwd[None, None] + a[..., None] * right[None, None] + b[..., None] * up[None, None]
    t_floor = np.where(dirs[..., 2] < -1e-9, (0 - loc[2]) / np.where(dirs[..., 2] == 0, 1, dirs[..., 2]), np.inf)
    t_wall = np.where(dirs[..., 0] > 1e-9, (3000 - loc[0]) / np.where(dirs[..., 0] == 0, 1, dirs[..., 0]), np.inf)
    t = np.minimum(t_floor, t_wall)  # planar depth, because the forward component of dirs is 1
    t = np.where(t > 0, t, np.inf)
    hits = loc + dirs * t[..., None]
    depth = t * np.linalg.norm(dirs, axis=-1) if radial else t
    return depth.astype(np.float32), hits


def write_exr(path, depth):
    rgba = np.stack([depth, depth, depth, np.ones_like(depth)], axis=-1).astype(np.float32)
    with OpenEXR.File({"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage},
                      {"RGBA": rgba}) as f:
        f.write(path)


def run_case(tmp, name, loc, rot, radial):
    frame_dir = os.path.join(tmp, name)
    fwd, right, up = ue_axes(*rot)
    fx = export_frame(frame_dir, np.array(loc, float), fwd, right, up)
    depth, _ = render_depth(np.array(loc, float), fwd, right, up, fx, radial)
    exr = os.path.join(tmp, name + ".exr")
    write_exr(exr, depth)

    cmd = [sys.executable, TOOLS, "convert", "--frame", frame_dir, "--depth", exr, "--stride", "1",
           "--edge-threshold", "0", "--max-depth", "1e7", "--depth-type", "radial" if radial else "planar",
           "--text-only"]
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL)

    pts = gss_tools.read_ply_xyz(os.path.join(frame_dir, "points.ply"))
    # back to Unreal space: WORLD_AXES is a permutation with signs -> inverse = transpose
    ue = (pts / UNIT) @ WORLD_AXES  # row-vector form of WORLD_AXES.T @ p
    on_floor = np.abs(ue[:, 2]) < 0.5
    on_wall = np.abs(ue[:, 0] - 3000) < 0.5
    ok = on_floor | on_wall
    assert len(pts) > 0.5 * W * H, f"{name}: too few points {len(pts)}"
    assert ok.all(), f"{name}: {np.count_nonzero(~ok)} points off the geometry, e.g. {ue[~ok][:3]}"

    # and the points re-project to their own pixel centres
    cam = gss_tools.read_camera(os.path.join(frame_dir, "sparse", "0"))
    u, v, _ = gss_tools.project(cam, pts)
    frac = np.abs(u - np.floor(u) - 0.5).max(), np.abs(v - np.floor(v) - 0.5).max()
    assert max(frac) < 1e-3, f"{name}: reprojection off pixel centre by {frac}"
    print(f"{name}: {len(pts)} points OK")


def run_falloff_case(tmp):
    """Radial falloff: centre fully kept, border thinned to ~edge density, identical across runs."""
    name = "frame_0100"
    frame_dir = os.path.join(tmp, name)
    fwd, right, up = ue_axes(-40, 0, 0)  # looking down at the floor -> every pixel valid
    loc = np.array([0, 0, 500.0])
    fx = export_frame(frame_dir, loc, fwd, right, up)
    depth, _ = render_depth(loc, fwd, right, up, fx, radial=False)
    exr = os.path.join(tmp, name + ".exr")
    write_exr(exr, depth)
    cmd = [sys.executable, TOOLS, "convert", "--frame", frame_dir, "--depth", exr, "--stride", "1",
           "--edge-threshold", "0", "--max-depth", "1e7", "--text-only",
           "--radial-falloff", "--falloff-inner", "0.4", "--falloff-edge", "0.05", "--falloff-exponent", "2"]

    def pixels():
        subprocess.check_call(cmd, stdout=subprocess.DEVNULL)
        cam = gss_tools.read_camera(os.path.join(frame_dir, "sparse", "0"))
        u, v, _ = gss_tools.project(cam, gss_tools.read_ply_xyz(os.path.join(frame_dir, "points.ply")))
        return np.floor(u).astype(int), np.floor(v).astype(int)

    u, v = pixels()
    assert np.isfinite(depth).all(), "test scene must cover the whole image"
    nx, ny = ((u + 0.5) / W - 0.5) * 2, ((v + 0.5) / H - 0.5) * 2
    r = np.sqrt(nx * nx + ny * ny)
    ys, xs = np.mgrid[0:H, 0:W]
    rr = np.sqrt((((xs + 0.5) / W - 0.5) * 2) ** 2 + (((ys + 0.5) / H - 0.5) * 2) ** 2)
    centre = np.count_nonzero(r < 0.4) / np.count_nonzero(rr < 0.4)
    border = np.count_nonzero(r > 1.0) / np.count_nonzero(rr > 1.0)
    assert centre == 1.0, f"centre density {centre}"
    assert 0.02 < border < 0.09, f"border density {border}"
    u2, v2 = pixels()
    assert np.array_equal(np.sort(u * H + v), np.sort(u2 * H + v2)), "falloff pattern must be deterministic"
    print(f"{name}: radial falloff OK (centre {centre:.0%}, border {border:.1%})")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        run_case(tmp, "frame_0000", (0, 0, 170), (-10, 0, 0), radial=False)
        run_case(tmp, "frame_0001", (-400, 250, 300), (-25, 20, 5), radial=False)
        run_case(tmp, "frame_0002", (100, -300, 220), (-15, -30, -8), radial=True)
        run_falloff_case(tmp)
    print("All gss_tools tests passed.")


if __name__ == "__main__":
    main()
