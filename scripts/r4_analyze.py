"""R4 lighting-ownership analysis, exactly as frozen in docs/lumen_lite_design.md R4.1.

    python scripts/r4_analyze.py <capture dir> --reference <R3 reference dir>
        [--baseline <earlier capture dir>]

Reads the raster captures scripts/capture_r4_contributions.ps1 -Phase measure
wrote, and the R3 per-seed and ensemble references, and writes r4-analysis.json
and r4-analysis.txt into the capture directory.  Nothing here touches a GPU.

Two places where R4.1 left a detail open are settled on the conservative side,
and both are printed in the report:
  - Group images (G_direct, G_indirect) are single runs, not three repeats.
    Their candidate uncertainty is borrowed from the full image's repeats, and
    their noise-corrected RMSE takes w_p = 0, which can only make it larger.
  - The owner-removal residual compares against the frame-300 full capture,
    the same frame the isolated and removed runs were taken at, while keeping
    R4.1's tolerance, which was sized for the three-repeat mean.
"""
import argparse
import json
import math
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pfm_to_png import read_pfm  # noqa: E402

SEEDS = (1337, 2026, 90210)
REPEATS = (300, 332, 364)
K = 3.0  # expanded-uncertainty coverage factor, R4.1

SCENES = {
    "cornell": "gi_cornell_box.json",
    "living-room": "living_room_showcase.json",
}
FORWARD = ["sun_diffuse", "sun_specular", "env_diffuse", "env_specular", "emission", "background",
           "emitter_diffuse", "emitter_specular"]
CONFIG_RAYS = {
    "S": ["ray_screen_hit", "ray_exit", "ray_exhausted", "ray_unusable_hit", "ray_portal"],
    "L": ["ray_screen_hit", "ray_screen_hit_cache", "ray_screen_hit_uncached", "ray_portal",
          "ray_field_cache", "ray_field_uncovered", "ray_field_exit"],
}
CONFIG_RAYS["L0"] = CONFIG_RAYS["L"]
COVERAGE_ONLY = {"S": [], "L": ["ray_field_exhausted"], "L0": ["ray_field_exhausted"]}
# Rays that partition the traced set; screen_hit_cache is a subset of screen hits.
PARTITION = {
    "S": ["ray_screen_hit", "ray_exit", "ray_exhausted", "ray_unusable_hit", "ray_portal"],
    "L": ["ray_screen_hit", "ray_screen_hit_uncached", "ray_portal", "ray_field_cache",
          "ray_field_uncovered", "ray_field_exit", "ray_field_exhausted"],
}
PARTITION["L0"] = PARTITION["L"]
# R4.5: L0 (Lumen-lite, radiosity off) is the gate; S and L are reported only.
ROLE = {"S": "legacy control, not gated", "L": "diagnostic (multi-bounce), not gated", "L0": "GATE"}
# The recovered baseline each configuration's bias and RMSE are compared with.
BASELINE_CONFIG = {"S": "S", "L": "L", "L0": "L"}


def reference_component(scene, owner, config="S", cache_screen_hits=False, hit_lighting=False):
    """R4.1 owner table: which reference image an owner is compared against."""
    if owner in ("ray_screen_hit_cache", "ray_field_exhausted"):
        return None
    if owner == "ray_field_cache":
        return "indirect"
    # R4.61: an uncovered field hit now returns its sun bounce.
    if owner == "ray_field_uncovered" and hit_lighting and config in ("L", "L0"):
        return "indirect"
    if owner in ("ray_screen_hit", "ray_screen_hit_uncached"):
        # R4.14: Lumen reads the hit surface's cache direct, a second bounce.
        # Cornell's historical R4.1 mapping predates that shader change.
        if cache_screen_hits and config in ("L", "L0"):
            return "indirect"
        return "direct" if scene == "cornell" else "indirect"
    return "direct"


def luminance(image):
    return image[..., 0] * 0.2126 + image[..., 1] * 0.7152 + image[..., 2] * 0.0722


def load_y(path):
    return luminance(read_pfm(str(path)).astype(np.float64))


def region_slice(region, height, width):
    return (slice(int(round(region["y0"] * height)), int(round(region["y1"] * height))),
            slice(int(round(region["x0"] * width)), int(round(region["x1"] * width))))


