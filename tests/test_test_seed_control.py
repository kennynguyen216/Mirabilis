"""MIRABILIS_TEST_SEED has to be honoured, bounded, and loud when wrong.

A reference capture whose seed cannot be stated is a capture nobody can
repeat, and R3 needs at least three explicitly recorded seeds per scene.
baseSeed existed only as a Render Settings field, which an unattended run has
no way to reach, so every automated capture so far was rendered at 1337
whether or not its record said so.

The value parser itself is checked by static_assert in src/env_flags.h, so a
Debug or Release build is that test run.  What can still go wrong is the
wiring, and that is what these checks read:

  - the seed is applied inside the bounded-run branch, so an interactive
    session is untouched;
  - it is applied before the path tracer starts, not after;
  - an invalid value aborts rather than falling back to the default;
  - the chosen seed reaches the log and the capture metadata.

Run: python -m unittest discover -s tests
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
ENGINE = ROOT / "src" / "vk_engine.cpp"
FLAGS = ROOT / "src" / "env_flags.h"
TRACE = ROOT / "src" / "vk_engine_path_trace.cpp"


def bounded_run_branch():
    """The body of `if (frameLimit) {` in VulkanEngine::run()."""
    text = ENGINE.read_text(encoding="utf-8")
    start = text.index("if (frameLimit) {")
    depth, index = 0, text.index("{", start)
    while True:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
        index += 1


class TestSeedControl(unittest.TestCase):
    def test_parser_exists_with_a_strict_contract(self):
        text = FLAGS.read_text(encoding="utf-8")
        self.assertIn("parse_env_int", text)
        for status in ("Absent", "Valid", "Invalid"):
            self.assertIn(status, text)

    def test_parser_truth_table_is_compiled_not_assumed(self):
        """The static_asserts are the parser's test; they must not be deleted."""
        text = FLAGS.read_text(encoding="utf-8")
        asserts = re.findall(r"static_assert\(parse_env_int\(", text)
        self.assertGreaterEqual(
            len(asserts), 20, "parse_env_int lost its compile-time truth table")
        for rejected in ('"7x"', '"0x10"', '"7.0"', '"1 2"', '"2147483648"', '"-"'):
            self.assertIn(
                f"parse_env_int({rejected}).status == EnvIntStatus::Invalid",
                text,
                f"{rejected} is no longer proven invalid",
            )

    def test_seed_is_read_only_inside_a_bounded_run(self):
        """Interactive sessions must keep the baseSeed default untouched."""
        branch = bounded_run_branch()
        self.assertIn("MIRABILIS_TEST_SEED", branch)
        whole = ENGINE.read_text(encoding="utf-8")
        self.assertEqual(
            whole.count("MIRABILIS_TEST_SEED"),
            branch.count("MIRABILIS_TEST_SEED"),
            "MIRABILIS_TEST_SEED is read outside the bounded-run branch",
        )

    def test_invalid_seed_aborts_instead_of_falling_back(self):
        branch = bounded_run_branch()
        invalid_at = branch.index("EnvIntStatus::Invalid")
        abort_at = branch.index("std::abort()", invalid_at)
        applied_at = branch.index("_traceSettings.baseSeed = ")
        self.assertLess(
            abort_at,
            applied_at,
            "an invalid seed must stop the run before any seed is applied",
        )
        self.assertIn("GI TEST FAIL", branch[invalid_at:abort_at])

    def test_seed_is_applied_before_the_path_tracer_runs(self):
        branch = bounded_run_branch()
        self.assertLess(
            branch.index("_traceSettings.baseSeed = "),
            branch.index('SDL_getenv("MIRABILIS_TEST_TRACE")')
            if 'SDL_getenv("MIRABILIS_TEST_TRACE")' in branch
            else branch.index("MIRABILIS_TEST_TRACE"),
            "the seed must be set before the path tracer is started",
        )

    def test_chosen_seed_reaches_the_log(self):
        self.assertIn("GI test seed:", bounded_run_branch())

    def test_chosen_seed_reaches_the_capture_metadata(self):
        """Without this the artifact cannot state what produced it."""
        text = TRACE.read_text(encoding="utf-8")
        self.assertIn('"\\nSeed: "<<_traceSettings.baseSeed', text)

    def test_interactive_default_is_unchanged(self):
        header = (ROOT / "src" / "vk_engine.h").read_text(encoding="utf-8")
        self.assertIn("int baseSeed{1337};", header)


if __name__ == "__main__":
    unittest.main()
