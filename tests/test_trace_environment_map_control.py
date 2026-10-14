"""MIRABILIS_SSGI_TRACE_ENVIRONMENT_MAP has to be honoured, strict, and recorded.

R3.1 froze the parity rule: a candidate compared against an R3 reference runs
with traceEnvironmentMap false, so SSGI lights ray misses from the analytic
gradient as the path tracer does.  Until an unattended run could select that,
no candidate-versus-reference comparison could discharge a gate.

The value parser is checked by static_assert in src/env_flags.h (absent keeps
the default, each on and off spelling, invalid), so a Debug or Release build is
that test run.  These checks read the wiring:

  - the override keeps the traceEnvironmentMap default when absent;
  - it is applied after the presets, so nothing later overwrites it;
  - an invalid value aborts rather than falling back to the default;
  - every capture sidecar and the benchmark line record the value used.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
ENGINE = ROOT / "src" / "vk_engine.cpp"
FLAGS = ROOT / "src" / "env_flags.h"
TRACE = ROOT / "src" / "vk_engine_path_trace.cpp"
HEADER = ROOT / "src" / "vk_engine.h"

VARIABLE = "MIRABILIS_SSGI_TRACE_ENVIRONMENT_MAP"


def override_block():
    """The init() block that reads the variable, through its assignment."""
    text = ENGINE.read_text(encoding="utf-8")
    start = text.index("const EnvBoolOverride traceEnvironmentMap")
    end = text.index("_ssgi.traceEnvironmentMap = ", start)
    return text[start : text.index(";", end) + 1]


def function_body(text, signature):
    start = text.index(signature)
    depth, index = 0, text.index("{", start)
    while True:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
        index += 1


class TraceEnvironmentMapControl(unittest.TestCase):
    def test_parser_truth_table_is_compiled_not_assumed(self):
        """Absent, enabled, disabled and invalid must stay proven."""
        text = FLAGS.read_text(encoding="utf-8")
        for case in (
            "parse_env_bool_override(nullptr, true) == EnvBoolOverride{true, true}",
            "parse_env_bool_override(nullptr, false) == EnvBoolOverride{true, false}",
            'parse_env_bool_override("1", false) == EnvBoolOverride{true, true}',
            'parse_env_bool_override("0", true) == EnvBoolOverride{true, false}',
            '!parse_env_bool_override("2", true).valid',
        ):
            self.assertIn(f"static_assert({case});", text, case)
        # Present values go through the R1 parser, not a second spelling list.
        body = function_body(text, "constexpr EnvBoolOverride parse_env_bool_override")
        self.assertIn("parse_env_bool(value)", body)

    def test_default_is_unchanged(self):
        self.assertIn("bool traceEnvironmentMap{true};", HEADER.read_text(encoding="utf-8"))

    def test_absent_keeps_the_current_default(self):
        block = override_block()
        self.assertRegex(
            block,
            r"parse_env_bool_override\(\s*SDL_getenv\(\"" + VARIABLE
            + r"\"\),\s*_ssgi\.traceEnvironmentMap\)",
        )

    def test_read_once_after_the_presets(self):
        text = ENGINE.read_text(encoding="utf-8")
        self.assertEqual(text.count(VARIABLE), 2, "read once, named once in its error")
        at = text.index(VARIABLE)
        for earlier in (
            "apply_ssgi_quality_preset(2);",
            "apply_max_fidelity_settings();\n",
            "apply_performance_settings();\n",
        ):
            earlier_at = text.index(earlier, text.index("void VulkanEngine::init()"))
            self.assertLess(earlier_at, at, f"{earlier} would overwrite the override")

    def test_invalid_value_aborts_before_it_is_applied(self):
        block = override_block()
        invalid_at = block.index("!traceEnvironmentMap.valid")
        abort_at = block.index("std::abort()", invalid_at)
        self.assertLess(abort_at, block.index("_ssgi.traceEnvironmentMap = "))
        self.assertIn("GI TEST FAIL", block[invalid_at:abort_at])

    def test_capture_sidecars_record_the_value_used(self):
        text = TRACE.read_text(encoding="utf-8")
        for capture in ("void VulkanEngine::capture_ssgi(", "void VulkanEngine::capture_raster("):
            body = function_body(text, capture)
            self.assertIn(
                '"\\nTrace environment map: " << _ssgi.traceEnvironmentMap', body, capture)

    def test_benchmark_line_records_the_value_used(self):
        text = ENGINE.read_text(encoding="utf-8")
        match = re.search(
            r'"SSGI benchmark: preset=\{\} extent=\{\}x\{\} trace-environment-map=\{\}[^"]*",'
            r"\s*_ssgi\.qualityPreset, extent\.width, extent\.height,\s*_ssgi\.traceEnvironmentMap,",
            text,
        )
        self.assertIsNotNone(match, "benchmark output must state the miss-fill source")


if __name__ == "__main__":
    unittest.main()
