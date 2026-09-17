"""Summarise how well the scene distance field matches the raster render.

usage: check_sdf_agreement.py <capture.pfm> [max scatter cm] [max bias cm]

The capture is a raster capture taken with the "SDF sphere trace" debug view
in its "Raster agreement (data)" mode (MIRABILIS_RENDER_DEBUG_VIEW=30,
MIRABILIS_SDF_VIEW_MODE=4).  Each pixel holds:
    r >= 0   both the field trace and the raster hit something: the distance
             between the two hits, in field voxels
    r = -1   the raster drew a surface the field trace missed
    r = -2   the field trace hit something the raster did not draw
    r = -3   neither hit anything
    g, b     the traced and raster hit distances, in world units

Hits are judged in world units from the traced and raster distances, split
into bias (the median signed difference) and scatter (how far pixels spread
around that bias).  The split matters: most scene meshes are baked as
two-sided shells whose surface sits half a bake voxel in front of the real
one, so every hit lands a consistent couple of centimetres early.  That is
expected and shows up as bias; a placement or orientation error shows up as
scatter.  Exits non-zero when either exceeds its limit (defaults 5 cm scatter,
the bake voxel size, and 5 cm bias), so a bounded run can use it as a check.
"""
import sys

import numpy as np


def read_pfm(path):
    with open(path, "rb") as f:
        header = f.readline().strip()
        channels = 3 if header == b"PF" else 1
        width, height = (int(v) for v in f.readline().split())
        scale = float(f.readline().strip())
        dtype = "<f4" if scale < 0 else ">f4"
        data = np.frombuffer(f.read(), dtype=dtype)
    return data.reshape(height, width, channels)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    scatter_limit = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    bias_limit = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    image = read_pfm(sys.argv[1])
    code = image[..., 0]
    total = code.size

    both = code >= 0
    raster_only = np.isclose(code, -1.0)
    field_only = np.isclose(code, -2.0)
    neither = np.isclose(code, -3.0)
    print(f"pixels {total}")
    for label, mask in [("both hit", both), ("raster only (field missed)", raster_only),
                        ("field only (raster missed)", field_only),
                        ("neither", neither)]:
        print(f"  {label:28s} {mask.sum() / total * 100:6.2f}%")

    if not both.any():
        print("no pixel where both hit; nothing to measure")
        sys.exit(1)
    traced = image[..., 1][both]
    raster = image[..., 2][both]
    signed_cm = (traced - raster) * 100.0
    bias = float(np.median(signed_cm))
    scatter = np.abs(signed_cm - bias)
    p50, p90, p99 = np.percentile(scatter, [50, 90, 99])
    print(f"signed error (traced - raster): bias {bias:+.2f} cm "
          f"(negative = field surface in front)")
    print(f"scatter around bias: median {p50:.2f} cm, p90 {p90:.2f} cm, "
          f"p99 {p99:.2f} cm")
    for threshold in (2.5, 5.0, 10.0):
        print(f"  within {threshold:4.1f} cm of bias: "
              f"{np.mean(scatter <= threshold) * 100:.2f}% of both-hit pixels")

    ok = p90 <= scatter_limit and abs(bias) <= bias_limit
    print(f"agreement {'PASS' if ok else 'FAIL'} "
          f"(p90 scatter limit {scatter_limit} cm, bias limit {bias_limit} cm)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
