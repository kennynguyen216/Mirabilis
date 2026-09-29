"""IQ4 Q1 readout (docs/screen_probe_proposal.md section 9; design doc "IQ4 build step 2" and
"IQ4 step 2 correction").

Reads <scene>-L0-full-f300-ssgi.probe.pfm from a capture directory: r = 1 on a
probe-served pixel, g = the probes' own reconstruction there, b = 1 where the
pixel's cache sky covers it.  Q1 passes only if the served set is non-empty
and every served value is finite and within [0.98, 1.02].

Also reports the served fraction of non-background pixels: served / (served +
the run log's "pixels traced", the pixels that fell back to their own rays),
so a stricter probe test cannot hide a coverage loss.  Report only.

--compare-raw also reports, on uncovered served pixels, the production raw
image (-ssgi.raw.pfm, RGB mean) against g.  Both are the combined coefficient
set there, so they agree to half-float rounding.  Diagnostic only; it never
changes the verdict.  --write-violations writes every served pixel outside
the band (or non-finite) to <scene>-q1-violations.csv in the capture directory.

usage: python scripts/iq4_q1.py <capture dir> [--compare-raw] [--write-violations]
"""
import argparse
import pathlib
import re
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pfm_to_png import read_pfm  # noqa: E402

LOW, HIGH = 0.98, 1.02
SCENES = ("cornell", "living-room")


def q1(probe):
    """(passed, stats) for one H x W x 3 probe readout."""
    served = probe[..., 0] > 0.5
    g = probe[..., 1][served]
    stats = {"served": int(served.sum()), "pixels": int(served.size)}
    if g.size == 0:
        return False, {**stats, "reason": "no probe-served pixels"}
    finite = np.isfinite(g)
    # NaN and inf compare false, so they count as outside as well.
    outside = ~((g >= LOW) & (g <= HIGH))
    stats.update(nonfinite=int((~finite).sum()), outside=int(outside.sum()))
    if finite.any():
        v = g[finite]
        stats.update(mean=float(v.mean()), min=float(v.min()), max=float(v.max()),
                     p=np.percentile(v, [0.1, 1, 50, 99, 99.9]).round(4).tolist())
    return stats["nonfinite"] == 0 and stats["outside"] == 0, stats


def violations(probe):
    """(y, x, g) of every served pixel outside [LOW, HIGH] or non-finite."""
    served = probe[..., 0] > 0.5
    g = probe[..., 1]
    ys, xs = np.nonzero(served & ~((g >= LOW) & (g <= HIGH)))
    return [(int(y), int(x), float(g[y, x])) for y, x in zip(ys, xs)]


def fallback_pixels(log_text):
    """The capture log's count of pixels that traced their own rays."""
    match = re.search(r"SSGI screen trace: (\d+) pixels traced", log_text)
    if not match:
        raise ValueError("no 'SSGI screen trace' line in the run log")
    return int(match.group(1))


def compare_raw(probe, raw):
    """Largest |raw - g| / max(g, 1) on uncovered served pixels, and how many."""
    uncovered = (probe[..., 0] > 0.5) & (probe[..., 2] < 0.5)
    if not uncovered.any():
        return 0, float("nan")
    g = probe[..., 1][uncovered]
    d = np.abs(raw.mean(axis=-1)[uncovered] - g) / np.maximum(np.abs(g), 1.0)
    return int(uncovered.sum()), float(np.nanmax(d))


def analyse(directory, scenes=SCENES):
    directory = pathlib.Path(directory)
    return {s: q1(read_pfm(str(directory / f"{s}-L0-full-f300-ssgi.probe.pfm")))
            for s in scenes}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    parser.add_argument("--compare-raw", action="store_true")
    parser.add_argument("--write-violations", action="store_true")
    args = parser.parse_args()
    directory = pathlib.Path(args.directory)
    ok = True
    for scene, (passed, stats) in analyse(directory).items():
        print(scene, "PASS" if passed else "FAIL", stats)
        ok &= passed
        fallback = fallback_pixels((directory / f"{scene}-L0-full-f300.log").read_text(errors="replace"))
        print(f"  served / non-background: {stats['served']} / {stats['served'] + fallback} = "
              f"{stats['served'] / max(stats['served'] + fallback, 1):.4%}")
        if args.write_violations:
            rows = violations(read_pfm(str(directory / f"{scene}-L0-full-f300-ssgi.probe.pfm")))
            path = directory / f"{scene}-q1-violations.csv"
            path.write_text("y,x,g\n" + "".join(f"{y},{x},{g!r}\n" for y, x, g in rows))
            print(f"  violations: {len(rows)} -> {path}")
        if args.compare_raw:
            base = directory / f"{scene}-L0-full-f300-ssgi"
            n, worst = compare_raw(read_pfm(str(base) + ".probe.pfm"), read_pfm(str(base) + ".raw.pfm"))
            print(f"  diagnostic: uncovered served {n} px, max |raw - g| / max(g, 1) = {worst:.2e}")
    print("Q1", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
