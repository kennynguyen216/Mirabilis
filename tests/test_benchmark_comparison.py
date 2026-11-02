import unittest
from benchmark_comparison import comparison, percentile


class BenchmarkMathTests(unittest.TestCase):
    def test_fps_gain_uses_inverse_frame_time(self):
        result = comparison([5] * 5, [4] * 5)
        self.assertEqual(result["baselineFps"], 200)
        self.assertEqual(result["candidateFps"], 250)
        self.assertEqual(result["fpsChangePercent"], 25)
        self.assertEqual(result["pairedBootstrap95Percent"], [25, 25])
        self.assertEqual(result["verdict"], "improvement in this sample")

    def test_regression_is_not_reported_as_an_improvement(self):
        result = comparison([4] * 5, [5] * 5)
        self.assertAlmostEqual(result["fpsChangePercent"], -20)
        self.assertEqual(result["verdict"], "regression in this sample")

    def test_noise_and_small_changes_are_not_claimed_as_wins(self):
        result = comparison([3, 4, 5, 6, 7], [7, 6, 5, 4, 3])
        self.assertEqual(result["verdict"], "no clear change")
        low, high = result["pairedBootstrap95Percent"]
        self.assertLess(low, 0)
        self.assertGreater(high, 0)
        self.assertEqual(comparison([5] * 5, [4.99] * 5)["verdict"], "no clear change")

    def test_percentiles_and_invalid_samples(self):
        self.assertAlmostEqual(percentile([4, 2, 1, 3], .99), 3.97)
        with self.assertRaises(AssertionError): comparison([4, 5], [4, 5])
        with self.assertRaises(AssertionError): comparison([4, 5, 6], [4, 5, 0])
