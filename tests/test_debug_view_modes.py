"""The shader's debug-view constants have to agree with the C++ enum.

C++ writes the raw RenderDebugView value into sceneData.materialDebug.x and the
forward shader compares it against its own named constants, so the two lists
are one interface with no compiler checking it.  They drifted by two: the
material roughness and metallic views showed nothing at all and the seven after
them each showed the wrong buffer.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "vk_engine.h"
HELPERS = ROOT / "src" / "vk_engine_render_helpers.h"
SHADER = ROOT / "shaders" / "material_brdf.glsl"


def cpp_enum():
    """RenderDebugView as {name: value}, following C++ implicit numbering."""
    body = re.search(
        r"enum class RenderDebugView\s*:\s*int\s*\{(.*?)\n\};",
        HEADER.read_text(encoding="utf-8"),
        re.S,
    )
    assert body, "RenderDebugView enum not found in " + str(HEADER)
    values, next_value = {}, 0
    for name, assigned in re.findall(
        r"^\s*(\w+)\s*(?:=\s*(-?\d+))?\s*,", body.group(1), re.M
    ):
        next_value = int(assigned) if assigned else next_value
        values[name] = next_value
        next_value += 1
    return values


def shader_constants():
    return {
        name: int(value)
        for name, value in re.findall(
            r"^const int MaterialDebug(\w+)\s*=\s*(\d+);",
            SHADER.read_text(encoding="utf-8"),
            re.M,
        )
    }


class DebugViewModes(unittest.TestCase):
    def test_material_debug_constants_match_the_enum(self):
        enum = cpp_enum()
        shader = shader_constants()
        self.assertTrue(shader, "no MaterialDebug* constants found in " + str(SHADER))
        for name, value in shader.items():
            # The shader drops the "Material" prefix on two of them.
            key = name if name in enum else "Material" + name
            self.assertIn(key, enum, f"MaterialDebug{name} names no RenderDebugView")
            self.assertEqual(
                enum[key], value, f"MaterialDebug{name} is {value}, enum is {enum[key]}"
            )

    def test_shader_covers_every_forward_written_view(self):
        """is_forward_material_debug_view() spans MaterialRoughness..TangentHandedness,
        and the shader returns false for anything it does not name, so a view
        inside that range with no constant renders black."""
        enum = cpp_enum()
        shader = set(shader_constants().values())
        first = enum["MaterialRoughness"]
        last = enum["TangentHandedness"]
        for value in range(first, last + 1):
            self.assertIn(value, shader, f"RenderDebugView {value} has no shader branch")

    def test_every_enumerator_is_named_in_the_ui_list(self):
        enum = cpp_enum()
        size = re.search(
            r"std::array<const char\*,\s*(\d+)>\s*RenderDebugViewNames",
            HELPERS.read_text(encoding="utf-8"),
        )
        assert size, "RenderDebugViewNames not found in " + str(HELPERS)
        self.assertEqual(len(enum), int(size.group(1)))
        # render_debug_view_name() indexes that array by the enum value, so the
        # values have to be a gapless 0..n-1 run for the names to line up.
        self.assertEqual(sorted(enum.values()), list(range(len(enum))))


if __name__ == "__main__":
    unittest.main()
