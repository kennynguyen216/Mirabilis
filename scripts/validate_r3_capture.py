"""Validate an R3 reference capture directory and define its canonical reference.

A reference nobody has decoded is not a reference: section 7.2 calls a finite
sample path trace a noisy estimate, and section 7.1 makes a capture diagnostic
only if any parity field is unknown.  So every .pfm produced by
scripts/capture_r3_references.ps1 is read back here and checked against its own
metadata, its run environment and the frozen matrix before it may be called a
reference.

    python scripts/validate_r3_capture.py tmp/r3-references/<timestamp>

Exit status: 0 accepted, 1 a check failed, 2 bad usage, 3 a check was blocked,
4 not acceptable as a reference (diagnostic manifest or dirty tree).  A canonical
reference is written only on 0.  Writes validation.txt,
noise.json and the canonical ensemble reference into the directory it was given.

Checks per capture
  - PFM header: PF (3 channels), positive dimensions, negative scale
  - every value finite, every value >= 0 (radiance cannot be negative)
  - every section 7.1 provenance field present in the sidecar
  - Release build, and a source revision matching the manifest
  - camera, scene, seed, sample count and dimensions agree with the frozen
    matrix, the manifest and the run's own .env.txt
  - direct + indirect reconstructs the combined image
  - the engine's own .bmp aligns with the .pfm, which is what proves row order

Checks per directory
  - both required scene groups are present: cornell and living-room
  - at least three distinct seeds each

Then, and only for a directory that passed, the canonical reference is written
as the per-pixel ensemble mean over the contributing seeds, so the reference is
defined before any candidate is compared against it and cannot be swapped for
whichever single seed happens to flatter a later result.
"""
import argparse
import hashlib
import json
import math
import pathlib
import re
import struct
import sys

import numpy as np

COMPONENTS = {"": "combined", ".direct": "direct", ".indirect": "indirect"}

# Section 7.4.  Both are required; a directory holding only one of them is not
# an R3 reference set, however clean that one is.
REQUIRED_SCENES = {
    "cornell": {"file": "gi_cornell_box.json", "camera": "0 2 3.5 0 0"},
    "living-room": {"file": "living_room_showcase.json", "camera": "0 1.6 -3 0 3.14"},
}

# Section 7.1: if any of these is missing the capture is diagnostic only.
REQUIRED_SIDECAR_FIELDS = [
    "GPU", "Driver integer", "Vulkan API integer", "Build",
    "Scene", "Scene hash", "Dimensions", "Render scale", "Samples", "Seed",
    "Depth", "Exposure EV", "Material model", "Portal limit",
    "Source revision/state", "Camera",
    "Sun radiance", "Environment intensity", "Emitter triangles",
]

REQUIRED_SIDE_FILES = [
    "manifest.txt", "source.txt", "binary-sha256.txt",
    "shader-sha256.txt", "scene-sha256.txt", "r3_regions.json",
    "regions-sha256.txt",
]


def read_pfm(path):
    """Decode a PFM, returning (top-down float32 array, header fields)."""
    with open(path, "rb") as file:
        header = file.readline().rstrip()
        if header not in (b"PF", b"Pf"):
            raise ValueError(f"{path.name}: not a PFM (header {header!r})")
        channels = 3 if header == b"PF" else 1
        dims = file.readline().rstrip()
        while dims.startswith(b"#"):
            dims = file.readline().rstrip()
        width, height = (int(v) for v in dims.split())
        scale = float(file.readline().rstrip())
        payload = file.read()
        expected = width * height * channels * 4
        if len(payload) != expected:
            raise ValueError(
                f"{path.name}: {len(payload)} payload bytes, expected {expected}")
        endian = "<" if scale < 0 else ">"
        data = np.frombuffer(payload, dtype=endian + "f4")
        data = data.reshape((height, width, channels))
        # PFM stores rows bottom-to-top; flip so row 0 is the top of the image.
        return np.flipud(data).copy(), {
            "channels": channels, "width": width, "height": height, "scale": scale}


