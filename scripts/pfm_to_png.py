"""Tonemap a linear-HDR .pfm capture (from capture_raster/capture_ssgi/
capture_path_trace) to a viewable .png.

usage: pfm_to_png.py <input.pfm> [output.png] [exposure]

Uses the same Reinhard-style operator family the engine's own tonemap pass
offers (see shaders/tonemap.frag) rather than a plain clip, so a bright
Sponza capture doesn't just blow out to white.
"""
import sys

import numpy as np
from PIL import Image


def read_pfm(path):
    with open(path, "rb") as f:
        header = f.readline().rstrip()
        if header not in (b"PF", b"Pf"):
            raise ValueError("Not a PFM file: " + path)
        channels = 3 if header == b"PF" else 1
        dims = f.readline().rstrip()
        while dims.startswith(b"#"):
            dims = f.readline().rstrip()
        width, height = (int(v) for v in dims.split())
        scale = float(f.readline().rstrip())
        endian = "<" if scale < 0 else ">"
        data = np.frombuffer(
            f.read(width * height * channels * 4), dtype=endian + "f4")
        data = data.reshape((height, width, channels))
        return np.flipud(data)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else in_path.rsplit(".", 1)[0] + ".png"
    exposure = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

    image = read_pfm(in_path)
    image = np.nan_to_num(image, nan=0.0, posinf=0.0, neginf=0.0)
    if image.shape[2] == 1:
        image = np.repeat(image, 3, axis=2)

    linear = image * exposure
    tonemapped = linear / (linear + 1.0)  # simple Reinhard
    srgb = np.where(
        tonemapped <= 0.0031308,
        tonemapped * 12.92,
        1.055 * np.power(np.clip(tonemapped, 0, None), 1.0 / 2.4) - 0.055)
    srgb8 = np.clip(srgb * 255.0 + 0.5, 0, 255).astype(np.uint8)

    Image.fromarray(srgb8, mode="RGB").save(out_path)
    print(f"Wrote {out_path} ({image.shape[1]}x{image.shape[0]})")


if __name__ == "__main__":
    main()
