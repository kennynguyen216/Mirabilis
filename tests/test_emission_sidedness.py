"""Emission must stay one-sided on every path radiance can leave a surface.

Four shader paths can emit, and a fix applied to only one of them is worse
than none: the light sampler and the camera would disagree about which way a
panel glows, and no image comparison would say which was right.  Design
section 4.4 forbids an absolute cosine at an emitter unless the material is
explicitly two-sided, and no material can be -- the engine has no sidedness
property.

These are source-text checks because the code under test is GLSL and cannot be
executed on the CPU.  They are deliberately narrow: each one names the single
construct whose return would restore a two-sided shortcut.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHADERS = ROOT / "shaders"


def source(name):
    return (SHADERS / name).read_text(encoding="utf-8")


def code_lines(name):
    """Shader lines with // comments and blank lines removed.

    The comments explain why each test exists and quote the constructs they
    forbid, so matching against raw text would find the explanation.
    """
    for line in source(name).splitlines():
        stripped = line.split("//")[0].strip()
        if stripped:
            yield stripped


class SampledEmitterCosine(unittest.TestCase):
    FILE = "path_trace_transport.glsl"

    def test_emitter_cosine_is_clamped_and_oriented(self):
        assignments = [
            line for line in code_lines(self.FILE) if "cosineLight=" in line
        ]
        self.assertEqual(len(assignments), 1, "expected one cosineLight assignment")
        self.assertIn(
            "max(dot(",
            assignments[0],
            "sampled emitters need a clamped oriented cosine",
        )
        self.assertNotIn(
            "abs(dot(",
            assignments[0],
            "abs(dot()) makes a back-facing emitter light the room anyway",
        )

    def test_a_back_facing_emitter_sample_is_still_rejected(self):
        self.assertTrue(
            any("cosineLight<=1e-8" in line for line in code_lines(self.FILE)),
            "the zero cosine has to drop the sample, not divide by it",
        )

    def test_camera_and_bsdf_emission_are_gated_on_facing(self):
        """bounce==0 is camera-visible emission and previousDelta is the
        BSDF-hit case after a specular bounce; both read the same line."""
        emission = [
            line
            for line in code_lines(self.FILE)
            if "material.emission.rgb" in line and "addContribution" in line
        ]
        self.assertEqual(len(emission), 1, "expected one direct emission line")
        self.assertIn("bounce==0||previousDelta", emission[0])
        self.assertIn(
            "&&emittingSide",
            emission[0],
            "camera-visible and BSDF-hit emission must be one-sided too",
        )

    def test_facing_is_decided_before_the_normal_is_flipped(self):
        """`geometric` is negated for a back hit a couple of lines later, so a
        side test computed after that flip would always pass."""
        lines = list(code_lines(self.FILE))
        decide = next(i for i, l in enumerate(lines) if "bool emittingSide=" in l)
        flip = next(i for i, l in enumerate(lines) if "geometric=front?geometric:-" in l)
        emit = next(i for i, l in enumerate(lines) if "material.emission.rgb" in l)
        self.assertLess(decide, emit)
        self.assertLess(emit, flip)


class OneNormalConventionEverywhere(unittest.TestCase):
    """Raster, surface cache and path tracer must agree on which side emits.

    The forward pass classifies from the interpolated vertex normal and the
    card capture from `vertex.normal`; the path tracer's `geometricNormal()`
    is the winding cross product.  Those disagree in sign whenever a transform
    is mirrored -- winding reverses, transpose(inverse(linear)) does not -- or
    when imported glTF winding is inconsistent with its authored normals.  A
    surface like that would glow in the raster image and emit from the
    opposite face in the reference trace, so no image comparison could settle
    which renderer was right.
    """

    def test_the_shared_helper_consults_the_authored_vertex_normals(self):
        body = re.search(
            r"vec3 authoredSideNormal\(([^)]*)\)\s*\{(.*?)\n\}",
            source("path_trace_intersect.glsl"),
            re.S,
        )
        self.assertIsNotNone(
            body, "authoredSideNormal() is the one place the convention is decided"
        )
        code = body.group(2)
        for vertex_normal in ("t.n0", "t.n1", "t.n2"):
            self.assertIn(vertex_normal, code, "sidedness must come from the authored normals")
        self.assertIn(
            "-geometric",
            code,
            "only the sign is taken from the authored normals; the area PDF "
            "still needs the plane normal",
        )

    def test_both_path_tracer_emission_sites_use_it(self):
        lines = list(code_lines("path_trace_transport.glsl"))
        sampled = [l for l in lines if "cosineLight=" in l]
        direct = [l for l in lines if "bool emittingSide=" in l]
        self.assertEqual(len(sampled), 1)
        self.assertEqual(len(direct), 1)
        # The sampled emitter reads it one line earlier, into emitterNormal.
        emitter_normal = [l for l in lines if "vec3 emitterNormal=" in l]
        self.assertEqual(len(emitter_normal), 1)
        self.assertIn("authoredSideNormal(emitter,", emitter_normal[0])
        self.assertIn("emitterNormal", sampled[0])
        self.assertIn("authoredSideNormal(t,", direct[0])

    def test_no_emission_site_tests_the_bare_winding_normal(self):
        lines = list(code_lines("path_trace_transport.glsl"))
        emitter_normal = next(l for l in lines if "vec3 emitterNormal=" in l)
        self.assertNotIn(
            "crossEdges/twiceArea,-normalize",
            emitter_normal,
            "the raw winding normal must not decide the emitting side",
        )
        direct = next(l for l in lines if "bool emittingSide=" in l)
        self.assertNotRegex(
            direct,
            r"emittingSide=dot\(direction,geometric\)",
            "the raw winding normal must not decide the emitting side",
        )

    def test_raster_and_cache_classify_from_the_authored_normal(self):
        """The two rasterised paths must keep using vertex normals, or the
        helper above would be matching a convention nothing else uses."""
        forward = [
            l for l in code_lines("mesh_shading.glsl") if "material_emission(" in l
        ]
        self.assertEqual(len(forward), 1)
        self.assertIn("geometricNormal", forward[0])
        # mesh_shading derives that name straight from the interpolated normal.
        self.assertTrue(
            any(
                "geometricNormal = normalize(inNormal)" in l
                for l in code_lines("mesh_shading.glsl")
            ),
            "the forward pass must classify from the authored vertex normal",
        )
        capture = " ".join(code_lines("surface_card_capture.vert"))
        self.assertIn("outAuthoredFacing = facing", capture)
        self.assertIn("float facing = dot(vertex.normal", capture)


class RasterEmission(unittest.TestCase):
    FILE = "material_brdf.glsl"

    def test_material_emission_tests_the_viewer_side(self):
        body = re.search(
            r"vec3 material_emission\((.*?)\)\s*\{(.*?)\n\}", source(self.FILE), re.S
        )
        self.assertIsNotNone(body, "material_emission() not found")
        arguments, code = body.group(1), body.group(2)
        self.assertIn("geometricNormal", arguments)
        self.assertIn("worldPosition", arguments)
        code = "\n".join(l.split("//")[0] for l in code.splitlines())
        self.assertIn("dot(geometricNormal", code)
        self.assertIn("vec3(0.0)", code, "the back face must emit nothing")

    def test_both_forward_passes_pass_the_geometry_in(self):
        """An unqualified material_emission() call would no longer compile, but
        a caller could still pass the shading normal, which a normal map tilts."""
        for name in ("mesh_shading.glsl", "portal_view_shading.glsl"):
            calls = [l for l in code_lines(name) if "material_emission(" in l]
            self.assertEqual(len(calls), 1, f"{name}: expected one call")
            self.assertIn("material_emission(geometricNormal, inWorldPosition)", calls[0])


class SurfaceCacheEmission(unittest.TestCase):
    """The Lumen-lite world fallback reads emission from the card atlas, so a
    two-sided capture there leaks emission back in after the other three paths
    are fixed."""

    def test_emissive_page_follows_the_authored_normal(self):
        # The write spans several lines, so match the whole statement.
        code = " ".join(code_lines("surface_card_capture.frag"))
        write = re.search(r"outEmissive\s*=\s*([^;]*);", code)
        self.assertIsNotNone(write, "outEmissive is never assigned")
        self.assertIn("inAuthoredFacing > 0.0", write.group(1))
        self.assertIn("vec3(0.0)", write.group(1))

    def test_authored_facing_is_captured_before_the_two_sided_flip(self):
        lines = list(code_lines("surface_card_capture.vert"))
        emitted = next(i for i, l in enumerate(lines) if "outAuthoredFacing =" in l)
        flip = next(i for i, l in enumerate(lines) if "facing = -facing" in l)
        self.assertLess(
            emitted, flip, "the emissive side must be the pre-flip authored side"
        )

    def test_reflectance_capture_is_still_two_sided(self):
        """Only emission changed: a thin wall must still be readable from both
        sides or the world fallback loses coverage it had before."""
        code = " ".join(code_lines("surface_card_capture.vert"))
        self.assertIn("PushConstants.twoSided != 0u", code)
        self.assertIn("normal = -normal", code)


if __name__ == "__main__":
    unittest.main()