def write_pfm(path, image):
    """Write in the engine's own convention, so the ensemble reads back like a capture."""
    height, width, _ = image.shape
    with open(path, "wb") as file:
        file.write(b"PF\n")
        file.write(f"{width} {height}\n".encode())
        file.write(b"-1.0\n")
        for y in range(height - 1, -1, -1):
            file.write(image[y].astype("<f4").tobytes())


def read_bmp(path):
    """Decode the engine's 24-bit bottom-up BMP without assuming a library."""
    raw = path.read_bytes()
    if raw[:2] != b"BM":
        raise ValueError(f"{path.name}: not a BMP")
    offset = struct.unpack_from("<I", raw, 10)[0]
    width, height = struct.unpack_from("<ii", raw, 18)
    bits = struct.unpack_from("<H", raw, 28)[0]
    if bits != 24:
        raise ValueError(f"{path.name}: {bits}-bit BMP, expected 24")
    stride = (width * 3 + 3) & ~3
    rows = []
    for row in range(abs(height)):
        start = offset + row * stride
        pixels = np.frombuffer(raw[start:start + width * 3], dtype=np.uint8)
        rows.append(pixels.reshape((width, 3))[:, ::-1])  # BGR -> RGB
    image = np.stack(rows)
    # Positive height means the first stored row is the bottom one.
    return np.flipud(image).copy() if height > 0 else image


def engine_tonemap(linear, exposure_ev):
    """The operator capture_path_trace applies when it writes its .bmp."""
    value = np.maximum(linear, 0.0) * (2.0 ** exposure_ev)
    value = value / (1.0 + value)
    srgb = np.where(
        value <= 0.0031308,
        12.92 * value,
        1.055 * np.power(np.clip(value, 0, None), 1.0 / 2.4) - 0.055)
    return np.clip(srgb * 255.0 + 0.5, 0, 255).astype(np.uint8)


def parse_fields(path):
    fields = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            key, _, value = line.partition(":")
            fields[key.strip()] = value.strip()
    return fields


def parse_env_file(path):
    settings = {}
    if not path.exists():
        return settings
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            settings[key.strip()] = value.strip()
    return settings


def parse_camera(text):
    """`x,y,z pitch=p yaw=y` from the sidecar, as five floats."""
    match = re.search(
        r"(-?[\d.eE+-]+),(-?[\d.eE+-]+),(-?[\d.eE+-]+)\s+pitch=(-?[\d.eE+-]+)\s+yaw=(-?[\d.eE+-]+)",
        text)
    if not match:
        return None
    return [float(value) for value in match.groups()]


def angles_agree(left, right, tolerance=1e-3):
    """Yaw is accumulated and never wrapped, so compare modulo a full turn."""
    difference = abs(left - right) % (2.0 * math.pi)
    return min(difference, 2.0 * math.pi - difference) <= tolerance


class Report:
    def __init__(self):
        self.lines = []
        self.failures = 0
        self.blocked = 0

    def check(self, ok, label, detail=""):
        if not ok:
            self.failures += 1
        self.lines.append(
            f"{'PASS' if ok else 'FAIL'}  {label}{('  ' + detail) if detail else ''}")
        return ok

    def block(self, label, detail=""):
        self.blocked += 1
        self.lines.append(f"BLOCKED  {label}{('  ' + detail) if detail else ''}")

    def note(self, text):
        self.lines.append(f"       {text}")


def region_slice(region, width, height):
    x0 = int(round(region["x0"] * width))
    x1 = int(round(region["x1"] * width))
    y0 = int(round(region["y0"] * height))
    y1 = int(round(region["y1"] * height))
    return slice(max(0, y0), min(height, max(y1, y0 + 1))), \
        slice(max(0, x0), min(width, max(x1, x0 + 1)))


