"""Run the real PowerShell metadata writer and read what it actually emits.

This exists because the previous tests did not catch a shipped defect. They
asserted things about hand-written fixtures and about the *text* of the capture
script, so they were green while the first real run produced:

  - a manifest whose every dynamic line was split in two, because PowerShell
    binds "," tighter than "+", so
        @('Commit: ' + $revision, 'Branch: ' + $branch)
    parses as 'Commit: ' + ($revision, 'Branch: ') + $branch;
  - a source.txt collapsed onto a single line, same cause, string + array;
  - a binary-sha256.txt written as UTF-16 through Format-List | Out-File, with
    the hash wrapped across two lines at the host's console width;
  - a blank Invocation, because $MyInvocation.Line is empty under -File.

So these tests invoke scripts/r3_metadata.ps1 for real, into a temporary
directory, and parse the bytes it writes with the same parse_fields() the
validator uses. No engine, no GPU, no capture: the writer only hashes files and
formats strings.

Skipped where PowerShell is unavailable.

Run: python -m unittest discover -s tests
"""
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
METADATA = ROOT / "scripts" / "r3_metadata.ps1"
CAPTURE = ROOT / "scripts" / "capture_r3_references.ps1"
VALIDATOR_DIR = ROOT / "scripts"


def _load_validator():
    spec = importlib.util.spec_from_file_location(
        "validate_r3_capture", VALIDATOR_DIR / "validate_r3_capture.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The validator itself, not a copy of it: a re-implementation here would drift
# (it already had, using setdefault where the validator overwrites) and would
# reinstate exactly the fixture-agrees-with-fixture risk this file exists to
# remove.
validate_r3_capture = _load_validator()
parse_fields = validate_r3_capture.parse_fields

POWERSHELL = shutil.which("powershell") or shutil.which("pwsh")

REVISION = "742c0a688d0cfa44622fcbaa47685fd60d3c4eda"
BRANCH = "recovery/r3-matched-references"
SEEDS = (1337, 2026, 90210)
FRAMES = 512
TIMEOUT = 900


def run_writer(output, dirty=(), out_file_width=None, diagnostic=False):
    """Invoke Write-R3Metadata for real and return its exit status and output."""
    dirty_literal = ("@(" + ",".join(f"'{entry}'" for entry in dirty) + ")") if dirty \
        else "@()"
    # The engine binary is deliberately not required: any file will do as the
    # thing being hashed, which keeps this test off the GPU entirely.
    engine_stand_in = METADATA
    # Forcing Out-File's width is what reproduces the original defect: the old
    # writer piped Get-FileHash through Format-List | Out-File, so the hash was
    # wrapped to whatever width the host happened to have. Setting BufferSize
    # is not usable here -- PowerShell refuses a buffer narrower than the
    # window -- and is unnecessary, since this parameter drives the same wrap.
    width = (f"$PSDefaultParameterValues['Out-File:Width'] = {out_file_width}; "
             if out_file_width else "")
    script = f"""
$ErrorActionPreference = 'Stop'
{width}. '{METADATA.as_posix()}'
$scenes = @(
    @{{ name = 'cornell';     scene = 'gi_cornell_box.json';        camera = '0 2 3.5 0 0' }},
    @{{ name = 'living-room'; scene = 'living_room_showcase.json';  camera = '0 1.6 -3 0 3.14' }}
)
$invocation = Get-R3InvocationDescription -Seeds @({','.join(str(s) for s in SEEDS)}) `
    -Frames {FRAMES} -TimeoutSeconds {TIMEOUT} -Diagnostic ${str(diagnostic).lower()}
$null = Write-R3Metadata -Output '{pathlib.Path(output).as_posix()}' `
    -Repo '{ROOT.as_posix()}' -Revision '{REVISION}' -Branch '{BRANCH}' `
    -Dirty {dirty_literal} -Seeds @({','.join(str(s) for s in SEEDS)}) `
    -Frames {FRAMES} -TimeoutSeconds {TIMEOUT} -Invocation $invocation `
    -EnginePath '{engine_stand_in.as_posix()}' -Scenes $scenes -CapturedAt '2026-09-22 20:45:19 -05:00'
"""
    return subprocess.run(
        [POWERSHELL, "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, cwd=str(ROOT))


@unittest.skipIf(POWERSHELL is None, "PowerShell not available")
class MetadataSerialization(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._temporary = tempfile.TemporaryDirectory()
        cls.directory = pathlib.Path(cls._temporary.name)
        cls.result = run_writer(cls.directory)
        if cls.result.returncode != 0:
            raise AssertionError(
                f"metadata writer failed:\n{cls.result.stdout}\n{cls.result.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls._temporary.cleanup()

    def manifest_lines(self):
        return (self.directory / "manifest.txt").read_text(
            encoding="utf-8", errors="replace").splitlines()

    # --- defect 1: one physical line per record --------------------------
    def test_every_required_manifest_record_is_one_physical_line(self):
        required = {
            "Acceptable as an R3 reference": "yes",
            "Commit": REVISION,
            "Branch": BRANCH,
            "Dirty tree": "no",
            "Seeds": "1337, 2026, 90210",
            "Frames (accumulated samples per capture)": str(FRAMES),
            "Per-run timeout (s)": str(TIMEOUT),
            "Build configuration": "Release",
        }
        lines = self.manifest_lines()
        for key, value in required.items():
            matches = [line for line in lines if line.startswith(key + ":")]
            self.assertTrue(matches, f"no line starts with {key!r}")
            self.assertEqual(
                matches[0], f"{key}: {value}",
                f"{key!r} is not a single key/value line; the value was split off")

    def test_no_manifest_line_is_a_bare_dangling_value(self):
        """The old output put every value on its own line; catch that shape."""
        for line in self.manifest_lines():
            self.assertFalse(
                re.fullmatch(r"\s*[0-9a-fA-F]{40}\s*", line),
                f"bare commit hash on its own line: {line!r}")
            self.assertFalse(
                re.fullmatch(r"\s*\d+(,\s*\d+)+\s*", line),
                f"bare seed list on its own line: {line!r}")

    def test_no_manifest_key_line_ends_with_a_dangling_colon(self):
        for line in self.manifest_lines():
            if line.rstrip().endswith(":") and not line.startswith(" "):
                self.assertIn(
                    line.rstrip(), ("Worktree at capture time:",),
                    f"key with no value on its line: {line!r}")

    # --- parse_fields sees what validation will see ----------------------
    def test_parse_fields_recovers_every_key(self):
        fields = parse_fields(self.directory / "manifest.txt")
        self.assertEqual(fields.get("Commit"), REVISION)
        self.assertEqual(fields.get("Seeds"), "1337, 2026, 90210")
        self.assertEqual(
            fields.get("Frames (accumulated samples per capture)"), str(FRAMES))
        self.assertEqual(fields.get("Acceptable as an R3 reference"), "yes")
        self.assertEqual(fields.get("Dirty tree"), "no")
        self.assertEqual(fields.get("Build configuration"), "Release")

    # --- defect 4: invocation is recorded and actually binds --------------
    def test_invocation_is_recorded(self):
        fields = parse_fields(self.directory / "manifest.txt")
        invocation = fields.get("Invocation", "")
        self.assertTrue(invocation, "Invocation is blank")
        self.assertIn("capture_r3_references.ps1", invocation)
        self.assertNotIn(
            "-File", invocation,
            "-File passes -Seeds 1337,2026,90210 as one string, which cannot "
            "bind to [int[]]$Seeds; the recorded command would not run")

    def test_recorded_invocation_binds_against_the_real_param_block(self):
        """Bind the recorded command line against capture_r3_references.ps1's
        own param block -- the real one, read from the script -- and check the
        values that arrive. Substring assertions passed happily while the
        recorded -File command could not bind at all.

        The script body is never executed, so no engine and no GPU: only its
        param block is lifted out, via the PowerShell parser, into a stub."""
        fields = parse_fields(self.directory / "manifest.txt")
        invocation = fields.get("Invocation", "")
        prefix = r"& .\scripts\capture_r3_references.ps1"
        self.assertTrue(
            invocation.startswith(prefix),
            f"unrecognised invocation form: {invocation!r}")
        call = "Test-Bind" + invocation[len(prefix):]
        script = f"""
$ErrorActionPreference = 'Stop'
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    '{CAPTURE.as_posix()}', [ref]$null, [ref]$null)
$body = 'function Test-Bind {{ ' + $ast.ParamBlock.Extent.Text + @'

  ConvertTo-Json @{{
      Seeds = @($Seeds); Frames = $Frames
      TimeoutSeconds = $TimeoutSeconds; Diagnostic = [bool]$Diagnostic
  }} -Compress
}}
'@
Invoke-Expression $body
{call}
"""
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(
            result.returncode, 0,
            "the recorded invocation does not bind:\n"
            f"{result.stdout}\n{result.stderr}")
        bound = json.loads(result.stdout)
        self.assertEqual(bound["Seeds"], list(SEEDS))
        self.assertEqual(bound["Frames"], FRAMES)
        self.assertEqual(bound["TimeoutSeconds"], TIMEOUT)
        self.assertFalse(bound["Diagnostic"])

    # --- defect 3: source.txt has separate physical lines -----------------
    def test_source_txt_fields_are_on_separate_lines(self):
        lines = (self.directory / "source.txt").read_text(
            encoding="utf-8", errors="replace").splitlines()
        self.assertGreaterEqual(
            len(lines), 4, f"source.txt collapsed into {len(lines)} line(s): {lines}")
        self.assertEqual(lines[0], f"Commit: {REVISION}")
        self.assertEqual(lines[1], f"Branch: {BRANCH}")
        self.assertEqual(lines[2], "Dirty tree: no")
        self.assertEqual(lines[3], "Worktree:")

    # --- defect 2: one machine-readable hash record -----------------------
    def test_binary_hash_is_exactly_one_valid_record(self):
        raw = (self.directory / "binary-sha256.txt").read_bytes()
        self.assertNotIn(b"\x00", raw, "binary-sha256.txt is UTF-16, not ASCII")
        lines = [line for line in raw.decode("utf-8").splitlines() if line.strip()]
        self.assertEqual(len(lines), 1, f"expected one record, got {lines}")
        match = re.fullmatch(r"([0-9A-Fa-f]{64})\s\s+(\S.*)", lines[0])
        self.assertIsNotNone(match, f"malformed hash record: {lines[0]!r}")
        self.assertEqual(match.group(2), "bin/Release/engine.exe")

    def test_every_hash_file_is_ascii_single_line_records(self):
        for name in ("binary-sha256.txt", "shader-sha256.txt",
                     "scene-sha256.txt", "regions-sha256.txt"):
            raw = (self.directory / name).read_bytes()
            self.assertNotIn(b"\x00", raw, f"{name} is not ASCII/UTF-8")
            for line in raw.decode("utf-8").splitlines():
                if line.strip():
                    self.assertRegex(line, r"^[0-9A-Fa-f]{64}\s\s+\S",
                                     f"{name} record is wrapped or malformed")

    # --- defect 2 again: console width must not reach the bytes ----------
    def test_output_is_identical_under_a_narrow_console(self):
        """Format-List/Out-File wrapped at host width; nothing may do that now."""
        with tempfile.TemporaryDirectory() as narrow_dir:
            narrow = pathlib.Path(narrow_dir)
            result = run_writer(narrow, out_file_width=20)
            self.assertEqual(result.returncode, 0,
                             f"{result.stdout}\n{result.stderr}")
            for name in ("manifest.txt", "source.txt", "binary-sha256.txt",
                         "shader-sha256.txt", "scene-sha256.txt",
                         "regions-sha256.txt"):
                self.assertEqual(
                    (self.directory / name).read_bytes(),
                    (narrow / name).read_bytes(),
                    f"{name} changed when the console was 20 columns wide")

    # --- dirty runs ------------------------------------------------------
    def test_dirty_worktree_is_recorded_on_its_own_lines(self):
        with tempfile.TemporaryDirectory() as dirty_dir:
            directory = pathlib.Path(dirty_dir)
            result = run_writer(directory, dirty=(" M src/vk_engine.cpp",
                                                  "?? scripts/new_thing.ps1"))
            self.assertEqual(result.returncode, 0,
                             f"{result.stdout}\n{result.stderr}")
            fields = parse_fields(directory / "manifest.txt")
            self.assertEqual(fields.get("Dirty tree"), "YES")
            self.assertTrue(
                fields.get("Acceptable as an R3 reference", "").startswith("NO"))
            lines = (directory / "source.txt").read_text(
                encoding="utf-8").splitlines()
            self.assertIn(" M src/vk_engine.cpp", lines)
            self.assertIn("?? scripts/new_thing.ps1", lines)


@unittest.skipIf(POWERSHELL is None, "PowerShell not available")
class ValidatorAcceptsRealProducerOutput(unittest.TestCase):
    """The producer's real bytes must satisfy the validator's own hash checks."""

    def test_validator_hash_checks_pass_on_real_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            result = run_writer(path)
            self.assertEqual(result.returncode, 0,
                             f"{result.stdout}\n{result.stderr}")
            validator = validate_r3_capture
            report = validator.Report()
            validator.validate_hash_files(path, report)
            failures = [line for line in report.lines if line.startswith("FAIL")]
            self.assertEqual(failures, [], "\n".join(report.lines))


if __name__ == "__main__":
    unittest.main()
