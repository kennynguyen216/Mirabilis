"""The Cornell ceiling emitter has to face the room it is supposed to light.

The emitter is the built-in "floor" quad: a unit quad in the XZ plane whose
vertex normals and winding both give +Y (src/vk_engine_resources.cpp,
init_default_meshes).  Authored at the ceiling with no rotation it emitted
upward, into the sealed ceiling slab 0.02 above it, and the room was lit only
because the path tracer takes an absolute cosine at the light and the raster
passes cull nothing.  That made the scene useless as an orientation test.

This mirrors to_matrix() in src/scene.cpp -- yaw, then pitch, then roll -- so
re-authoring the scene in the editor and saving rotation back to zero fails
here rather than in an image nobody diffs.

Run: python -m unittest discover -s tests
"""
import json
import math
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENE = ROOT / "assets" / "scenes" / "gi_cornell_box.json"

# src/vk_engine_resources.cpp: init_default_meshes(), the "floor" asset.
QUAD_CORNERS = [
    (-0.5, 0.0, -0.5),
    (0.5, 0.0, -0.5),
    (0.5, 0.0, 0.5),
    (-0.5, 0.0, 0.5),
]
QUAD_TRIANGLES = [(0, 2, 1), (0, 3, 2)]
QUAD_VERTEX_NORMAL = (0.0, 1.0, 0.0)


def rotation_matrix(rotation):
    """to_matrix() in src/scene.cpp, rotation only, as row-major 3x3."""
    def axis(angle, a):
        c, s = math.cos(angle), math.sin(angle)
        if a == "y":
            return ((c, 0, s), (0, 1, 0), (-s, 0, c))
        if a == "x":
            return ((1, 0, 0), (0, c, -s), (0, s, c))
        return ((c, -s, 0), (s, c, 0), (0, 0, 1))

    def multiply(m, n):
        return tuple(
            tuple(sum(m[r][k] * n[k][c] for k in range(3)) for c in range(3))
            for r in range(3)
        )

    return multiply(
        multiply(axis(rotation[1], "y"), axis(rotation[0], "x")), axis(rotation[2], "z")
    )


def apply(matrix, vector):
    return tuple(sum(matrix[r][c] * vector[c] for c in range(3)) for r in range(3))


def scaled(vector, scale):
    return tuple(v * s for v, s in zip(vector, scale))


def cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def unit(a):
    length = math.sqrt(sum(v * v for v in a))
    return tuple(v / length for v in a)


def find(name, scene=SCENE):
    objects = json.loads(scene.read_text(encoding="utf-8"))["objects"]
    matches = [o for o in objects if o["name"] == name]
    assert len(matches) == 1, f"expected one {name!r} in {scene}, found {len(matches)}"
    return matches[0]


def emissive_objects():
    """Every object in every scene that actually emits light.

    Emission is one-sided now, so an emitter authored face-up is not merely
    mislabelled -- it emits into whatever is above it and nothing else.
    """
    for scene in sorted((ROOT / "assets" / "scenes").glob("*.json")):
        for obj in json.loads(scene.read_text(encoding="utf-8"))["objects"]:
            if obj.get("emissionStrength", 0) > 0 and any(obj.get("emissionColor", [])):
                yield scene, obj


class CornellEmitter(unittest.TestCase):
    def setUp(self):
        self.emitter = find("Ceiling emitter")
        self.rotation = rotation_matrix(self.emitter["rotation"])

    def test_emitter_actually_emits(self):
        self.assertGreater(self.emitter["emissionStrength"], 0.0)
        self.assertEqual(self.emitter["asset"], "floor")

    def test_shading_normal_points_down_into_the_room(self):
        normal = apply(self.rotation, QUAD_VERTEX_NORMAL)
        self.assertAlmostEqual(normal[1], -1.0, places=6)

    def test_winding_agrees_with_the_shading_normal(self):
        """A flipped normal with unflipped winding would leave the geometric
        normal -- what the path tracer derives from the triangle -- still
        pointing at the ceiling."""
        scale = self.emitter["scale"]
        shading = apply(self.rotation, QUAD_VERTEX_NORMAL)
        for triangle in QUAD_TRIANGLES:
            p = [
                apply(self.rotation, scaled(QUAD_CORNERS[i], scale)) for i in triangle
            ]
            edge0 = tuple(p[1][i] - p[0][i] for i in range(3))
            edge1 = tuple(p[2][i] - p[0][i] for i in range(3))
            geometric = unit(cross(edge0, edge1))
            self.assertGreater(sum(g * s for g, s in zip(geometric, shading)), 0.99)

    def test_emitter_faces_the_room_and_not_the_ceiling(self):
        normal = apply(self.rotation, QUAD_VERTEX_NORMAL)
        ceiling = find("Sealed ceiling")
        floor = find("Neutral floor")
        self.assertLess(self.emitter["position"][1], ceiling["position"][1])
        toward_room = floor["position"][1] - self.emitter["position"][1]
        self.assertGreater(normal[1] * toward_room, 0.0)


class EveryAuthoredEmitter(unittest.TestCase):
    """The one-sided rule applies to every emissive material, not just Cornell.

    A flat "floor" quad authored with no rotation emits straight up.  Both
    emitters in the repository are named "Ceiling emitter" and sit above the
    space they are meant to light, so up is always the wrong way.
    """

    def test_no_flat_emitter_is_left_facing_up(self):
        seen = 0
        for scene, obj in emissive_objects():
            if obj["asset"] != "floor":
                continue  # A closed mesh is already correct from outside.
            seen += 1
            normal = apply(rotation_matrix(obj["rotation"]), QUAD_VERTEX_NORMAL)
            self.assertLess(
                normal[1],
                0.0,
                f"{scene.name}: {obj['name']!r} emits upward (rotation "
                f"{obj['rotation']}); a flat emitter only lights the side its "
                f"authored normal points to",
            )
        self.assertGreater(seen, 0, "no flat emissive objects found to check")


if __name__ == "__main__":
    unittest.main()