def ratio_metrics(candidate_repeats, reference_seeds, reference, u_c_override=None, w=None):
    """R4.1 mean ratio, bias and RMSE for one region.

    candidate_repeats: list of Y arrays (one per repeat, or a single run).
    reference_seeds:   list of per-seed Y arrays.  reference: ensemble Y.
    """
    n = reference.size
    seeds = np.stack(reference_seeds)
    v = seeds.var(axis=0, ddof=1) / len(reference_seeds)
    r_mean = float(reference.mean())
    u_r = max(math.sqrt(float(v.sum())) / n,
              float(np.std([s.mean() for s in reference_seeds], ddof=1)) / math.sqrt(len(reference_seeds)))
    stack = np.stack(candidate_repeats)
    c = stack.mean(axis=0)
    c_mean = float(c.mean())
    if u_c_override is not None:
        u_c = u_c_override
    elif len(candidate_repeats) > 1:
        u_c = float(np.std([x.mean() for x in candidate_repeats], ddof=1)) / math.sqrt(len(candidate_repeats))
    else:
        u_c = 0.0
    if w is None:
        w = (stack.var(axis=0, ddof=1) / len(candidate_repeats)) if len(candidate_repeats) > 1 else np.zeros_like(c)
    rho = c_mean / r_mean if r_mean != 0 else float("nan")
    u_rho = abs(rho) * math.sqrt((u_c / c_mean) ** 2 + (u_r / r_mean) ** 2) if c_mean and r_mean else float("nan")
    diff = c - reference
    mse = float((diff ** 2).mean())
    mse_corr = mse - float(v.mean()) - float(w.mean())
    return {
        "candidate_mean": c_mean,
        "reference_mean": r_mean,
        "u_candidate": u_c,
        "u_reference": u_r,
        "ratio": rho,
        "bias": rho - 1.0,
        "U": K * u_rho,
        "rel_rmse": math.sqrt(mse) / r_mean,
        "rel_rmse_corrected": math.sqrt(max(mse_corr, 0.0)) / r_mean,
        "rel_mae": float(np.abs(diff).mean()) / r_mean,
    }


def band_verdict(m, low=0.8, high=1.2):
    lo, hi = m["ratio"] - m["U"], m["ratio"] + m["U"]
    if lo >= low and hi <= high:
        return "PASS"
    if hi < low or lo > high:
        return "FAIL"
    return "INCONCLUSIVE (blocked)"


