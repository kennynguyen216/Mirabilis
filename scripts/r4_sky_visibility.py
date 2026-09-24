"""R4.16: group the cache sky visibility check by frozen region.

    python scripts/r4_sky_visibility.py <sky-visibility.csv> [--regions r3_regions.json]

Reads the per-texel CSV MIRABILIS_R4_SKY_VISIBILITY_CHECK writes and reports,
for every texel and for the texels that project into each living-room region,
the sample shares of each field-against-BVH class and the radiance-weighted
field / exact sky irradiance.  Writes <csv>.txt beside it.
"""
import argparse
import csv
import json
import pathlib

CLASSES = ["n_agree_exit", "n_agree_blocked", "n_false_block", "n_false_exit",
           "n_unresolved_lost", "n_unresolved_harmless"]


def summarise(rows):
    samples = sum(sum(int(r[c]) for c in CLASSES) for r in rows)
    exact = sum(float(r["l_exact"]) for r in rows)
    out = {"texels": len(rows), "samples": samples}
    for c in CLASSES:
        out[c] = sum(int(r[c]) for r in rows) / samples if samples else float("nan")
    for key in ("l_field", "l_false_block", "l_false_exit", "l_unresolved_lost"):
        out[key + "_over_exact"] = sum(float(r[key]) for r in rows) / exact if exact else float("nan")
    if rows and "l_gpu" in rows[0]:
        gpu = [float(r["l_gpu"]) for r in rows]
        out["l_gpu_over_exact"] = sum(gpu) / exact if exact else float("nan")
        out["l_gpu_excess"] = sum(max(g - float(r["l_exact"]), 0.0) for g, r in zip(gpu, rows)) / exact if exact else float("nan")
        out["l_gpu_deficit"] = sum(max(float(r["l_exact"]) - g, 0.0) for g, r in zip(gpu, rows)) / exact if exact else float("nan")
        out["l_gpu_sum"] = sum(gpu)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--regions", type=pathlib.Path,
                        default=pathlib.Path("tmp/r3-references/20260922-212524/r3_regions.json"))
    args = parser.parse_args()
    rows = list(csv.DictReader(args.csv.open(encoding="utf-8")))
    regions = json.loads(args.regions.read_text(encoding="utf-8"))["living_room_showcase.json"]["regions"]
    groups = {"all texels": rows}
    for name, reg in regions.items():
        groups[f"projects into {name}"] = [
            r for r in rows if r["in_front"] == "1"
            and reg["x0"] <= float(r["screen_u"]) <= reg["x1"]
            and reg["y0"] <= float(r["screen_v"]) <= reg["y1"]]
    lines = ["R4.16 cache sky visibility: field against BVH, 32 directions per texel", ""]
    for name, members in groups.items():
        s = summarise(members)
        lines.append(f"{name}: {s['texels']} texels, {s['samples']} directions")
        lines.append("  samples: " + ", ".join(f"{c[2:]} {s[c]:.3f}" for c in CLASSES))
        lines.append(f"  radiance-weighted, over the BVH's escaping sky: field exits {s['l_field_over_exact']:.3f}, "
                     f"false blocking {s['l_false_block_over_exact']:.3f}, unresolved-lost "
                     f"{s['l_unresolved_lost_over_exact']:.3f}, false exits (leak) {s['l_false_exit_over_exact']:.3f}")
        if "l_gpu_over_exact" in s:
            lines.append(f"  GPU sky / exact {s['l_gpu_over_exact']:.4f}, excess {s['l_gpu_excess']:.4f}, "
                         f"deficit {s['l_gpu_deficit']:.4f}, GPU sum {s['l_gpu_sum']:.6f}")
    text = "\n".join(lines) + "\n"
    args.csv.with_suffix(".txt").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
