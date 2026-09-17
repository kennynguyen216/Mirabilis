"""Summarise how well surface cache lighting matches the raster direct term.

usage: check_surface_cache_lighting.py <capture.pfm>

The capture is a raster capture taken with the "Surface cache atlas" debug
view on its "Screen: comparison data" page (MIRABILIS_RENDER_DEBUG_VIEW=29,
MIRABILIS_SURFACE_CACHE_PAGE=8).  Each pixel holds:
    r   1 when some card saw this G-buffer position, 0 when none did,
        -1 where nothing was drawn
    g   light arriving at the surface according to the cache (luminance)
    b   the same according to the raster pass, directLighting / albedo;
        negative where the albedo is too dark to recover it

The cache shadows through the scene distance field and the raster pass
through the shadow map, so penumbrae and contact shadows legitimately
differ; what should agree is which surfaces are lit at all and, where both
are lit, how brightly (N.L and sun colour are shared).
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
    image = read_pfm(sys.argv[1])
    found, cache, raster = image[..., 0], image[..., 1], image[..., 2]
    drawn = found >= 0.0
    covered = drawn & (found > 0.5)
    print(f"drawn pixels {int(drawn.sum())}, cache lookup found a card for "
          f"{covered.sum() / max(drawn.sum(), 1) * 100:.1f}%")

    judged = covered & (raster >= 0.0)
    if not judged.any():
        print("no pixel with a card and a recoverable raster light")
        sys.exit(1)
    threshold = 0.1
    cache_lit = cache[judged] > threshold
    raster_lit = raster[judged] > threshold
    agree = np.mean(cache_lit == raster_lit)
    print(f"lit/shadow classification agrees on {agree * 100:.1f}% of "
          f"{int(judged.sum())} judged pixels")
    print(f"  raster lit {raster_lit.mean() * 100:.1f}%, cache lit "
          f"{cache_lit.mean() * 100:.1f}%")
    print(f"  cache lit where raster shadowed: "
          f"{np.mean(cache_lit & ~raster_lit) * 100:.1f}%, "
          f"cache shadowed where raster lit: "
          f"{np.mean(~cache_lit & raster_lit) * 100:.1f}%")
    both = cache_lit & raster_lit
    if both.any():
        c = cache[judged][both]
        r = raster[judged][both]
        ratio = c / r
        print(f"where both lit: median cache/raster ratio {np.median(ratio):.3f}, "
              f"p10 {np.percentile(ratio, 10):.3f}, p90 {np.percentile(ratio, 90):.3f}")


if __name__ == "__main__":
    main()