def analyse(capture, reference_dir, regions_by_scene, cache_screen_hits=False,
            primary_cache_sky=False, hit_lighting=False, linear_split=False):
    result = {"notes": __doc__.split("\n\n")[2].strip(), "scenes": {}}
    result["notes"] += ("\nScreen-hit mapping: post-R4.14 cache direct"
                        if cache_screen_hits else "\nScreen-hit mapping: historical R4.1")
    if hit_lighting:
        result["notes"] += "\nUncovered field hits: R4.61 hit-lit sun, indirect"
    for scene, scene_file in SCENES.items():
        if not any((capture / f"{scene}-{c}-full-f300.pfm").exists() for c in ROLE):
            continue
        regions = regions_by_scene[scene_file]["regions"]
        ref = {comp: load_y(reference_dir / f"{scene}-reference{suffix}.pfm")
               for comp, suffix in (("combined", ""), ("direct", ".direct"), ("indirect", ".indirect"))}
        seeds = {comp: [load_y(reference_dir / f"{scene}-seed{s}{suffix}.pfm") for s in SEEDS]
                 for comp, suffix in (("combined", ""), ("direct", ".direct"), ("indirect", ".indirect"))}
        height, width = ref["combined"].shape
        scene_result = {}
        for config in [c for c in ROLE if (capture / f"{scene}-{c}-full-f300.pfm").exists()]:
            prefix = capture / f"{scene}-{config}"
            full = [load_y(f"{prefix}-full-f{f}.pfm") for f in REPEATS]
            if full[0].shape != (height, width):
                raise SystemExit(f"{scene} {config}: candidate {full[0].shape} vs reference {(height, width)}: blocked")
            # Orientation: the candidate must agree better with the reference
            # than with its vertical flip (R4.1).
            def corr(a, b):
                return float(np.corrcoef(a.ravel(), b.ravel())[0, 1])
            orientation = {"upright": corr(full[0], ref["combined"]),
                           "flipped": corr(full[0], np.flipud(ref["combined"]))}
            rays = CONFIG_RAYS[config] + (["ray_primary_cache_sky"]
                    if primary_cache_sky and config in ("L", "L0") else [])
            owners = FORWARD + rays
            only = {o: load_y(f"{prefix}-only-{o}.pfm") for o in owners}
            without = {o: load_y(f"{prefix}-without-{o}.pfm") for o in owners}
            groups = {"direct": [o for o in owners if reference_component(scene, o, config, cache_screen_hits, hit_lighting) == "direct"],
                      "indirect": [o for o in owners if reference_component(scene, o, config, cache_screen_hits, hit_lighting) == "indirect"],
                      "none": [o for o in owners if reference_component(scene, o, config, cache_screen_hits, hit_lighting) is None]}
            group_image = {g: sum((only[o] for o in members), np.zeros_like(full[0]))
                           for g, members in groups.items()}
            config_result = {"role": ROLE[config], "orientation": orientation, "groups": groups, "regions": {}}
            for name, region in regions.items():
                ys, xs = region_slice(region, height, width)
                sl = lambda a: a[ys, xs]  # noqa: E731
                full_r = [sl(x) for x in full]
                s_c = float(np.std([x.mean() for x in full_r], ddof=1))
                u_c_full = s_c / math.sqrt(len(full_r))
                entry = {
                    "pixels": int(full_r[0].size),
                    "combined": ratio_metrics(full_r, [sl(s) for s in seeds["combined"]], sl(ref["combined"])),
                    "G_direct": ratio_metrics([sl(group_image["direct"])], [sl(s) for s in seeds["direct"]],
                                              sl(ref["direct"]), u_c_override=u_c_full),
                    "G_indirect": ratio_metrics([sl(group_image["indirect"])], [sl(s) for s in seeds["indirect"]],
                                                sl(ref["indirect"]), u_c_override=u_c_full),
                    "G_none_mean": float(sl(group_image["none"]).mean()),
                    "negative_pixels": int(sum((sl(x) < 0).sum() for x in full)),
                    "nonfinite_pixels": int(sum((~np.isfinite(sl(x))).sum() for x in full)),
                }
                for key in ("combined", "G_direct", "G_indirect"):
                    entry[key]["band_0.8_1.2"] = band_verdict(entry[key])
                full300 = float(full_r[0].mean())
                tol_owner = max(K * s_c * math.sqrt(1 / 3 + 2), 0.001 * full300)
                owners_entry = {}
                for o in owners:
                    iso = float(sl(only[o]).mean())
                    removed = float(sl(without[o]).mean())
                    e = (full300 - removed) - iso
                    # R4.5: only forward owners are tested in the final
                    # image; a ray owner's result here is a clamp diagnostic.
                    owners_entry[o] = {"isolated_mean": iso, "share_of_full": iso / full300 if full300 else float("nan"),
                                       "removal_delta": full300 - removed, "residual": e, "tolerance": tol_owner,
                                       "removal_test": ("PASS" if abs(e) <= tol_owner else "FAIL")
                                       + ("" if o in FORWARD else " (diagnostic)"),
                                       "reference_component": reference_component(scene, o, config, cache_screen_hits, hit_lighting)}
                closure = full300 - sum(v["isolated_mean"] for v in owners_entry.values())
                tol_closure = max(K * s_c * math.sqrt(1 / 3 + len(owners)), 0.001 * full300)
                entry["owners"] = owners_entry
                entry["closure"] = {"residual": closure, "tolerance": tol_closure,
                                    "test": ("PASS" if abs(closure) <= tol_closure else "FAIL") + " (diagnostic)"}
                entry["candidate_single_run_sd"] = s_c
                config_result["regions"][name] = entry
            # R4.5: ray-owner removal and closure in the raw pre-temporal
            # buffer, with R4.1's tolerance formulas on raw repeats.
            raw_full = [load_y(f"{prefix}-full-f{f}-ssgi.raw.pfm") for f in REPEATS]
            raw_only = {o: load_y(f"{prefix}-only-{o}-ssgi.raw.pfm") for o in rays}
            raw_without = {o: load_y(f"{prefix}-without-{o}-ssgi.raw.pfm") for o in rays}
            rh, rw = raw_full[0].shape
            config_result["raw_rays"] = {}
            for name, region in regions.items():
                ys, xs = region_slice(region, rh, rw)
                means = [float(x[ys, xs].mean()) for x in raw_full]
                s_c = float(np.std(means, ddof=1))
                base = means[0]
                tol = max(K * s_c * math.sqrt(1 / 3 + 2), 0.001 * base)
                owners_raw = {}
                for o in rays:
                    iso = float(raw_only[o][ys, xs].mean())
                    e = (base - float(raw_without[o][ys, xs].mean())) - iso
                    owners_raw[o] = {"isolated_mean": iso, "residual": e, "tolerance": tol,
                                     "removal_test": "PASS" if abs(e) <= tol else "FAIL"}
                closure = base - sum(v["isolated_mean"] for v in owners_raw.values())
                tol_c = max(K * s_c * math.sqrt(1 / 3 + len(rays)), 0.001 * base)
                config_result["raw_rays"][name] = {
                    "raw_mean": base, "owners": owners_raw,
                    "closure": {"residual": closure, "tolerance": tol_c,
                                "test": "PASS" if abs(closure) <= tol_c else "FAIL"}}
            # R4.62: the forward owners are linear in the final image, the ray
            # owners only before the temporal filter.  Split the full image's
            # ray part by each pixel's raw indirect fraction.
            if linear_split:
                indirect_rays = [o for o in rays if reference_component(
                    scene, o, config, cache_screen_hits, hit_lighting) == "indirect"]
                raw_ind = sum(raw_only[o] for o in indirect_rays)
                raw_all = sum(raw_only[o] for o in rays)
                forward = sum(only[o] for o in FORWARD)
                ray_part = np.mean(full, axis=0) - forward
                rows = np.arange(height) * rh // height
                cols = np.arange(width) * rw // width
                config_result["linear_split"] = {}
                for name, region in regions.items():
                    rys, rxs = region_slice(region, rh, rw)
                    total = float(raw_all[rys, rxs].sum())
                    aggregate = float(raw_ind[rys, rxs].sum()) / total if total > 0 else 0.0
                    f_raw = np.where(raw_all > 0, raw_ind / np.where(raw_all > 0, raw_all, 1.0), aggregate)
                    f = f_raw[np.ix_(rows, cols)]
                    ys, xs = region_slice(region, height, width)
                    sl = lambda a: a[ys, xs]  # noqa: E731
                    u_c = float(np.std([sl(x).mean() for x in full], ddof=1)) / math.sqrt(len(full))
                    split = {
                        "G_direct_lin": ratio_metrics([sl(forward + ray_part * (1 - f))],
                                                      [sl(x) for x in seeds["direct"]], sl(ref["direct"]),
                                                      u_c_override=u_c),
                        "G_indirect_lin": ratio_metrics([sl(ray_part * f)], [sl(x) for x in seeds["indirect"]],
                                                        sl(ref["indirect"]), u_c_override=u_c),
                    }
                    for m in split.values():
                        m["band_0.8_1.2"] = band_verdict(m)
                    split["raw_indirect_fraction"] = aggregate
                    config_result["linear_split"][name] = split
            # Ray coverage from the SSGI capture (its own, possibly half, extent).
            coverage = {}
            for ray in rays + COVERAGE_ONLY[config]:
                cov = load_y(f"{prefix}-coverage-{ray}-ssgi.indirect.pfm")
                coverage[ray] = cov
            config_result["coverage"] = {}
            ch, cw = next(iter(coverage.values())).shape
            for name, region in regions.items():
                ys, xs = region_slice(region, ch, cw)
                means = {ray: float(img[ys, xs].mean()) for ray, img in coverage.items()}
                total = sum(means[r] for r in PARTITION[config] if r in means)
                config_result["coverage"][name] = {
                    ray: {"mean": m, "share": (m / total) if total else float("nan")} for ray, m in means.items()}
            # R4.12: forward emitter cache coverage, 1 at a hit, 0.5 at a
            # miss, 0 where nothing was drawn.  Only meaningful with emitters.
            emitter_coverage = pathlib.Path(f"{prefix}-coverage-emitter_cache.pfm")
            if emitter_coverage.exists():
                img = load_y(emitter_coverage)
                config_result["emitter_cache_coverage"] = {}
                for name, region in regions.items():
                    ys, xs = region_slice(region, height, width)
                    r = img[ys, xs]
                    drawn = int((r > 0.25).sum())
                    config_result["emitter_cache_coverage"][name] = {
                        "drawn_pixels": drawn,
                        "hit_fraction": float((r > 0.75).sum()) / drawn if drawn else float("nan")}
            scene_result[config] = config_result
        result["scenes"][scene] = scene_result
    return result