def validate_capture(base, scene, seed, manifest, report, exact_revision):
    """Decode and check one capture's three PFMs, sidecar, environment and BMP."""
    expected = REQUIRED_SCENES[scene]
    sidecar = base.with_suffix(".txt")
    if not report.check(sidecar.exists(), f"{base.name}: sidecar present"):
        return None
    fields = parse_fields(sidecar)
    environment = parse_env_file(base.parent / (base.name + ".env.txt"))

    missing = [name for name in REQUIRED_SIDECAR_FIELDS if not fields.get(name)]
    report.check(not missing, f"{base.name}: section 7.1 fields complete",
                 f"missing {', '.join(missing)}" if missing else "")

    report.check(fields.get("Build") == "Release",
                 f"{base.name}: Release build", f"Build={fields.get('Build')}")

    expected_commit = manifest.get("Commit", "")
    recorded = fields.get("Source revision/state", "")
    # An accepted set has to match exactly. Prefix matching would let
    # "<commit> (dirty)" -- or any longer string beginning with the commit --
    # stand in for the commit itself, which is the one thing the field exists
    # to pin down. A diagnostic set is allowed the suffix the capture script
    # appends, because it is already labelled unusable as a reference.
    if exact_revision:
        report.check(bool(expected_commit) and recorded == expected_commit,
                     f"{base.name}: source revision equals manifest commit",
                     f"sidecar={recorded!r} manifest={expected_commit!r}")
    else:
        report.check(bool(expected_commit) and recorded.startswith(expected_commit),
                     f"{base.name}: source revision matches manifest",
                     f"sidecar={recorded!r} manifest={expected_commit!r}")

    report.check(bool(environment), f"{base.name}: run environment recorded")
    report.check(environment.get("MIRABILIS_TEST_CAMERA") == expected["camera"],
                 f"{base.name}: run camera matches frozen matrix",
                 f"env={environment.get('MIRABILIS_TEST_CAMERA')!r} "
                 f"matrix={expected['camera']!r}")
    report.check(environment.get("MIRABILIS_TEST_SCENE") == expected["file"],
                 f"{base.name}: run scene matches frozen matrix",
                 f"env={environment.get('MIRABILIS_TEST_SCENE')!r}")
    report.check(environment.get("MIRABILIS_TEST_SEED") == str(seed),
                 f"{base.name}: run seed matches filename",
                 f"env={environment.get('MIRABILIS_TEST_SEED')!r} filename={seed}")

    # The sample count has to come from the manifest, not from agreement among
    # the captures: three runs that are consistently wrong agree perfectly.
    expected_samples = manifest.get("Frames (accumulated samples per capture)")
    report.check(
        expected_samples is not None and fields.get("Samples") == expected_samples,
        f"{base.name}: sample count matches manifest",
        f"sidecar={fields.get('Samples')} manifest={expected_samples}")
    report.check(environment.get("MIRABILIS_TEST_FRAMES") == expected_samples,
                 f"{base.name}: requested frames match manifest",
                 f"env={environment.get('MIRABILIS_TEST_FRAMES')}")

    camera = parse_camera(fields.get("Camera", ""))
    if camera is None:
        report.check(False, f"{base.name}: camera parses",
                     f"Camera={fields.get('Camera')!r}")
    else:
        wanted = [float(value) for value in expected["camera"].split()]
        positions_agree = all(
            abs(camera[index] - wanted[index]) <= 1e-3 for index in range(3))
        report.check(positions_agree, f"{base.name}: rendered camera position",
                     f"sidecar={camera[:3]} matrix={wanted[:3]}")
        report.check(angles_agree(camera[3], wanted[3])
                     and angles_agree(camera[4], wanted[4]),
                     f"{base.name}: rendered camera orientation",
                     f"sidecar pitch/yaw={camera[3:]} matrix={wanted[3:]}")

    images = {}
    for suffix, label in COMPONENTS.items():
        path = base.parent / (base.name + suffix + ".pfm")
        if not report.check(path.exists(), f"{base.name}: {label} pfm present"):
            return None
        try:
            data, header = read_pfm(path)
        except ValueError as error:
            report.check(False, f"{base.name}: {label} decodes", str(error))
            return None
        images[label] = data

        report.check(header["channels"] == 3, f"{base.name}: {label} has 3 channels")
        report.check(header["width"] > 0 and header["height"] > 0,
                     f"{base.name}: {label} dimensions positive",
                     f"{header['width']}x{header['height']}")
        report.check(header["scale"] < 0, f"{base.name}: {label} is little-endian",
                     f"scale={header['scale']}")
        finite = bool(np.isfinite(data).all())
        report.check(finite, f"{base.name}: {label} all finite",
                     "" if finite else f"{int((~np.isfinite(data)).sum())} bad values")
        if finite:
            minimum = float(data.min())
            report.check(minimum >= 0.0, f"{base.name}: {label} non-negative",
                         f"min={minimum:.6g}")

        sidecar_dimensions = fields.get("Dimensions", "").replace(" ", "")
        actual = f"{header['width']}x{header['height']}"
        report.check(sidecar_dimensions == actual,
                     f"{base.name}: {label} dimensions match sidecar",
                     f"sidecar={sidecar_dimensions or '?'} file={actual}")

    report.check(fields.get("Seed") == str(seed),
                 f"{base.name}: sidecar seed matches filename",
                 f"sidecar={fields.get('Seed')} filename={seed}")
    report.check(fields.get("Scene") == expected["file"],
                 f"{base.name}: sidecar scene matches matrix",
                 f"sidecar={fields.get('Scene')} expected={expected['file']}")

    residual = float(np.abs(
        images["combined"] - (images["direct"] + images["indirect"])).max())
    report.check(residual <= 1e-4,
                 f"{base.name}: direct + indirect reconstructs combined",
                 f"max residual={residual:.3g}")

    # Orientation, against the engine's own output rather than a synthetic file.
    bmp_path = base.with_suffix(".bmp")
    if not bmp_path.exists():
        report.block(f"{base.name}: orientation", "no .bmp to align against")
    else:
        try:
            bmp = read_bmp(bmp_path)
            exposure = float(fields.get("Exposure EV", "0"))
            rendered = engine_tonemap(images["combined"], exposure)
            if bmp.shape != rendered.shape:
                report.check(False, f"{base.name}: orientation",
                             f"bmp {bmp.shape} vs pfm {rendered.shape}")
            else:
                aligned = float(np.abs(
                    bmp.astype(np.int16) - rendered.astype(np.int16)).mean())
                flipped = float(np.abs(
                    np.flipud(bmp).astype(np.int16) - rendered.astype(np.int16)).mean())
                # Absolute difference on purpose.  If the two agree the image
                # is near-symmetric and proves nothing either way; if they
                # differ it discriminates, and which one is smaller then says
                # whether the rows are the right way up.  Testing the signed
                # difference would report a genuinely inverted capture -- where
                # the flipped comparison is the better one -- as merely
                # indiscriminable, which is the one case that must fail loudly.
                if abs(flipped - aligned) < 1.0:
                    report.block(
                        f"{base.name}: orientation",
                        f"image too vertically symmetric to discriminate "
                        f"(aligned={aligned:.3f} flipped={flipped:.3f})")
                else:
                    report.check(
                        aligned < 1.0,
                        f"{base.name}: pfm rows align with engine bmp",
                        f"aligned={aligned:.3f} flipped={flipped:.3f}")
        except ValueError as error:
            report.check(False, f"{base.name}: orientation", str(error))

    return {"images": images, "fields": fields, "seed": seed, "base": base}


