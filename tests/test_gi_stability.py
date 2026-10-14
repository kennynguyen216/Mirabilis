"""Regression contracts for GI lighting and denoising; no GPU required.

Run: python -m unittest discover -s tests -p test_gi_stability.py
Shader compilation and bounded renderer captures exercise the GPU paths.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def shader(name):
    return (ROOT / "shaders" / name).read_text(encoding="utf-8")


class GIStability(unittest.TestCase):
    def test_depth_reconstruction_uses_the_sampled_full_resolution_pixel(self):
        for name in ("ssgi_body.glsl", "ssgi_temporal.comp", "ssgi_bilateral.comp", "ssgi_composite.frag"):
            self.assertIn("ssgi_geometry_uv(", shader(name), name)
        body = shader("ssgi_body.glsl")
        origin = body.split("TraceOrigin trace_origin(")[1].split("return o;")[0]
        self.assertLess(origin.index("screenUV = ssgi_geometry_uv"), origin.index("view_position(screenUV"))
        temporal = shader("ssgi_temporal.comp")
        self.assertIn("vec4(geometryUV * 2.0 - 1.0, depth, 1.0)", temporal)
        self.assertIn("ssgi_view_position(expectedUV, expectedDepth", temporal)

    def test_screen_cache_hits_do_not_depend_on_screen_history(self):
        body = shader("ssgi_body.glsl")
        hit = body.split("bool unusableHit = false;")[1].split("#else")[0]
        self.assertNotIn("PushConstants.control.z", hit)
        self.assertIn("LUMEN_SURFACE_LOOKUP(hitWorld.xyz / hitWorld.w, hitNormal)", hit)
        self.assertIn("hit = false;", hit)  # Uncovered hits use the world path.
        self.assertIn("#define LUMEN_SURFACE_LOOKUP(p, n) surface_cache_lookup(p, n, false, true, true)", body)

    def test_history_checks_each_tap_in_world_space(self):
        body = shader("ssgi_temporal.comp")
        self.assertIn("encode_octahedron(worldNormal)", body)
        self.assertIn("dot(worldNormal, decode_octahedron(metadata.gb))", body)
        self.assertNotIn("texture(previousMetadata", body)
        self.assertNotIn("texture(previousTemporalIndirect", body)
        tap = body.split("for (int i = 0; i < 4; ++i)")[1].split("accepted = historyWeightSum")[0]
        for field in ("metadata.r <= 1e-6", "metadata.gb", "metadata.a - currentPortal"):
            self.assertIn(field, tap)
        self.assertLess(tap.index("metadata.a - currentPortal"), tap.index("history +="))
        age_cap = float(re.search(r"min\(historyLength \+ 1.0, ([\d.]+)\)", body).group(1))
        self.assertGreaterEqual(age_cap / (age_cap + 1), .99,
                                "History age must allow the supported 0.99 weight")

    def test_filter_and_upsample_preserve_planes_and_reject_sky(self):
        for name in ("ssgi_bilateral.comp", "ssgi_composite.frag"):
            body = shader(name)
            self.assertIn("ssgi_plane_weight(centerPosition", body)
            self.assertIn("<= 1e-6", body)
        self.assertNotIn("decode_octahedron(metadata.gb)", shader("ssgi_composite.frag"))
        # The old sign(0) fold aliased an exact -Z normal to +Z.
        self.assertNotIn("sign(", shader("ssgi_common.glsl"))

    def test_radiosity_filters_the_cached_sky_source(self):
        body = shader("surface_cache_radiosity.comp")
        self.assertIn("SURFACE_CACHE_READ_SKY(texel) texelFetch(cacheSky", body)
        self.assertIn("surface_cache_lookup(p, field_normal(p), false, true, true)", body)
        lookup = shader("surface_cache_lookup.glsl")
        self.assertIn("cardUV, texel, sameSurfaceMargin, 2, lightingRadius)", lookup,
                      "GI must filter the cached sun shadow as well as the sky")
        cache = (ROOT / "src" / "vk_engine_surface_cache.cpp").read_text(encoding="utf-8")
        update = cache.split("void VulkanEngine::update_surface_cache_radiosity(")[1]
        self.assertIn("writer.write_image(11, _surfaceCache.sky.imageView", update)

    def test_hardware_world_trace_uses_the_closest_hit_at_the_surface(self):
        body = shader("lumen_world_trace.glsl")
        hardware = body.split("#ifdef MIRABILIS_RT_SHADOWS")[1].split("#else")[0]
        self.assertIn("worldPosition + worldNormal * epsilon", hardware)
        self.assertNotIn("2.5 * voxel", hardware)
        self.assertNotIn("TerminateOnFirstHit", hardware,
                         "GI needs the closest surface, not traversal's first candidate")
        self.assertIn("rayQueryGetIntersectionTEXT(query, true)", hardware)
        self.assertIn("lumen_hit_radiance(p, rayClass)", hardware)
        self.assertIn("return lumen_hit_radiance(p, rayClass);", body.split("#else")[1])
        engine = (ROOT / "src" / "vk_engine.cpp").read_text(encoding="utf-8")
        self.assertLess(engine.index("_ssgi.screenTraceEnabled = !_rayQueryShadows;"),
                        engine.index('apply_env_flag("MIRABILIS_SSGI_SCREEN_TRACE"'),
                        "Use exact world tracing by default, retaining explicit overrides")

    def test_radiosity_strips_preserve_full_card_uvs_and_blend(self):
        body = shader("surface_cache_radiosity.comp")
        self.assertIn("uvec2 local = work + PushConstants.tile.xy", body)
        self.assertIn("(vec2(local) + 0.5) / vec2(PushConstants.rect.zw)", body)
        cache = (ROOT / "src" / "vk_engine_surface_cache.cpp").read_text(encoding="utf-8")
        self.assertIn("(budget - texels) / rect.z", cache)
        self.assertIn("if (regionUpdate.row == 0)", cache)
        self.assertIn("card.rect.y + regionUpdate.row", cache)
        self.assertIn("region.extent = {card.rect.z, regionUpdate.rows, 1}", cache)

    def test_pending_gpu_work_is_not_a_fatal_timeout(self):
        body = (ROOT / "src" / "vk_engine.cpp").read_text(encoding="utf-8")
        self.assertIn("while (result == VK_TIMEOUT)", body)
        self.assertIn("wait_for_gpu_fence(_device, get_current_frame()._renderFence)", body)
        self.assertIn("wait_for_gpu_fence(_device, _immFence)", body)
        self.assertIn("if (acquireResult == VK_TIMEOUT || acquireResult == VK_NOT_READY) return", body)
        self.assertIn("draw_frame_ui(wallDeltaTime)", body,
                      "FPS must report real time, not the physics clamp's 10 FPS floor")

    def test_scene_rebuild_reserves_packing_memory_before_appending(self):
        body = (ROOT / "src" / "vk_engine_path_trace.cpp").read_text(encoding="utf-8")
        body = body.split("void VulkanEngine::update_trace_scene()")[1]
        self.assertLess(body.index("triangles.reserve(triangleCapacity)"), body.index("triangles.push_back(t)"))
        self.assertLess(body.index("texels.reserve(texelCapacity)"), body.index("texels.insert("))
        self.assertIn("textures.clear();", body,
                      "Counting unique textures must not mark them as already packed")

    def test_portal_receiver_denoises_without_widening_ray_hit_filters(self):
        portal = shader("portal_indirect.glsl")
        self.assertIn("surface_cache_lookup(world, normal, false, true, true, 3)", portal)
        self.assertIn("#define LUMEN_SURFACE_LOOKUP(p, n) surface_cache_lookup(p, n, false, true, true)", portal)
        self.assertIn("readSky, bilinearSky, readLighting, 1)", shader("surface_cache_lookup.glsl"))

    def test_cache_history_counts_samples_not_skipped_frames(self):
        cache = (ROOT / "src" / "vk_engine_surface_cache.cpp").read_text(encoding="utf-8")
        self.assertIn("1.0f / float(updates + 1), _surfaceCache.radiosityBlend", cache)
        self.assertNotIn("cardLastUpdate", cache)
        relight = cache.split("void VulkanEngine::light_surface_cache()")[1].split("void VulkanEngine::measure_surface_cache_coverage")[0]
        self.assertIn("std::fill(_surfaceCache.cardUpdates.begin(), _surfaceCache.cardUpdates.end(), 0)", relight)
        self.assertIn("_surfaceCache.radiosityRow = 0", relight)


if __name__ == "__main__":
    unittest.main()
