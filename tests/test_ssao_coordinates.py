"""CPU reference checks for SSAO rays and odd-resolution mapping.

Run: python -m unittest discover -s tests
These check the geometry contract; GPU output still needs visual validation.
"""
import math
import unittest


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def unit(a):
    size = math.sqrt(dot(a, a))
    return tuple(x / size for x in a)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def ray(uv, extent):
    tangent = math.tan(math.radians(35))
    return ((2*uv[0]-1)*tangent*extent[0]/extent[1], -(2*uv[1]-1)*tangent, -1)


class SSAOCoordinates(unittest.TestCase):
    def test_pixel_centers_and_composition_agree(self):
        for extent in ((1280, 720), (678, 381), (1279, 719), (1, 1), (3, 5)):
            ao = tuple((v+1)//2 for v in extent)
            for axis in range(2):
                for i in range(ao[axis]):
                    uv = (i+0.5)/ao[axis]
                    pixel = min(int(uv*extent[axis]), extent[axis]-1)
                    self.assertTrue(0 <= pixel < extent[axis])
                    # Composition maps that normalized position back to AO.
                    self.assertAlmostEqual(uv*ao[axis]-0.5, i)

    def test_grazing_wall_reconstruction_is_coplanar(self):
        extent = (678, 381)
        normal = unit((0.97, 0.1, 0.22))
        for x in range(7, 310, 13):
            uv = ((x+0.21)/extent[0], 0.47)
            pixel = tuple(int(uv[i]*extent[i]) for i in range(2))
            center = tuple((pixel[i]+0.5)/extent[i] for i in range(2))
            direction = ray(center, extent)
            distance = -5/dot(normal, direction)
            point = tuple(v*distance for v in direction)
            self.assertAlmostEqual(dot(normal, point), -5, places=10)
            # Old reconstruction used a different ray with this depth.
            wrong = tuple(v*distance for v in ray(uv, extent))
            self.assertGreater(abs(dot(normal, wrong)+5), 0.001)

    def test_wall_basis_keeps_all_rotation_angles(self):
        for normal in ((1,0,0), (0,1,0), (0,0,1), unit((0.97,0.1,0.22))):
            axis = (0,0,1) if abs(normal[2]) < 0.9 else (0,1,0)
            tangent = unit(cross(axis, normal))
            bitangent = cross(normal, tangent)
            for angle in range(0, 360, 15):
                a = math.radians(angle)
                rotated = tuple(tangent[i]*math.cos(a)+bitangent[i]*math.sin(a) for i in range(3))
                self.assertAlmostEqual(dot(rotated, normal), 0)
                self.assertAlmostEqual(dot(rotated, rotated), 1)

    def test_plane_rejection_preserves_raised_occluder(self):
        normal = (0,1,0)
        center = (0,0,-5)
        self.assertFalse(dot(normal, sub((1,0,-4), center)) > 0.001)
        self.assertTrue(dot(normal, sub((0,0.3,-5), center)) > 0.001)


if __name__ == '__main__':
    unittest.main()
