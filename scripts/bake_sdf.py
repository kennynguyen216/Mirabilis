"""Bake a mesh to a signed distance field volume, for the Lumen-lite 3D
backup tracing tier.

usage: bake_sdf.py --queue <queue dir> <cache dir>
       bake_sdf.py <input.obj|.glb|...> <output.sdf> [resolution] [mesh name]
       bake_sdf.py --test-sphere <output.sdf> [resolution]
       bake_sdf.py --verify-axes <output.sdf> [resolution]

--queue is the normal path.  The engine writes every scene mesh that has no
cached volume to <queue dir>/<hash>.obj, in the mesh's own local space and
named by a hash of its geometry; this bakes each one to <cache dir>/<hash>.sdf
at a fixed world-space voxel size and skips any that already exist.

A mesh name picks one geometry out of a multi-mesh file (for example
"Suzanne" from assets/basicmesh.glb); without one, every geometry in the file
is merged into a single volume.

Uses mesh_to_sdf's virtual-depth-scan method (not the classic inside/outside
ray-parity test), because it does not require a watertight mesh -- Sponza
and the living room showcase both have thin, open geometry (foliage cards,
glass panes) that would misclassify under the ray-parity approach.

Output format (".sdf", all little-endian):
    magic       4 bytes   b"MSDF"
    version     uint32    1
    dimX,Y,Z    uint32 x3 grid resolution
    boundsMin   float32x3 world-space box covered by the grid's outer texel
    boundsMax   float32x3 edges, so a shader maps a world point p to texture
                          coordinates as (p - boundsMin) / (boundsMax - boundsMin)
                          with no half-texel correction of its own
    data        float32 x dimX*dimY*dimZ, signed distance in world units,
                C-contiguous in (z, y, x) order: texel (x, y, z) is at flat
                index x + y*dimX + z*dimX*dimY, which is the layout an
                untransposed VkBufferImageCopy uploads

    Sign convention: negative = inside the mesh, positive = outside.

How mesh_to_sdf's grid relates to world space (read from its source, and
checked by --verify-axes rather than trusted):
    - scale_to_unit_cube recentres the mesh on its bounding-box centre and
      scales it by 2 / (longest bounding-box extent), so the longest axis
      spans [-1, 1].
    - get_raster_points samples np.linspace(-1, 1, N) on each axis, so sample
      0 sits exactly on -1 and sample N-1 on +1: the samples are texel
      centres, and the outer texel edges are half a spacing further out.
    - Its returned array is indexed [x, y, z], and its distances are in the
      normalised space, not world units.
"""
import itertools
import struct
import sys

import numpy as np
import trimesh
from mesh_to_sdf import mesh_to_voxels


def bake(mesh: trimesh.Trimesh, resolution: int):
    if not mesh.is_watertight:
        print(
            f"warning: mesh is not watertight ({mesh.body_count} bodies); "
            "mesh_to_sdf's depth-scan method tolerates this, but expect "
            "more error in thin/concave regions than a watertight mesh.")
    centroid = mesh.bounding_box.centroid
    # World units per normalised unit, matching scale_to_unit_cube.
    world_per_unit = np.max(mesh.bounding_box.extents) / 2.0
    # Samples span [-1, 1] as texel centres; the edges are half a spacing out.
    half_spacing = 1.0 / (resolution - 1)
    edge = (1.0 + half_spacing) * world_per_unit
    bounds_min = (centroid - edge).astype(np.float32)
    bounds_max = (centroid + edge).astype(np.float32)

    grid = mesh_to_voxels(mesh, voxel_resolution=resolution)  # [x, y, z]
    grid = (grid * world_per_unit).astype(np.float32)
    spacing = 2.0 * edge / resolution
    grid = remove_floating_inside_islands(grid, spacing)
    grid_zyx = np.transpose(grid, (2, 1, 0))
    return grid_zyx, bounds_min, bounds_max


# An inside region that never comes within this many voxels of the surface
# cannot be real geometry.
FLOATING_ISLAND_VOXELS = 1.5


