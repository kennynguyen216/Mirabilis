"""Compare raster direct lighting with the path tracer's direct image.

usage: compare_parity.py <raster-diffuse.pfm> <raster-specular.pfm> <trace.direct.pfm> [out.png]

The raster images are the forward-written "Direct diffuse only" and "Direct
specular only" debug views; their sum is the raster sun term.  The path
tracer's direct image holds the same sun term when its environment is black
and the scene has no emitters.

Pixels are compared only where both renderers see a lit surface and away from
silhouettes and shadow boundaries, because the tracer jitters samples inside
each pixel while raster samples the centre, and the tracer shadows while the
reference scene's raster pass does not.
"""
import sys

import numpy as np


def read_pfm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
    return data[: width * height * 3].reshape(height, width, 3)[::-1]


def luminance(image):
    return image @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def main():
    diffuse = read_pfm(sys.argv[1])
    specular = read_pfm(sys.argv[2])
    trace = read_pfm(sys.argv[3])
    raster = diffuse + specular
    raster_l = luminance(raster)
    trace_l = luminance(trace)

    lit = (raster_l > 1e-3) & (trace_l > 1e-3)
    # Stable neighbourhoods only: every 8-neighbour within 15% of the centre in
    # both images, which drops silhouettes, shadow edges and highlight rims.
    stable = lit.copy()
    for image in (raster_l, trace_l):
        padded = np.pad(image, 1, mode="edge")
        centre = np.maximum(image, 1e-6)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                neighbour = padded[1 + dy:padded.shape[0] - 1 + dy,
                                   1 + dx:padded.shape[1] - 1 + dx]
                stable &= np.abs(neighbour - image) <= 0.15 * centre
    relative = np.abs(raster_l - trace_l) / np.maximum(trace_l, 1e-6)
    values = relative[stable]
    ratio = raster[stable].sum(axis=0) / np.maximum(trace[stable].sum(axis=0), 1e-6)
    print("pixels compared: {} of {} lit ({:.1f}% of image)".format(
        values.size, int(lit.sum()), 100.0 * values.size / lit.size))
    for name, q in (("median", 50), ("p90", 90), ("p99", 99)):
        print("relative luminance error {}: {:.4f}".format(name, np.percentile(values, q)))
    print("mean relative error: {:.4f}".format(values.mean()))
    print("raster/trace energy ratio r g b: {:.4f} {:.4f} {:.4f}".format(*ratio))

    if len(sys.argv) > 4:
        # Error heat map: black = excluded, green->red = 0 -> 25% error.
        import struct, zlib
        h, w = relative.shape
        err = np.clip(relative / 0.25, 0, 1)
        rgb = np.zeros((h, w, 3), dtype=np.uint8)
        rgb[..., 0] = (err * 255).astype(np.uint8)
        rgb[..., 1] = ((1 - err) * 255).astype(np.uint8)
        rgb[~stable] = 0
        raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

        def chunk(tag, body):
            c = tag + body
            return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

        with open(sys.argv[4], "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
            f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