def cross_seed_noise(captures, regions):
    """Spread across seeds, per component, whole image and frozen regions."""
    results = {}
    for label in COMPONENTS.values():
        stack = np.stack([c["images"][label] for c in captures])
        height, width = stack.shape[1:3]
        entry = {}
        targets = {"whole": {"x0": 0.0, "y0": 0.0, "x1": 1.0, "y1": 1.0}}
        targets.update(regions)
        for name, region in targets.items():
            rows, columns = region_slice(region, width, height)
            patch = stack[:, rows, columns, :]
            mean = float(patch.mean(axis=0).mean())
            # Sample standard deviation across seeds, per pixel, then averaged.
            # Named for what it is: this is a standard deviation, not a mean
            # absolute deviation, and the two are not interchangeable.
            stddev = float(patch.std(axis=0, ddof=1).mean())
            entry[name] = {
                "mean": mean,
                "mean_seed_stddev": stddev,
                "relative_noise": (stddev / mean) if mean > 1e-12 else math.inf,
                "seed_means": [float(image.mean()) for image in patch],
            }
        results[label] = entry
    return results


HASH_RECORD = re.compile(r"^([0-9A-Fa-f]{64})\s\s+(\S.*)$")


def validate_hash_files(directory, report):
    """A manifest of hashes that is not itself well formed records nothing."""
    binary = directory / "binary-sha256.txt"
    if binary.exists():
        # Get-FileHash | Format-List writes `Hash : <64 hex>` among other lines.
        hashes = re.findall(r"(?mi)^Hash\s*:\s*([0-9A-Fa-f]{64})\s*$",
                            binary.read_text(encoding="utf-8", errors="replace"))
        report.check(bool(hashes), "directory: binary-sha256.txt holds a SHA-256",
                     f"found {len(hashes)}")

    for name in ("shader-sha256.txt", "scene-sha256.txt"):
        path = directory / name
        if not path.exists():
            continue
        lines = [line for line in path.read_text(
            encoding="utf-8", errors="replace").splitlines() if line.strip()]
        malformed = [line for line in lines if not HASH_RECORD.match(line)]
        report.check(bool(lines) and not malformed,
                     f"directory: {name} records are well-formed SHA-256",
                     f"{len(malformed)} malformed of {len(lines)}"
                     if lines else "file is empty")
        if name == "scene-sha256.txt":
            listed = {match.group(2).strip()
                      for match in (HASH_RECORD.match(line) for line in lines) if match}
            for scene in sorted(REQUIRED_SCENES):
                wanted = REQUIRED_SCENES[scene]["file"]
                report.check(wanted in listed,
                             f"directory: scene hash recorded for {scene}",
                             f"{wanted} not listed")