def rmse_change(new_repeats, base_repeats, reference_seeds, reference):
    """R4.1 RMSE change: delta = mean[(c' - c)(c' + c - 2r)] and its U."""
    n = reference.size
    v = np.stack(reference_seeds).var(axis=0, ddof=1) / len(reference_seeds)
    c_new, c_base = np.mean(new_repeats, axis=0), np.mean(base_repeats, axis=0)
    delta = float(((c_new - c_base) * (c_new + c_base - 2 * reference)).mean())
    u_ref = 2 * math.sqrt(float(((c_new - c_base) ** 2 * v).sum())) / n
    per_repeat = [float(((a - b) * (a + b - 2 * reference)).mean()) for a, b in zip(new_repeats, base_repeats)]
    u_cand = float(np.std(per_repeat, ddof=1)) / math.sqrt(len(per_repeat))
    return delta, K * math.sqrt(u_ref ** 2 + u_cand ** 2)


def compare_to_baseline(result, baseline, capture, baseline_dir, reference_dir, regions_by_scene):
    """R4.1 bias and RMSE improvement rules against an earlier capture set."""
    out = {}
    for scene, configs in result["scenes"].items():
        regions = regions_by_scene[SCENES[scene]]["regions"]
        ref = load_y(reference_dir / f"{scene}-reference.pfm")
        seeds = [load_y(reference_dir / f"{scene}-seed{s}.pfm") for s in SEEDS]
        for config, data in configs.items():
            new_full = [load_y(capture / f"{scene}-{config}-full-f{f}.pfm") for f in REPEATS]
            base_full = [load_y(baseline_dir / f"{scene}-{BASELINE_CONFIG[config]}-full-f{f}.pfm") for f in REPEATS]
            for region, entry in data["regions"].items():
                ys, xs = region_slice(regions[region], *ref.shape)
                new = entry["combined"]
                base = baseline["scenes"][scene][BASELINE_CONFIG[config]]["regions"][region]["combined"]
                u_new, u_base = new["U"] / K, base["U"] / K
                improved = abs(new["bias"]) < abs(base["bias"]) - K * math.sqrt(u_new ** 2 + u_base ** 2)
                delta, u_delta = rmse_change([x[ys, xs] for x in new_full], [x[ys, xs] for x in base_full],
                                             [s[ys, xs] for s in seeds], ref[ys, xs])
                out[f"{scene}/{config}/{region}"] = {
                    "ratio_base": base["ratio"], "ratio_new": new["ratio"],
                    "bias_base": base["bias"], "bias_new": new["bias"], "bias_improves": improved,
                    "mse_delta": delta, "mse_delta_U": u_delta, "rmse_improves": delta < -u_delta}
    return out