def remove_floating_inside_islands(grid, spacing):
    """Flip the sign of 'inside' regions that float in empty space.

    On a non-watertight mesh the normal-based sign test misclassifies some
    isolated points well away from the surface as inside.  A sphere tracer
    then stops on them as solid specks, and a GI fallback would pick up light
    from surfaces that do not exist.  Every genuine inside region touches the
    surface somewhere, so its smallest |distance| is near zero; a floating
    speck's is not.  Size is deliberately not the test: thin geometry such as
    foliage cards bakes to small inside regions that are real.  Only the sign
    is wrong on a speck -- its magnitude is still the distance to the nearest
    surface -- so flipping it gives the correct outside value.
    """
    from scipy import ndimage

    labels, count = ndimage.label(grid < 0.0)
    if count == 0:
        return grid
    closest = ndimage.minimum(np.abs(grid), labels, np.arange(1, count + 1))
    floating = np.flatnonzero(closest > FLOATING_ISLAND_VOXELS * spacing) + 1
    if floating.size:
        mask = np.isin(labels, floating)
        grid = grid.copy()
        grid[mask] = np.abs(grid[mask])
        print(f"removed {floating.size} of {count} inside regions floating "
              f"more than {FLOATING_ISLAND_VOXELS} voxels from any surface "
              f"({int(mask.sum())} voxels)")
    return grid


# Queue bakes size their grid in world units rather than a fixed count, so a
# 30 m wall and a 20 cm cup both get voxels small enough to hold their shape.
TARGET_VOXEL = 0.05
# The voxel budget per mesh, rather than a cap per axis: a long flat mesh (a
# roof lattice, a floor) keeps its full resolution because it is small along
# its thin axis, and only meshes large in every direction get coarser.  A
# 128-per-axis cap coarsened an 18 m roof lattice to 14.5 cm voxels, its
# shells closed the gaps between the bars, and the solid plate that resulted
# shadowed Sponza's whole courtyard.
MAX_VOXELS = 4_000_000
MAX_DIM = 1024
# Empty voxels around the mesh, so the surface never sits on the grid's edge
# where clamped sampling would flatten the distance.
PAD_VOXELS = 2
# A mesh thinner than this many voxels on some axis has no inside the grid
# can represent, so it is always baked as a shell (see below).
THIN_VOXELS = 2.0
# Surface samples for shell bakes: one per this fraction of a voxel along
# each side, bounded so a huge wall stays affordable and a tiny part is not
# undersampled.
SHELL_SAMPLE_SPACING_VOXELS = 0.25
SHELL_SAMPLES_MIN = 50_000
SHELL_SAMPLES_MAX = 4_000_000
# Closed meshes can be baked signed, but mesh_to_sdf's scan-based sign still
# fails inside concave crevices (between sofa cushions, around a picture
# frame's moulding) and the trace shows torn black patches there.
SIGN_CLOSED_MESHES = False


def bake_world_grid(mesh):
    """Bake one mesh at a fixed world-space voxel size.

    Only a closed mesh has an inside, so only a closed mesh is baked signed.
    Everything else -- which in these scenes is most things: walls, floors
    and panels are single zero-thickness sheets -- is baked as a two-sided
    shell, |distance| minus half a voxel.  mesh_to_sdf's sign test cannot be
    trusted on an open sheet: its scans see both sides and give the same
    surface points opposite normals, so the inside/outside vote flips from
    point to point and the traced result is full of holes and drips.  A shell
    needs no sign at all, and its surface points are sampled straight from
    the triangles, so faces no outside scan can see are still covered.
    """
    from mesh_to_sdf import mesh_to_sdf

    lo, hi = mesh.bounds
    extent = hi - lo
    padded = np.maximum(extent, TARGET_VOXEL) + 2 * PAD_VOXELS * TARGET_VOXEL
    voxel = max(TARGET_VOXEL,
                float(np.prod(padded) / MAX_VOXELS) ** (1.0 / 3.0),
                float(np.max(extent)) / (MAX_DIM - 2 * PAD_VOXELS))
    dims = np.maximum(
        np.ceil(extent / voxel).astype(int) + 2 * PAD_VOXELS, 2 * PAD_VOXELS + 1)
    dims = np.minimum(dims, MAX_DIM)
    centre = (lo + hi) * 0.5
    bounds_min = centre - dims * voxel * 0.5
    bounds_max = centre + dims * voxel * 0.5

    axes = [bounds_min[a] + (np.arange(dims[a]) + 0.5) * voxel for a in range(3)]
    px, py, pz = np.meshgrid(*axes, indexing="ij")
    points = np.stack([px.ravel(), py.ravel(), pz.ravel()], axis=1)

    # glTF splits vertices along UV and normal seams, so a closed mesh only
    # reads as closed once coincident vertices are merged.
    merged = mesh.copy()
    merged.merge_vertices()
    thin = float(np.min(extent)) < THIN_VOXELS * voxel
    signed = SIGN_CLOSED_MESHES and merged.is_watertight and not thin

    if signed:
        distances = mesh_to_sdf(mesh, points)
    else:
        spacing = SHELL_SAMPLE_SPACING_VOXELS * voxel
        samples = int(np.clip(mesh.area / (spacing * spacing),
                              SHELL_SAMPLES_MIN, SHELL_SAMPLES_MAX))
        distances = mesh_to_sdf(mesh, points, surface_point_method="sample",
                                sample_point_count=samples)
    grid = distances.astype(np.float32).reshape(tuple(dims))  # [x, y, z]

    if signed:
        grid = remove_floating_inside_islands(grid, voxel)
    else:
        grid = np.abs(grid) - 0.5 * voxel
    grid_zyx = np.transpose(grid, (2, 1, 0))
    return (grid_zyx, bounds_min.astype(np.float32),
            bounds_max.astype(np.float32), voxel, not signed)