def clear_stale_references(directory):
    """Delete any previous run's canonical outputs before validating.

    A reference left behind by an earlier, successful run would otherwise sit
    beside captures that have since failed, and nothing in the directory would
    say the two disagree.
    """
    removed = []
    for path in sorted(directory.glob("*-reference*")):
        if path.suffix in (".pfm", ".txt") and path.is_file():
            path.unlink()
            removed.append(path.name)
    return removed


def write_ensemble_reference(directory, scene, captures, report):
    """The canonical reference: the per-pixel mean over every contributing seed.

    Defined here, before any candidate exists, so nobody can later compare
    against whichever single seed happens to flatter a result.  The seeds that
    went into it are recorded beside it.
    """
    seeds = sorted(capture["seed"] for capture in captures)
    written = []
    for suffix, label in COMPONENTS.items():
        stack = np.stack([c["images"][label] for c in captures])
        mean = stack.mean(axis=0).astype(np.float32)
        path = directory / f"{scene}-reference{suffix}.pfm"
        write_pfm(path, mean)
        written.append(path.name)

    sample = captures[0]["fields"]
    (directory / f"{scene}-reference.txt").write_text(
        "\n".join([
            f"Canonical R3 reference for {scene}",
            "",
            "Definition: per-pixel arithmetic mean of the contributing seeds.",
            "Frozen before any candidate comparison. A single seed must not be",
            "substituted for this ensemble after seeing a candidate result.",
            "",
            f"Contributing seeds: {', '.join(str(seed) for seed in seeds)}",
            f"Captures: {', '.join(c['base'].name for c in sorted(captures, key=lambda c: c['seed']))}",
            f"Files: {', '.join(written)}",
            f"Scene: {REQUIRED_SCENES[scene]['file']}",
            f"Camera: {REQUIRED_SCENES[scene]['camera']}",
            f"Dimensions: {sample.get('Dimensions')}",
            f"Samples per contributing capture: {sample.get('Samples')}",
            f"Depth: {sample.get('Depth')}",
            f"Exposure EV: {sample.get('Exposure EV')}",
            f"Source revision/state: {sample.get('Source revision/state')}",
            "",
            "Units: linear HDR radiance. No tone mapping, no exposure, no sRGB.",
            "Rows: PFM order, bottom row first, as written by the engine.",
        ]) + "\n", encoding="utf-8")
    report.check(True, f"{scene}: canonical ensemble reference written",
                 f"seeds {', '.join(str(seed) for seed in seeds)}")
    return {"seeds": seeds, "files": written}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    # Defaults to the copy frozen inside the capture directory, never the
    # repository's current one. The regions are supposed to have been fixed
    # before the captures existed; reading today's file would let an edit made
    # after the fact silently redefine what "deep interior" meant when these
    # images were judged.
    parser.add_argument("--regions", type=pathlib.Path, default=None)
    args = parser.parse_args()

    directory = args.directory
    if not directory.is_dir():
        print(f"Not a directory: {directory}")
        return 2

    regions_path = args.regions or (directory / "r3_regions.json")
    if not regions_path.is_file():
        print(f"No frozen regions file: {regions_path}")
        return 2
    regions_document = json.loads(regions_path.read_text(encoding="utf-8"))
    report = Report()

    # Before anything is judged, so a failing run cannot leave an earlier run's
    # reference standing beside it.
    removed = clear_stale_references(directory)

    for name in REQUIRED_SIDE_FILES:
        report.check((directory / name).exists(), f"directory: {name} present")
    validate_hash_files(directory, report)
    manifest = parse_fields(directory / "manifest.txt") \
        if (directory / "manifest.txt").exists() else {}

    # The frozen regions must be the ones whose hash was recorded at capture.
    recorded_regions = directory / "regions-sha256.txt"
    if recorded_regions.exists():
        wanted = re.findall(r"([0-9A-Fa-f]{64})",
                            recorded_regions.read_text(encoding="utf-8",
                                                       errors="replace"))
        actual = hashlib.sha256(regions_path.read_bytes()).hexdigest()
        report.check(bool(wanted) and actual.lower() == wanted[0].lower(),
                     "directory: frozen regions match their recorded SHA-256",
                     f"file={actual[:16]}... recorded="
                     f"{(wanted[0][:16] + '...') if wanted else '<none>'}")
    report.note(f"Regions used: {regions_path}")

    # Publication gates that have nothing to do with any single capture.
    acceptable = manifest.get("Acceptable as an R3 reference", "").strip().lower() == "yes"
    clean_tree = manifest.get("Dirty tree", "").strip() == "no"
    # An accepted set must name its own commit exactly; a diagnostic one may
    # carry the capture script's " (dirty)" suffix.
    exact_revision = acceptable and clean_tree
    if removed:
        report.note(f"Removed stale canonical outputs: {', '.join(removed)}")

    # The manifest is the record of what was asked for. If it disagrees with
    # what is on disk, one of them is wrong and neither can be trusted to
    # describe the other.
    if manifest:
        report.check(manifest.get("Build configuration") == "Release",
                     "manifest: build configuration is Release",
                     f"manifest={manifest.get('Build configuration')!r}")
    manifest_seeds = {int(value) for value in
                      re.findall(r"-?\d+", manifest.get("Seeds", ""))}

    captures = {}
    for path in sorted(directory.glob("*-seed*.pfm")):
        if path.name.endswith((".direct.pfm", ".indirect.pfm")):
            continue
        match = re.match(r"(?P<scene>.+)-seed(?P<seed>-?\d+)\.pfm$", path.name)
        if match:
            captures.setdefault(match.group("scene"), []).append(
                (int(match.group("seed")), path.with_suffix("")))

    # Both scene groups are required.  A directory holding one of them is not
    # an R3 reference set, and reporting it as clean would be the whole point
    # of the check thrown away.
    for scene in sorted(REQUIRED_SCENES):
        report.check(scene in captures, f"directory: required scene {scene} present",
                     "" if scene in captures else "no captures found")
    unexpected = sorted(set(captures) - set(REQUIRED_SCENES))
    report.check(not unexpected, "directory: no unexpected scene groups",
                 f"found {', '.join(unexpected)}" if unexpected else "")

    noise, ensembles, publishable = {}, {}, {}
    for scene in sorted(captures):
        entries = sorted(captures[scene])
        report.note(f"--- {scene}: {len(entries)} seeds "
                    f"({', '.join(str(seed) for seed, _ in entries)}) ---")
        report.check(len({seed for seed, _ in entries}) >= 3,
                     f"{scene}: at least three distinct seeds",
                     f"found {len(entries)}")
        if scene not in REQUIRED_SCENES:
            continue

        # Exact set equality, both directions: a manifest listing a seed that
        # was never captured describes a run that did not happen, and a capture
        # whose seed the manifest never names has no recorded provenance.
        captured_seeds = {seed for seed, _ in entries}
        report.check(manifest_seeds == captured_seeds,
                     f"{scene}: capture seeds equal the manifest seed set",
                     f"manifest={sorted(manifest_seeds)} captured={sorted(captured_seeds)}")

        decoded = []
        for seed, base in entries:
            result = validate_capture(base, scene, seed, manifest, report,
                                      exact_revision)
            if result:
                decoded.append(result)

        shapes = {c["images"]["combined"].shape for c in decoded}
        if len(decoded) >= 2 and report.check(
                len(shapes) == 1, f"{scene}: identical dimensions across seeds"):
            scene_regions = regions_document.get(
                REQUIRED_SCENES[scene]["file"], {}).get("regions", {})
            scene_regions = {k: v for k, v in scene_regions.items() if k != "whole"}
            noise[scene] = cross_seed_noise(decoded, scene_regions)
            # Held, not written. Publication is a decision about the whole set.
            publishable[scene] = decoded
        elif len(decoded) < 2:
            report.block(f"{scene}: cross-seed noise", "fewer than two decoded captures")

    # Phase two: publish only once every scene has been judged, and only if the
    # whole set earned it. A reference is a claim about the set, so one scene
    # passing while another fails must leave nothing behind for either -- the
    # half that passed would otherwise look like an accepted reference.
    blockers = []
    if report.failures:
        blockers.append(f"{report.failures} failed checks")
    if report.blocked:
        blockers.append(f"{report.blocked} blocked checks")
    if not acceptable:
        blockers.append("manifest does not mark the set acceptable")
    if not clean_tree:
        blockers.append(
            f"manifest dirty-tree is {manifest.get('Dirty tree', '<missing>')!r}, not 'no'")
    missing_scene = sorted(set(REQUIRED_SCENES) - set(publishable))
    if missing_scene:
        blockers.append(f"no validated captures for {', '.join(missing_scene)}")

    if blockers:
        report.note("No canonical reference written: " + "; ".join(blockers) + ".")
    else:
        for scene in sorted(publishable):
            ensembles[scene] = write_ensemble_reference(
                directory, scene, publishable[scene], report)

    lines = list(report.lines) + [""]
    for scene, components in sorted(noise.items()):
        lines.append(f"Reference noise across seeds: {scene}")
        for component, entries in components.items():
            for name, entry in entries.items():
                lines.append(
                    f"  {component:<9} {name:<18} mean={entry['mean']:.6g} "
                    f"seed-sd={entry['mean_seed_stddev']:.6g} "
                    f"relative={entry['relative_noise']:.4%}")
        lines.append("")
    lines.append(f"{report.failures} failed, {report.blocked} blocked")

    text = "\n".join(lines)
    print(text)
    (directory / "validation.txt").write_text(text + "\n", encoding="utf-8")
    (directory / "noise.json").write_text(
        json.dumps({"noise": noise, "canonical_reference": ensembles}, indent=2),
        encoding="utf-8")

    if report.failures:
        return 1
    if not acceptable or not clean_tree:
        print("\nThis capture set is not acceptable as an R3 reference: the manifest "
              "marks it diagnostic or its worktree was dirty. No canonical reference "
              "was written.")
        return 4
    if report.blocked:
        print("\nBlocked checks are not passes; resolve them before calling these "
              "captures references.")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