def format_report(result):
    lines = ["R4 lighting-ownership analysis (docs/lumen_lite_design.md R4.1)", "", result["notes"], ""]
    for scene, configs in result["scenes"].items():
        for config, data in configs.items():
            o = data["orientation"]
            lines.append(f"== {scene} / {config} [{data['role']}] ==  orientation upright r={o['upright']:.4f} flipped r={o['flipped']:.4f}"
                         + ("" if o["upright"] > o["flipped"] else "  BLOCKED: orientation"))
            lines.append(f"   groups: direct={data['groups']['direct']}")
            lines.append(f"           indirect={data['groups']['indirect']} none={data['groups']['none']}")
            for region, e in data["regions"].items():
                lines.append(f"-- region {region} ({e['pixels']} px), candidate single-run sd {e['candidate_single_run_sd']:.3g}")
                for key in ("combined", "G_direct", "G_indirect"):
                    m = e[key]
                    lines.append(
                        f"   {key:10s} cand {m['candidate_mean']:.5g} ref {m['reference_mean']:.5g} "
                        f"ratio {m['ratio']:.4f} ± {m['U']:.4f} bias {m['bias']:+.4f} "
                        f"rRMSE {m['rel_rmse']:.4f} (corr {m['rel_rmse_corrected']:.4f}) rMAE {m['rel_mae']:.4f} "
                        f"-> {m['band_0.8_1.2']}")
                lines.append(f"   G_none mean {e['G_none_mean']:.5g}; negative {e['negative_pixels']}, nonfinite {e['nonfinite_pixels']}")
                for owner, v in sorted(e["owners"].items(), key=lambda kv: -abs(kv[1]["isolated_mean"])):
                    lines.append(
                        f"   owner {owner:20s} [{v['reference_component'] or 'none':8s}] mean {v['isolated_mean']:.5g} "
                        f"share {v['share_of_full']:+.4f} removal residual {v['residual']:+.3g} (tol {v['tolerance']:.3g}) {v['removal_test']}")
                c = e["closure"]
                lines.append(f"   closure residual {c['residual']:+.3g} (tol {c['tolerance']:.3g}) {c['test']}")
            for region, raw in data["raw_rays"].items():
                lines.append(f"   raw pre-temporal {region}: mean {raw['raw_mean']:.5g}; closure {raw['closure']['residual']:+.3g} "
                             f"(tol {raw['closure']['tolerance']:.3g}) {raw['closure']['test']}")
                for owner, v in raw["owners"].items():
                    lines.append(f"      raw {owner:20s} mean {v['isolated_mean']:.5g} residual {v['residual']:+.3g} "
                                 f"(tol {v['tolerance']:.3g}) {v['removal_test']}")
            for region, split in data.get("linear_split", {}).items():
                lines.append(f"   R4.62 linear split {region} (raw indirect fraction {split['raw_indirect_fraction']:.3f}):")
                for key in ("G_direct_lin", "G_indirect_lin"):
                    m = split[key]
                    lines.append(f"      {key:14s} cand {m['candidate_mean']:.5g} ref {m['reference_mean']:.5g} "
                                 f"ratio {m['ratio']:.4f} ± {m['U']:.4f} rRMSE {m['rel_rmse']:.4f} -> {m['band_0.8_1.2']}")
            for region, cov in data["coverage"].items():
                lines.append(f"   coverage {region}: " + ", ".join(
                    f"{ray} {v['share']:.3f}" for ray, v in cov.items()))
            lines.append("")
    if "baseline_comparison" in result:
        lines.append("== against baseline ==")
        for key, v in result["baseline_comparison"].items():
            lines.append(f"   {key}: ratio {v['ratio_base']:.4f} -> {v['ratio_new']:.4f}; "
                         f"bias improves={v['bias_improves']}; MSE delta {v['mse_delta']:+.4g} "
                         f"(U {v['mse_delta_U']:.3g}) RMSE improves={v['rmse_improves']}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--reference", type=pathlib.Path, required=True)
    parser.add_argument("--baseline", type=pathlib.Path)
    parser.add_argument("--post-r414-screen-hits", action="store_true",
                        help="classify Lumen screen-hit cache direct as one indirect bounce")
    parser.add_argument("--primary-cache-sky", action="store_true",
                        help="include the R4.26 primary cache sky owner in Lumen captures")
    parser.add_argument("--hit-lighting", action="store_true",
                        help="R4.61: classify uncovered field hits as one indirect bounce")
    parser.add_argument("--linear-split", action="store_true",
                        help="R4.62: split the full image's ray part by the raw indirect fraction")
    args = parser.parse_args()
    regions = json.loads((args.reference / "r3_regions.json").read_text(encoding="utf-8"))
    result = analyse(args.capture, args.reference, regions,
                     args.post_r414_screen_hits, args.primary_cache_sky, args.hit_lighting,
                     args.linear_split)
    if args.baseline:
        baseline = json.loads((args.baseline / "r4-analysis.json").read_text(encoding="utf-8"))
        result["baseline_comparison"] = compare_to_baseline(
            result, baseline, args.capture, args.baseline, args.reference, regions)
    (args.capture / "r4-analysis.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    report = format_report(result)
    (args.capture / "r4-analysis.txt").write_text(report, encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
