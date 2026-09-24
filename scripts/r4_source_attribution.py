"""R4.13 living-room transport diagnosis (docs/lumen_lite_design.md R4.13).

    python scripts/r4_source_attribution.py <sources capture dir> [--cache-baseline <R4.9 capture dir>]

For each isolated light source (sun, sky) it compares the path-traced
reference's direct and indirect components with the L0 candidate's direct
owners and its two indirect paths, screen hits and field-cache hits, using the
R4.1 uncertainty method.  It also splits the full-lighting cache hits by the
cache's direct source.  Measurement only; writes r4-sources.txt beside the
captures.
"""
import argparse
import pathlib
import re
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from r4_analyze import REPEATS, SEEDS, load_y, ratio_metrics, region_slice  # noqa: E402

import json

DIRECT_OWNERS = ["sun_diffuse", "sun_specular", "env_specular", "background",
                 "ray_field_exit", "ray_field_uncovered"]
INDIRECT_PATHS = ["ray_screen_hit", "ray_screen_hit_uncached", "ray_field_cache"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--regions", type=pathlib.Path,
                        default=pathlib.Path("tmp/r3-references/20260922-212524/r3_regions.json"))
    parser.add_argument("--cache-baseline", type=pathlib.Path)
    # The isolated path-traced references, if they live in an earlier run.
    parser.add_argument("--references", type=pathlib.Path)
    args = parser.parse_args()
    c = args.capture
    regions = json.loads(args.regions.read_text(encoding="utf-8"))["living_room_showcase.json"]["regions"]
    lines = ["R4.13 living-room source attribution", ""]
    for source in ("sun", "sky"):
        refs = args.references or c
        seeds = {comp: [load_y(refs / f"living-room-{source}-reference-seed{s}{suffix}.pfm") for s in SEEDS]
                 for comp, suffix in (("combined", ""), ("direct", ".direct"), ("indirect", ".indirect"))}
        full = [load_y(c / f"living-room-{source}-L0-full-f{f}.pfm") for f in REPEATS]
        owners = {o: load_y(c / f"living-room-{source}-L0-only-{o}.pfm")
                  for o in DIRECT_OWNERS + INDIRECT_PATHS}
        h, w = full[0].shape
        lines.append(f"== {source} only ==")
        for name, region in regions.items():
            ys, xs = region_slice(region, h, w)
            sl = lambda a: a[ys, xs]  # noqa: E731
            ref = {k: np.mean([sl(s) for s in v], axis=0) for k, v in seeds.items()}
            s_c = float(np.std([sl(x).mean() for x in full], ddof=1))
            u_c = s_c / np.sqrt(len(full))
            combined = ratio_metrics([sl(x) for x in full], [sl(s) for s in seeds["combined"]], ref["combined"])
            direct = sum(sl(owners[o]) for o in DIRECT_OWNERS)
            indirect = {p: sl(owners[p]) for p in INDIRECT_PATHS}
            d = ratio_metrics([direct], [sl(s) for s in seeds["direct"]], ref["direct"], u_c_override=u_c)
            i = ratio_metrics([sum(indirect.values())], [sl(s) for s in seeds["indirect"]], ref["indirect"],
                              u_c_override=u_c)
            lines.append(f"  {name}: reference direct {ref['direct'].mean():.5f} indirect {ref['indirect'].mean():.5f}; "
                         f"combined ratio {combined['ratio']:.3f} ± {combined['U']:.3f}")
            lines.append(f"    candidate direct owners {d['candidate_mean']:.5f} (ratio {d['ratio']:.3f} ± {d['U']:.3f}): "
                         + ", ".join(f"{o} {sl(owners[o]).mean():.5f}" for o in DIRECT_OWNERS))
            lines.append(f"    candidate indirect {i['candidate_mean']:.5f} (ratio {i['ratio']:.3f} ± {i['U']:.3f}): "
                         + ", ".join(f"{p} {v.mean():.5f}" for p, v in indirect.items())
                         + f"; missing {ref['indirect'].mean() - i['candidate_mean']:.5f}")
        lines.append("")
    # The cache's direct page by source, everything else lit normally.
    lines.append("== ray_field_cache by cache source (full lighting) ==")
    parts = {s: load_y(c / f"living-room-cache-{s}-L0-only-ray_field_cache.pfm") for s in ("sun", "sky")}
    whole = None
    if args.cache_baseline:
        whole = load_y(args.cache_baseline / "living-room-L0-only-ray_field_cache.pfm")
    for name, region in regions.items():
        ys, xs = region_slice(region, *parts["sun"].shape)
        text = ", ".join(f"cache {s} {v[ys, xs].mean():.5f}" for s, v in parts.items())
        if whole is not None:
            text += f"; all sources {whole[ys, xs].mean():.5f}"
        lines.append(f"  {name}: {text}")
    log = (c / "living-room-L0-card-coverage.log").read_text(encoding="utf-8", errors="replace")
    coverage = re.findall(r"Surface cache coverage:[^\n]*", log)
    lines.append("")
    lines.append("== card coverage ==")
    lines.extend("  " + line for line in coverage)
    text = "\n".join(lines) + "\n"
    (c / "r4-sources.txt").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
