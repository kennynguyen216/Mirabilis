"""IQ4 probe sampler: angular support and expected value under directional input.

Constant input (Q1) cannot see a sampler that never visits some directions:
step 2c's fixed bit-reversed (cos theta, phi) pairing sampled only 64 of the
4,096 cells, under every seed.  These checks feed the sampler a small patch of
unit radiance instead.

The sampler below mirrors ssgi_probe's lines in shaders/ssgi_body.glsl --
randomFloat(), the per-lane and per-probe seeds, the 64 cos theta strata and
the phi pattern -- in uint32 arithmetic, so it draws the shader's directions for
a given probe and frame.  It is a mirror, not the shader: a change to one must
be made to the other (the last test pins the shader lines it mirrors).

Run: python -m unittest discover -s tests
"""
import pathlib
import unittest

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
SSGI = ROOT / "shaders" / "ssgi_body.glsl"
LANE = np.arange(64, dtype=np.uint32)
REV = np.array([int(f"{i:06b}"[::-1], 2) for i in range(64)], dtype=np.uint32)


def random_float(state):
    state = state * np.uint32(747796405) + np.uint32(2891336453)
    word = ((state >> ((state >> np.uint32(28)) + np.uint32(4))) ^ state) * np.uint32(277803737)
    word = (word >> np.uint32(22)) ^ word
    return state, word.astype(np.float64) / 4294967296.0


UNIFORM_SALT, ADAPTIVE_SALT = 0, 0x9e3779b9


def probe_rays(px, py, frames, rotate=True, salt=UNIFORM_SALT):
    """cos theta and phi / 2pi of the 64 rays of the probe seeded from cell (px, py) and salt
    (a uniform probe's tile, or an adaptive probe's 4 x 4 sub-tile) for each frame seed."""
    with np.errstate(over="ignore"):
        f = np.asarray(frames, dtype=np.uint32)[:, None]
        state = (np.uint32(px * 1973) ^ np.uint32(py * 9277) ^ LANE * np.uint32(26699) ^
                 f * np.uint32(3079) ^ np.uint32(0x68bc21eb) ^ np.uint32(salt))
        state, j1 = random_float(state)
        state, j2 = random_float(state)
        cos_theta = (LANE + j1) / 64.0
        u2 = (REV + j2) / 64.0
        if rotate:
            rot_state = (np.uint32(px * 1973) ^ np.uint32(py * 9277) ^ f * np.uint32(3079) ^
                         np.uint32(0x2545f491) ^ np.uint32(salt))
            _, rotation = random_float(rot_state)
            u2 = np.mod(u2 + rotation, 1.0)
    return cos_theta, u2


# The counterexample: unit radiance on cos theta in [32/64, 33/64), phi / 2pi in [2/64, 3/64).
PATCH_TRUTH = ((33 / 64) ** 2 - (32 / 64) ** 2) / 2 * (1 / 64) * 2   # irradiance / pi


def patch_estimate(cos_theta, u2):
    """Per probe-frame irradiance / pi of the patch: (2pi / 64) sum f cos theta / pi."""
    inside = (cos_theta >= 32 / 64) & (cos_theta < 33 / 64) & (u2 >= 2 / 64) & (u2 < 3 / 64)
    return (2.0 / 64.0) * (inside * cos_theta).sum(axis=1)


def samples(rotate):
    frames = np.arange(2000)
    rays = [probe_rays(px, py, frames, rotate) for px in range(0, 60, 6) for py in range(0, 34, 4)]
    return np.concatenate([r[0] for r in rays]), np.concatenate([r[1] for r in rays])


class Sampler(unittest.TestCase):
    def test_every_cell_is_sampled(self):
        cos_theta, u2 = probe_rays(7, 11, np.arange(2000))
        cells = np.unique(np.floor(cos_theta * 64).astype(int) * 64 + np.floor(u2 * 64).astype(int))
        self.assertEqual(cells.size, 64 * 64)

    def test_patch_expected_value(self):
        estimate = patch_estimate(*samples(rotate=True))
        stderr = estimate.std() / np.sqrt(estimate.size)
        self.assertGreater(estimate.mean(), 0.0)
        self.assertLess(abs(estimate.mean() - PATCH_TRUTH), max(4 * stderr, 0.03 * PATCH_TRUTH))

    def test_adaptive_probes_have_full_support_and_their_own_rays(self):
        cos_theta, u2 = probe_rays(7, 11, np.arange(2000), salt=ADAPTIVE_SALT)
        cells = np.unique(np.floor(cos_theta * 64).astype(int) * 64 + np.floor(u2 * 64).astype(int))
        self.assertEqual(cells.size, 64 * 64)
        # The same cell numbers as a uniform probe, but different rays.
        self.assertFalse(np.array_equal(cos_theta, probe_rays(7, 11, np.arange(2000))[0]))

    def test_fixed_pairing_is_caught(self):
        # Step 2c's sampler: the same checks must fail on it.
        cos_theta, u2 = probe_rays(7, 11, np.arange(2000), rotate=False)
        cells = np.unique(np.floor(cos_theta * 64).astype(int) * 64 + np.floor(u2 * 64).astype(int))
        self.assertEqual(cells.size, 64)
        self.assertEqual(patch_estimate(*samples(rotate=False)).max(), 0.0)

    def test_mirror_matches_the_shader(self):
        text = SSGI.read_text(encoding="utf-8")
        for line in (
            "uint state = uint(seedCell.x) * 1973u ^ uint(seedCell.y) * 9277u ^ lane * 26699u ^\n"
            "        PushConstants.control.w * 3079u ^ 0x68bc21ebu ^ salt;",
            "uint rotationState = uint(seedCell.x) * 1973u ^ uint(seedCell.y) * 9277u ^\n"
            "        PushConstants.control.w * 3079u ^ 0x2545f491u ^ salt;",
            "const uint AdaptiveSalt = 0x9e3779b9u;",
            "trace_probe(probe_pixel(probe, ivec2(PushConstants.control.xy)), probe, 0u, probe);",
            "trace_probe(pixel, pixel / 4, AdaptiveSalt, adaptive_storage(int(slot)));",
            "float rotation = randomFloat(rotationState);",
            "float cosTheta = (float(lane) + randomFloat(state)) / 64.0;",
            "float u2 = fract((float(bitfieldReverse(lane) >> 26) + randomFloat(state)) / 64.0 +\n"
            "        rotation);",
            "vec3 weighted = incident * (2.0 * Pi);",
        ):
            self.assertIn(line, text)


if __name__ == "__main__":
    unittest.main()