def bake_queue(queue_dir, cache_dir):
    import glob
    import os
    import time

    os.makedirs(cache_dir, exist_ok=True)
    pending = sorted(glob.glob(os.path.join(queue_dir, "*.obj")))
    todo = [p for p in pending if not os.path.exists(
        os.path.join(cache_dir, os.path.splitext(os.path.basename(p))[0] + ".sdf"))]
    print(f"{len(pending)} queued meshes, {len(todo)} not yet baked")
    for number, obj_path in enumerate(todo, 1):
        name = os.path.splitext(os.path.basename(obj_path))[0]
        started = time.time()
        # process=False keeps the vertices exactly as the engine exported them.
        mesh = trimesh.load(obj_path, force="mesh", process=False)
        if len(mesh.faces) == 0:
            print(f"[{number}/{len(todo)}] {name}: no faces, skipped")
            continue
        grid_zyx, bounds_min, bounds_max, voxel, thin = bake_world_grid(mesh)
        out_path = os.path.join(cache_dir, name + ".sdf")
        # Written to a temporary name first, so an interrupted bake never
        # leaves a truncated volume the engine would treat as finished.
        write_sdf(out_path + ".partial", grid_zyx, bounds_min, bounds_max)
        os.replace(out_path + ".partial", out_path)
        print(f"[{number}/{len(todo)}] {name}: {len(mesh.faces)} faces, "
              f"voxel {voxel*100:.1f} cm, {'shell' if thin else 'signed'}, "
              f"{time.time() - started:.1f}s")


def write_sdf(path, grid_zyx, bounds_min, bounds_max):
    dim_z, dim_y, dim_x = grid_zyx.shape
    with open(path, "wb") as f:
        f.write(b"MSDF")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<III", dim_x, dim_y, dim_z))
        f.write(struct.pack("<3f", *bounds_min))
        f.write(struct.pack("<3f", *bounds_max))
        f.write(np.ascontiguousarray(grid_zyx, dtype="<f4").tobytes())
    print(
        f"Wrote {path}: {dim_x}x{dim_y}x{dim_z}, "
        f"bounds {bounds_min.tolist()} .. {bounds_max.tolist()}, "
        f"sdf range [{grid_zyx.min():.4f}, {grid_zyx.max():.4f}] world units")


def read_texels_as_gpu(path):
    """Read an .sdf back the way the GPU indexes it: returns (grid, dims,
    bounds_min, bounds_max) where grid[x, y, z] is the value a shader gets
    from texel (x, y, z)."""
    with open(path, "rb") as f:
        assert f.read(4) == b"MSDF"
        (version,) = struct.unpack("<I", f.read(4))
        assert version == 1
        dim_x, dim_y, dim_z = struct.unpack("<III", f.read(12))
        bounds_min = np.array(struct.unpack("<3f", f.read(12)))
        bounds_max = np.array(struct.unpack("<3f", f.read(12)))
        flat = np.frombuffer(f.read(), dtype="<f4")
    assert flat.size == dim_x * dim_y * dim_z
    x, y, z = np.meshgrid(
        np.arange(dim_x), np.arange(dim_y), np.arange(dim_z), indexing="ij")
    grid = flat[x + y * dim_x + z * dim_x * dim_y]
    return grid, (dim_x, dim_y, dim_z), bounds_min, bounds_max


