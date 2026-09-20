"""The startup preset and the settings it names have to be one fact.

The preset number lived in vk_engine.h as `qualityPreset{0}` while the ten
settings it stands for lived in SSGIPresets, written out a second time as
member initialisers.  They agreed only because someone kept them agreeing by
hand, so the engine could report a preset it was not running.  R2 makes the
table the only source and applies preset 2 (Balanced) at startup through the
same function runtime selection uses.

These checks read the sources rather than the engine: nothing here needs a GPU.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "vk_engine.h"
ENGINE = ROOT / "src" / "vk_engine.cpp"
SSGI = ROOT / "src" / "vk_engine_ssgi.cpp"

# The table as the design document records it, in the order
# apply_ssgi_quality_preset() assigns: halfResolution, raysPerPixel, stepCount,
# rayLength, thickness, startOffset, filterRadius, filterDepthFalloff,
# filterNormalPower, historyWeight.
EXPECTED_PRESETS = [
    ("false", "4", "32", "12.0f", "0.35f", "0.08f", "3", "800.0f", "32.0f", "0.92f"),
    ("true", "4", "48", "16.0f", "0.30f", "0.06f", "4", "700.0f", "28.0f", "0.94f"),
    ("true", "2", "32", "12.0f", "0.35f", "0.08f", "3", "800.0f", "32.0f", "0.92f"),
    ("true", "1", "16", "8.0f", "0.45f", "0.10f", "2", "900.0f", "36.0f", "0.90f"),
    ("false", "8", "96", "20.0f", "0.25f", "0.05f", "5", "800.0f", "32.0f", "0.96f"),
]

STARTUP_PRESET = 2

# Written by apply_ssgi_quality_preset() from the table, so a member
# initialiser holding a real value would be a second source of truth.
PRESET_OWNED_FIELDS = [
    "stepCount",
    "raysPerPixel",
    "rayLength",
    "thickness",
    "startOffset",
    "historyWeight",
    "filterRadius",
    "filterDepthFalloff",
    "filterNormalPower",
    "halfResolution",
    "qualityPreset",
]


def ssgi_state_body():
    """The SSGIState struct only: SSAO declares a filterRadius of its own."""
    text = HEADER.read_text(encoding="utf-8")
    start = text.index("struct SSGIState {")
    depth, index = 0, start
    while True:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
        index += 1


def preset_table():
    """SSGIPresets as a list of rows of literal tokens."""
    body = re.search(
        r"constexpr std::array<SSGIPreset,\s*\d+>\s*SSGIPresets\{\{(.*?)\}\};",
        SSGI.read_text(encoding="utf-8"),
        re.S,
    )
    assert body, "SSGIPresets table not found in " + str(SSGI)
    return [
        tuple(field.strip() for field in row.split(","))
        for row in re.findall(r"\{([^{}]*)\}", body.group(1))
    ]


class SsgiPresetDefault(unittest.TestCase):
    def test_startup_applies_balanced_through_the_preset_function(self):
        """R2: the implicit default is preset 2, set by the shared function."""
        text = ENGINE.read_text(encoding="utf-8")
        self.assertIn(
            f"apply_ssgi_quality_preset({STARTUP_PRESET});",
            text,
            "init() must apply the Balanced preset through the shared function, "
            "not by assigning qualityPreset",
        )

    def test_explicit_override_is_applied_after_the_startup_default(self):
        """An explicit MIRABILIS_SSGI_PRESET has to win, so it must come second."""
        text = ENGINE.read_text(encoding="utf-8")
        default_at = text.index(f"apply_ssgi_quality_preset({STARTUP_PRESET});")
        override_at = text.index('SDL_getenv("MIRABILIS_SSGI_PRESET")')
        self.assertLess(
            default_at,
            override_at,
            "the startup default must be applied before the environment "
            "override, or the override would be overwritten by it",
        )

    def test_presets_zero_to_four_keep_their_table_entries(self):
        """R2 changes which preset starts, never what any preset means."""
        table = preset_table()
        self.assertEqual(len(table), len(EXPECTED_PRESETS))
        for index, (actual, expected) in enumerate(zip(table, EXPECTED_PRESETS)):
            self.assertEqual(actual, expected, f"preset {index} changed")

    def test_the_startup_preset_is_half_resolution_two_rays(self):
        """Guards the one row R2 promotes, so a silent edit to it is caught."""
        row = preset_table()[STARTUP_PRESET]
        self.assertEqual(row[0], "true")
        self.assertEqual(row[1], "2")

    def test_preset_settings_have_no_second_source_in_the_header(self):
        text = ssgi_state_body()
        for field in PRESET_OWNED_FIELDS:
            declaration = re.search(rf"^\s*\w+\s+{field}\{{(.*?)\}};", text, re.M)
            self.assertIsNotNone(declaration, f"{field} not declared in SSGIState")
            self.assertEqual(
                declaration.group(1).strip(),
                "",
                f"{field} carries a literal default; the preset table owns it",
            )

    def test_apply_writes_every_preset_owned_field(self):
        """A field in the table that apply() forgets would never take effect."""
        body = re.search(
            r"void VulkanEngine::apply_ssgi_quality_preset\(int preset\)\s*\{(.*?)\n\}",
            SSGI.read_text(encoding="utf-8"),
            re.S,
        )
        assert body, "apply_ssgi_quality_preset not found in " + str(SSGI)
        for field in PRESET_OWNED_FIELDS:
            self.assertIn(
                f"_ssgi.{field}",
                body.group(1),
                f"apply_ssgi_quality_preset does not set {field}",
            )


if __name__ == "__main__":
    unittest.main()
