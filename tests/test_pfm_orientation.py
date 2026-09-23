"""Which way up a .pfm capture is, checked with an asymmetric marker.

Every R3 metric is computed over a predeclared rectangle, so a reader that
disagrees with the writer about row order silently measures the wrong part of
the image: a "floor" crop lands on the ceiling and the comparison still returns
a plausible number.  Section 7.3 requires the orientation be verified with a
known marker rather than assumed.

The engine writes rows from y = height-1 down to y = 0 with scale -1.0
(vk_engine_path_trace.cpp, capture_path_trace).  Its y = 0 is the top row of
the image, so the first row in the file is the bottom row, which is what the
PFM format specifies.  scripts/pfm_to_png.py read_pfm() therefore has to flip
the data back to top-down before anything indexes it.

What this file can and cannot establish
---------------------------------------
These checks are synthetic.  They build a file the same way the engine's writer
does and assert the reader decodes it back the same way, which pins the reader
against the writer's *stated* convention and catches a change to either one.
They cannot prove what the engine's GPU actually put in that buffer, because
nothing here renders anything.

The authoritative orientation check is therefore not here.  It runs per capture
in scripts/validate_r3_capture.py, which tone maps the real .pfm with the
engine's own operator and aligns it against the .bmp the engine wrote from the
same pixels, and which reports BLOCKED rather than PASS when an image is too
vertically symmetric for the comparison to discriminate.  The last test below
exists to stop that check being quietly removed.

Run: python -m unittest discover -s tests
"""
import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PFM_TOOL = ROOT / "scripts" / "pfm_to_png.py"
CAPTURE = ROOT / "src" / "vk_engine_path_trace.cpp"

WIDTH, HEIGHT = 7, 5
MARKER = (1.0, 0.0, 0.0)
BLANK = (0.0, 0.0, 0.0)


def load_read_pfm():
    """Import read_pfm from the script without running its main()."""
    spec = importlib.util.spec_from_file_location("pfm_to_png", PFM_TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_pfm


def write_pfm_like_the_engine(path, marker_at_engine_y, marker_at_x):
    """Write a PFM exactly as capture_path_trace does.

    The engine's loop is `for (int y = height-1; y >= 0; --y)`, so rows leave
    in descending engine-y order.  `marker_at_engine_y` is in the engine's own
    coordinates, where 0 is the top row of the image.
    """
    with open(path, "wb") as file:
        file.write(b"PF\n")
        file.write(f"{WIDTH} {HEIGHT}\n".encode())
        file.write(b"-1.0\n")
        for y in range(HEIGHT - 1, -1, -1):
            for x in range(WIDTH):
                on_marker = y == marker_at_engine_y and x == marker_at_x
                file.write(struct.pack("<3f", *(MARKER if on_marker else BLANK)))


class PfmOrientation(unittest.TestCase):
    def setUp(self):
        self.read_pfm = load_read_pfm()

    def test_engine_top_left_decodes_to_array_top_left(self):
        """Engine y=0,x=0 is the top-left pixel; so is decoded row 0, column 0."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "marker.pfm"
            write_pfm_like_the_engine(path, marker_at_engine_y=0, marker_at_x=0)
            image = self.read_pfm(str(path))
            self.assertEqual(image.shape, (HEIGHT, WIDTH, 3))
            self.assertAlmostEqual(float(image[0, 0, 0]), 1.0)
            # And nowhere else, so a flip that happens to land on another red
            # pixel cannot pass.
            self.assertAlmostEqual(float(image[HEIGHT - 1, 0, 0]), 0.0)
            self.assertAlmostEqual(float(image[0, WIDTH - 1, 0]), 0.0)
            self.assertAlmostEqual(float(image[HEIGHT - 1, WIDTH - 1, 0]), 0.0)

    def test_engine_bottom_right_decodes_to_array_bottom_right(self):
        """The opposite corner, so the test cannot pass under a 180 rotation."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "marker.pfm"
            write_pfm_like_the_engine(
                path, marker_at_engine_y=HEIGHT - 1, marker_at_x=WIDTH - 1)
            image = self.read_pfm(str(path))
            self.assertAlmostEqual(float(image[HEIGHT - 1, WIDTH - 1, 0]), 1.0)
            self.assertAlmostEqual(float(image[0, 0, 0]), 0.0)

    def test_asymmetric_marker_pins_both_axes(self):
        """One off-centre pixel, so a row flip and a column flip both fail."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "marker.pfm"
            write_pfm_like_the_engine(path, marker_at_engine_y=1, marker_at_x=2)
            image = self.read_pfm(str(path))
            rows, columns = (image[:, :, 0] > 0.5).nonzero()
            self.assertEqual(list(rows), [1], "row order disagrees with the writer")
            self.assertEqual(list(columns), [2], "column order disagrees with the writer")

    def test_writer_still_emits_descending_rows_and_little_endian(self):
        """If capture_path_trace changes its loop, this test's premise is void."""
        text = CAPTURE.read_text(encoding="utf-8")
        self.assertIn('<<"PF\\n"', text)
        self.assertIn('"\\n-1.0\\n"', text, "scale sign encodes endianness")
        self.assertIn(
            "for(int y=int(_drawExtent.height)-1;y>=0;--y)",
            text,
            "capture_path_trace no longer writes rows bottom-first; "
            "re-derive the orientation before trusting any crop",
        )


class RealCaptureOrientationCheckExists(unittest.TestCase):
    """The synthetic checks above are not the evidence; this one guards what is."""

    def setUp(self):
        self.validator = (ROOT / "scripts" / "validate_r3_capture.py").read_text(
            encoding="utf-8")

    def test_validator_aligns_the_pfm_against_the_engine_bmp(self):
        for required in ("def read_bmp", "def engine_tonemap", "np.flipud(bmp)"):
            self.assertIn(
                required,
                self.validator,
                "validate_r3_capture.py no longer aligns captures against the "
                "engine's own BMP; orientation would rest on a synthetic claim",
            )

    def test_validator_refuses_to_pass_a_symmetric_image(self):
        """A vertically symmetric image cannot discriminate, so it must block."""
        self.assertIn("too vertically symmetric to discriminate", self.validator)

    def test_validator_treats_blocked_as_not_passing(self):
        self.assertIn("Blocked checks are not passes", self.validator)


if __name__ == "__main__":
    unittest.main()