def verify_axes(path, resolution):
    """Bake a shape with no mirror symmetry, read it back with the GPU's
    indexing, and check it against exact signed distances.

    A body with a small cube off one corner is different under every one of
    the 48 axis permutations and flips, so the stored orientation is correct
    only if the untransformed grid matches far better than all 47 others.
    The expected distances are analytic box distances, so the reference does
    not depend on the same mesh processing being tested.
    """
    body = trimesh.creation.box(extents=[1.2, 0.6, 0.4])
    body.apply_translation([-0.3, 0.0, 0.0])
    marker = trimesh.creation.box(extents=[0.3, 0.3, 0.3])
    marker.apply_translation([0.7, 0.35, 0.25])
    mesh = trimesh.util.concatenate([body, marker])

    grid_zyx, bounds_min, bounds_max = bake(mesh, resolution)
    write_sdf(path, grid_zyx, bounds_min, bounds_max)

    grid, dims, bmin, bmax = read_texels_as_gpu(path)
    centres = [
        bmin[a] + (np.arange(dims[a]) + 0.5) / dims[a] * (bmax[a] - bmin[a])
        for a in range(3)]
    px, py, pz = np.meshgrid(*centres, indexing="ij")
    def box_sdf(centre, half):
        q = np.stack([np.abs(px - centre[0]) - half[0],
                      np.abs(py - centre[1]) - half[1],
                      np.abs(pz - centre[2]) - half[2]])
        outside = np.linalg.norm(np.maximum(q, 0.0), axis=0)
        inside = np.minimum(np.max(q, axis=0), 0.0)
        return outside + inside

    # The two boxes do not overlap, so the union's exact distance is the
    # smaller of the two, with no extra dependency for a mesh query.
    expected = np.minimum(
        box_sdf([-0.3, 0.0, 0.0], [0.6, 0.3, 0.2]),
        box_sdf([0.7, 0.35, 0.25], [0.15, 0.15, 0.15]))

    # Voxels right on the surface can take either sign from the bake's
    # sampling; leave them out so the comparison measures orientation.
    spacing = (bmax - bmin) / np.array(dims)
    confident = np.abs(expected) > spacing.max()
    print(f"texel spacing {spacing.max():.4f}, "
          f"{int(confident.sum())} confident voxels")

    # Sign agreement alone separates orientations poorly: most voxels are
    # empty space far from the shape, and a flip across a near-symmetric
    # axis changes only a sliver of signs.  Mean distance error compares
    # every voxel's full value, so a wrong orientation cannot hide there.
    results = {}
    for order in itertools.permutations(range(3)):
        for flips in itertools.product([False, True], repeat=3):
            candidate = np.transpose(grid, order)
            for axis, flip in enumerate(flips):
                if flip:
                    candidate = np.flip(candidate, axis)
            mean_error = np.mean(np.abs(candidate - expected))
            agree = np.mean(np.sign(candidate[confident]) ==
                            np.sign(expected[confident]))
            results[(order, flips)] = (mean_error, agree)

    identity_key = ((0, 1, 2), (False, False, False))
    identity_error, identity_agree = results[identity_key]
    ranked = sorted(results.items(), key=lambda item: item[1][0])
    for (order, flips), (mean_error, agree) in ranked[:4]:
        label = "".join(("-" if f else "+") + "xyz"[o]
                        for o, f in zip(order, flips))
        print(f"  {label}: mean distance error {mean_error:.4f}, "
              f"sign agreement {agree:.4f}")
    best_other = min(v[0] for k, v in results.items() if k != identity_key)
    ok = (identity_agree > 0.999 and
          identity_error < 0.25 * spacing.max() and
          best_other > 3.0 * identity_error)
    print(f"orientation {'PASS' if ok else 'FAIL'}: identity mean error "
          f"{identity_error:.4f} vs best of {len(results) - 1} others "
          f"{best_other:.4f}; identity sign agreement {identity_agree:.4f}")
    sys.exit(0 if ok else 1)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == "--queue":
        bake_queue(sys.argv[2], sys.argv[3])
        return

    resolution = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    if sys.argv[1] == "--verify-axes":
        verify_axes(sys.argv[2], resolution)
        return
    if sys.argv[1] == "--test-sphere":
        mesh = trimesh.creation.icosphere(subdivisions=3, radius=1.0)
    elif len(sys.argv) > 4:
        scene = trimesh.load(sys.argv[1], force="scene")
        name = sys.argv[4]
        if name not in scene.geometry:
            print(f"no geometry named {name!r}; the file has "
                  f"{sorted(scene.geometry.keys())}")
            sys.exit(1)
        mesh = scene.geometry[name]
    else:
        mesh = trimesh.load(sys.argv[1], force="mesh")

    grid_zyx, bounds_min, bounds_max = bake(mesh, resolution)
    write_sdf(sys.argv[2], grid_zyx, bounds_min, bounds_max)


if __name__ == "__main__":
    main()
