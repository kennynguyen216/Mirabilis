import unittest
import numpy as np
from lighting_benchmark import image_difference,check_repeat_image


class BenchmarkImageChecks(unittest.TestCase):
    def test_stable_reference_remains_exact(self):
        image=np.full((16,16,3),.1,dtype=np.float32)
        self.assertEqual(check_repeat_image(image,image,image)['meanRelative'],0)

    def test_small_run_noise_is_calibrated_against_reference_repeat(self):
        image=np.full((16,16,3),.1,dtype=np.float32)
        noise=np.random.default_rng(216).uniform(-1e-5,1e-5,image.shape).astype(np.float32)
        result=check_repeat_image(image,image+noise,image-noise)
        self.assertLess(result['meanRelative'],.005)
        self.assertGreater(result['referenceRepeatDifference']['meanRelative'],0)
        self.assertLess(abs(result['signedMeanRelative']),result['meanRelative'])

    def test_darkening_geometry_outlier_and_high_noise_cannot_bypass_check(self):
        image=np.full((16,16,3),.1,dtype=np.float32);repeat=image+1e-5
        for changed in [image*.9,image+.001]:
            with self.assertRaises(AssertionError):check_repeat_image(image,changed,repeat)
        outlier=image.copy();outlier[0,0]=1
        with self.assertRaises(AssertionError):check_repeat_image(image,outlier,repeat)
        with self.assertRaises(AssertionError):check_repeat_image(image,image+.001,image+.1)

    def test_invalid_captures_fail(self):
        image=np.zeros((2,2,3),dtype=np.float32)
        with self.assertRaises(AssertionError):image_difference(image,image[:1])
        invalid=image.copy();invalid[0,0,0]=np.nan
        with self.assertRaises(AssertionError):image_difference(image,invalid)


if __name__=='__main__':unittest.main()
