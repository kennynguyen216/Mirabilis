"""R4 ownership instrumentation has to mean what its record says it means.

The owner bits live in three places -- src/r4_contributions.h, the forward
shader's mask and the SSGI trace's ray classes -- and a capture labelled with
an owner it did not isolate would be read as evidence.  These checks read the
sources (nothing here needs a GPU) and exercise the analysis arithmetic on
synthetic images whose answer is known.

Run: python -m unittest discover -s tests
"""
import math
import pathlib
import re
import sys
import unittest

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "r4_contributions.h"
ENGINE_H = ROOT / "src" / "vk_engine.h"
MESH = ROOT / "shaders" / "mesh_shading.glsl"
SSGI = ROOT / "shaders" / "ssgi_body.glsl"
sys.path.insert(0, str(ROOT / "scripts"))
import r4_analyze  # noqa: E402


def header_bits():
    text = HEADER.read_text(encoding="utf-8")
    return {name: int(shift) for name, shift in
            re.findall(r"inline constexpr uint32_t (\w+) = 1u << (\d+);", text)}


class Instrumentation(unittest.TestCase):
    def test_ray_class_bits_match_the_header(self):
        header = header_bits()
        shader = {name: int(shift) for name, shift in
                  re.findall(r"const uint (Ray\w+) = 1u << (\d+);", SSGI.read_text(encoding="utf-8"))}
        self.assertEqual(len(shader), 12)
        for name, shift in shader.items():
            self.assertEqual(header[name], shift, name)

    def test_forward_mask_bits_match_the_header(self):
        text = MESH.read_text(encoding="utf-8")
        header = header_bits()
        for term, owner in (("directDiffuse", "SunDiffuse"), ("directSpecular", "SunSpecular"),
                            ("ambientDiffuse", "EnvDiffuse"), ("ambientSpecular", "EnvSpecular"),
                            ("emission", "Emission"), ("emitterDiffuse", "EmitterDiffuse"),
                            ("emitterSpecular", "EmitterSpecular")):
            self.assertIn(f"(keep & {1 << header[owner]}u) != 0u ? {term}", text, term)

    def test_transport_buffer_is_never_masked(self):
        """SSGI transports this; removing an owner must not change transport."""
        text = MESH.read_text(encoding="utf-8")
        self.assertIn("outDirectLighting = vec4(directDiffuse + emitterDiffuse +", text)
        self.assertNotIn("keep & ", text[text.index("outDirectLighting ="):])

    def test_defaults_keep_every_owner(self):
        header = header_bits()
        text = ENGINE_H.read_text(encoding="utf-8")
        forward = sum(1 << header[n] for n in ("SunDiffuse", "SunSpecular", "EnvDiffuse",
                                              "EnvSpecular", "Emission", "Background",
                                              "EmitterDiffuse", "EmitterSpecular"))
        rays = sum(1 << s for n, s in header.items() if n.startswith("Ray"))
        self.assertIn(f"uint32_t forward{{{hex(forward)}u}};", text)
        self.assertIn(f"uint32_t ray{{{hex(rays)}u}};", text)
        self.assertIn("bool rayCoverage{false};", text)

    def test_parser_truth_table_is_compiled(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertGreaterEqual(len(re.findall(r"static_assert\(", text)), 8)
        for rejected in ('""', '"sun_diffuse,"', '"sun"'):
            self.assertIn(f"!parse_contributions({rejected}).valid", text)


class CacheChannelContract(unittest.TestCase):
    """R4.7: cache direct owns sun, sky and emitters; indirect is reflection only."""

    DIRECT = ROOT / "shaders" / "surface_cache_direct.comp"
    RADIOSITY = ROOT / "shaders" / "surface_cache_radiosity.comp"

    SAMPLING = ROOT / "shaders" / "emitter_sampling.glsl"

    def test_one_emitter_estimator_with_the_path_tracer_convention(self):
        shared = self.SAMPLING.read_text(encoding="utf-8")
        self.assertIn("authoredSideNormal(emitter,", shared)
        self.assertIn("float u = sqrt(xi.y);", shared)
        self.assertIn("(0.5 * twiceArea) *", shared)
        self.assertNotIn("abs(dot(", shared, "sidedness must not be bypassed")
        direct = self.DIRECT.read_text(encoding="utf-8")
        self.assertIn('#include "emitter_sampling.glsl"', direct)
        self.assertIn("emitter_irradiance(world, normal, texel, visibleFraction)", direct)
        self.assertIn("light += emitterLight;", direct)
        self.assertIn("imageStore(emitterPage, texel, vec4(emitterLight, visibleFraction));", direct)
        brdf = (ROOT / "shaders" / "material_brdf.glsl").read_text(encoding="utf-8")
        self.assertIn('#include "emitter_sampling.glsl"', brdf)
        for text in (direct, brdf):
            self.assertNotIn("float cosineLight", text, "estimator copied instead of shared")

    def test_visibility_endpoints_leave_along_their_own_normals(self):
        """R4.11/R4.12: the along-ray stop left grazing segments in the emitter's shell."""
        shared = self.SAMPLING.read_text(encoding="utf-8")
        self.assertIn("visibilityTarget = lightPoint + emitterNormal * (EmitterClearanceVoxels * voxel);", shared)
        self.assertIn("for (int step = 0; step < 96 && t < span; ++step)", shared)
        self.assertNotIn("span - 2.5", shared)

    def test_forward_emitter_light_reads_the_cache_not_the_field(self):
        brdf = (ROOT / "shaders" / "material_brdf.glsl").read_text(encoding="utf-8")
        body = brdf[brdf.index("void material_emitters("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("surface_cache_lookup(worldPosition, geometricNormal)", body)
        self.assertNotIn("emitter_visibility", body)
        self.assertIn("if (cached.weight <= 0.0) {", body, "a cache miss is no estimate")
        self.assertIn("specular *= cached.directAlpha", body, "specular stays its own term")
        structures = (ROOT / "shaders" / "input_structures.glsl").read_text(encoding="utf-8")
        self.assertIn("layout(set = 0, binding = 12) uniform sampler2D cacheDirect;", structures)

    def test_emission_leaves_transport_while_emitters_are_sampled(self):
        mesh = MESH.read_text(encoding="utf-8")
        self.assertIn("(emittersSampled ? vec3(0.0) : emission)", mesh)
        self.assertIn("(sceneData.emitterSettings.x > 0.5 ? vec3(0.0) : surface.emissive)",
                      SSGI.read_text(encoding="utf-8"))

    def test_screen_hits_and_field_hits_share_the_cache_direct_source(self):
        ssgi = SSGI.read_text(encoding="utf-8")
        self.assertIn("incident = r4_ray_value(RayScreenHit,\n                    hitSurface.albedo * hitSurface.direct", ssgi)
        self.assertIn("incident = r4_ray_value(RayScreenHitUncached, screenDirect);", ssgi)

    def test_cache_sky_is_hemispherical_and_exit_only(self):
        direct = self.DIRECT.read_text(encoding="utf-8")
        self.assertIn("const uint SkySamples = 32u;", direct)
        self.assertIn("if (!blocked && t >= tExit) {", direct)
        self.assertNotIn("sky_visibility", direct)
        self.assertNotIn("sky.sh[", direct)

    def test_background_follows_the_environment_policy(self):
        sky = (ROOT / "shaders" / "sky.comp").read_text(encoding="utf-8")
        self.assertIn(": gradient_radiance(direction);", sky)
        self.assertIn("radiance * PushConstants.data1.y;", sky)

    def test_direct_sky_follows_the_environment_selection(self):
        text = self.DIRECT.read_text(encoding="utf-8")
        self.assertIn("return gradient_radiance(direction);", text)

    def test_radiosity_is_reflected_geometry_transport_only(self):
        text = self.RADIOSITY.read_text(encoding="utf-8")
        self.assertNotIn("surface.emissive", text)
        self.assertNotIn("environment_radiance", text)
        self.assertIn("sum += surface.albedo * arriving;", text)

    def test_r461_hit_lighting_shares_the_forward_shadow(self):
        """R4.61: uncovered field hits are lit by the forward pass's own shadow test."""
        ssgi = SSGI.read_text(encoding="utf-8")
        self.assertIn('#include "sun_shadow.glsl"', ssgi)
        self.assertIn("rayClass = RayFieldUncovered;\n            return lumen_hit_sun(p);", ssgi)
        structures = (ROOT / "shaders" / "input_structures.glsl").read_text(encoding="utf-8")
        self.assertIn('#include "sun_shadow.glsl"', structures)
        self.assertNotIn("float sunlight_visibility", structures, "shadow test copied, not shared")
        direct = self.DIRECT.read_text(encoding="utf-8")
        self.assertIn("sky_visible_exact(world + normal * epsilon, lightDirection)", direct)

    def test_iq2_every_shadow_caller_has_a_ray_query_build(self):
        """IQ2: with ray-query shadows on, the shadow map no longer holds the
        opaque casters, so an entry point that reaches sunlight_visibility()
        without an .rt.spv build would lose their shadows."""
        shaders = ROOT / "shaders"
        text = {p.name: p.read_text(encoding="utf-8") for p in shaders.iterdir()
                if p.suffix in (".glsl", ".frag", ".vert", ".comp")}
        callers = {n for n, t in text.items()
                   if "sunlight_visibility(" in t and n != "sun_shadow.glsl"}
        # ssgi_body.glsl includes the shadow test only for Lumen-lite.
        need = sorted(n for n, t in text.items() if not n.endswith(".glsl") and (
            n in callers or any(f'#include "{c}"' in t and (
                c != "ssgi_body.glsl" or "#define LUMEN_LITE" in t) for c in callers)))
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8-sig")
        listed = re.search(r"foreach\(NAME ([^)]*)\)", cmake).group(1).split()
        self.assertEqual(sorted(listed), need)

    def test_one_gradient_definition(self):
        shaders = ROOT / "shaders"
        owners = [p.name for p in shaders.glob("*.glsl")
                  if "const vec3 EnvironmentDownColor" in p.read_text(encoding="utf-8")]
        self.assertEqual(owners, ["environment_gradient.glsl"])


class Analysis(unittest.TestCase):
    def test_ratio_and_uncertainty_on_a_known_image(self):
        rng = np.random.default_rng(1)
        truth = np.full((40, 40), 0.5)
        seeds = [truth + rng.normal(0, 0.05, truth.shape) for _ in range(3)]
        reference = np.mean(seeds, axis=0)
        candidate = [1.1 * reference + rng.normal(0, 1e-4, truth.shape) for _ in range(3)]
        m = r4_analyze.ratio_metrics(candidate, seeds, reference)
        self.assertAlmostEqual(m["ratio"], 1.1, places=3)
        # The reference term: per-pixel variance of the mean, summed, over N.
        v = np.var(np.stack(seeds), axis=0, ddof=1) / 3
        self.assertGreaterEqual(m["u_reference"], math.sqrt(v.sum()) / truth.size - 1e-12)
        self.assertGreater(m["U"], 0.0)

    def test_rmse_change_detects_a_real_improvement(self):
        rng = np.random.default_rng(2)
        reference = np.full((30, 30), 0.4)
        seeds = [reference + rng.normal(0, 0.02, reference.shape) for _ in range(3)]
        base = [reference * 2.0 + rng.normal(0, 1e-3, reference.shape) for _ in range(3)]
        new = [reference * 1.1 + rng.normal(0, 1e-3, reference.shape) for _ in range(3)]
        delta, u = r4_analyze.rmse_change(new, base, seeds, np.mean(seeds, axis=0))
        self.assertLess(delta, -u)
        delta, u = r4_analyze.rmse_change(base, new, seeds, np.mean(seeds, axis=0))
        self.assertGreater(delta, u)

    def test_band_verdicts(self):
        self.assertEqual(r4_analyze.band_verdict({"ratio": 1.0, "U": 0.1}), "PASS")
        self.assertEqual(r4_analyze.band_verdict({"ratio": 1.5, "U": 0.1}), "FAIL")
        self.assertTrue(r4_analyze.band_verdict({"ratio": 1.18, "U": 0.05}).startswith("INCONCLUSIVE"))

    def test_owner_mapping_follows_the_frozen_table(self):
        self.assertEqual(r4_analyze.reference_component("cornell", "ray_screen_hit"), "direct")
        self.assertEqual(r4_analyze.reference_component("living-room", "ray_screen_hit"), "indirect")
        self.assertIsNone(r4_analyze.reference_component("living-room", "ray_screen_hit_cache"))
        self.assertEqual(r4_analyze.reference_component("cornell", "ray_field_cache"), "indirect")
        self.assertEqual(r4_analyze.reference_component("cornell", "ray_exit"), "direct")

    def test_post_r414_lumen_screen_hits_are_one_bounce(self):
        for owner in ("ray_screen_hit", "ray_screen_hit_uncached"):
            self.assertEqual(r4_analyze.reference_component("cornell", owner, "L0", True), "indirect")
            self.assertEqual(r4_analyze.reference_component("cornell", owner, "L", True), "indirect")
            self.assertEqual(r4_analyze.reference_component("cornell", owner, "S", True), "direct")
            self.assertEqual(r4_analyze.reference_component("cornell", owner, "L0"), "direct")

    def test_r461_uncovered_hits_are_one_bounce_only_when_hit_lit(self):
        owner = "ray_field_uncovered"
        self.assertEqual(r4_analyze.reference_component("living-room", owner, "L0", True, True), "indirect")
        self.assertEqual(r4_analyze.reference_component("living-room", owner, "L0", True), "direct")
        self.assertEqual(r4_analyze.reference_component("living-room", owner, "S", True, True), "direct")


if __name__ == "__main__":
    unittest.main()
