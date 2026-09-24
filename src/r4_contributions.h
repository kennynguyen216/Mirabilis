#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "env_flags.h"

// R4 lighting-ownership instrumentation (docs/lumen_lite_design.md, R4.1).
// MIRABILIS_R4_CONTRIBUTIONS names the owners the final image keeps; every
// other owner's addition to the image is zeroed, while the transport buffers
// it feeds are left exactly as the full frame computes them.  Absent keeps
// every owner, so an ordinary run is untouched.
//
// Forward owners travel in sceneData.materialDebug.y, ray classes in bits
// 8.. of the SSGI trace's quality.w; the bit order here is the one
// mesh_shading.glsl and ssgi_body.glsl test.

namespace r4 {

inline constexpr uint32_t SunDiffuse = 1u << 0;
inline constexpr uint32_t SunSpecular = 1u << 1;
inline constexpr uint32_t EnvDiffuse = 1u << 2;
inline constexpr uint32_t EnvSpecular = 1u << 3;
inline constexpr uint32_t Emission = 1u << 4;
inline constexpr uint32_t Background = 1u << 5;
inline constexpr uint32_t EmitterDiffuse = 1u << 6;
inline constexpr uint32_t EmitterSpecular = 1u << 7;
inline constexpr uint32_t AllForward = (1u << 8) - 1u;

inline constexpr uint32_t RayScreenHit = 1u << 0;
inline constexpr uint32_t RayScreenHitCache = 1u << 1;
inline constexpr uint32_t RayExit = 1u << 2;
inline constexpr uint32_t RayExhausted = 1u << 3;
inline constexpr uint32_t RayUnusableHit = 1u << 4;
inline constexpr uint32_t RayPortal = 1u << 5;
inline constexpr uint32_t RayFieldCache = 1u << 6;
inline constexpr uint32_t RayFieldUncovered = 1u << 7;
inline constexpr uint32_t RayFieldExit = 1u << 8;
inline constexpr uint32_t RayFieldExhausted = 1u << 9;
inline constexpr uint32_t RayScreenHitUncached = 1u << 10;
inline constexpr uint32_t RayPrimaryCacheSky = 1u << 11;
inline constexpr uint32_t AllRays = (1u << 12) - 1u;

struct Owner {
    std::string_view name;
    uint32_t forward;
    uint32_t ray;
};

inline constexpr std::array<Owner, 20> Owners{{
    {"sun_diffuse", SunDiffuse, 0},
    {"sun_specular", SunSpecular, 0},
    {"env_diffuse", EnvDiffuse, 0},
    {"env_specular", EnvSpecular, 0},
    {"emission", Emission, 0},
    {"background", Background, 0},
    {"emitter_diffuse", EmitterDiffuse, 0},
    {"emitter_specular", EmitterSpecular, 0},
    {"ray_screen_hit", 0, RayScreenHit},
    {"ray_screen_hit_cache", 0, RayScreenHitCache},
    {"ray_exit", 0, RayExit},
    {"ray_exhausted", 0, RayExhausted},
    {"ray_unusable_hit", 0, RayUnusableHit},
    {"ray_portal", 0, RayPortal},
    {"ray_field_cache", 0, RayFieldCache},
    {"ray_field_uncovered", 0, RayFieldUncovered},
    {"ray_field_exit", 0, RayFieldExit},
    {"ray_field_exhausted", 0, RayFieldExhausted},
    {"ray_screen_hit_uncached", 0, RayScreenHitUncached},
    {"ray_primary_cache_sky", 0, RayPrimaryCacheSky},
}};

struct Contributions {
    bool valid;
    uint32_t forward;
    uint32_t ray;

    constexpr bool operator==(const Contributions& other) const
    {
        return valid == other.valid && forward == other.forward && ray == other.ray;
    }
};

// Strict: an empty list, an empty entry or an unknown name is invalid rather
// than read as "keep nothing" or skipped, because a capture labelled with
// owners it did not isolate is worse than no capture.
constexpr Contributions parse_contributions(const char* value)
{
    if (value == nullptr) {
        return {true, AllForward, AllRays};
    }
    std::string_view rest = value;
    Contributions result{true, 0, 0};
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view name = env_flags_detail::trim(rest.substr(0, comma));
        bool known = false;
        for (const Owner& owner : Owners) {
            if (owner.name == name) {
                result.forward |= owner.forward;
                result.ray |= owner.ray;
                known = true;
            }
        }
        if (!known) {
            return {false, 0, 0};
        }
        if (comma == std::string_view::npos) {
            return result;
        }
        rest = rest.substr(comma + 1);
    }
}

static_assert(parse_contributions(nullptr) == Contributions{true, AllForward, AllRays});
static_assert(parse_contributions("sun_diffuse") == Contributions{true, SunDiffuse, 0});
static_assert(parse_contributions("background, ray_exit") ==
    Contributions{true, Background, RayExit});
static_assert(parse_contributions("ray_field_exhausted") ==
    Contributions{true, 0, RayFieldExhausted});
static_assert(!parse_contributions("").valid);
static_assert(!parse_contributions("sun_diffuse,").valid);
static_assert(!parse_contributions("sun").valid);
static_assert(!parse_contributions("ssgi").valid);

// MIRABILIS_R4_CACHE_SOURCES: which direct sources the surface cache keeps
// (R4.13 diagnostic).  Absent keeps all three; anything unknown is invalid.
inline constexpr uint32_t CacheSun = 1u << 0;
inline constexpr uint32_t CacheSky = 1u << 1;
inline constexpr uint32_t CacheEmitters = 1u << 2;

struct CacheSources {
    bool valid;
    uint32_t mask;

    constexpr bool operator==(const CacheSources& other) const
    {
        return valid == other.valid && mask == other.mask;
    }
};

constexpr CacheSources parse_cache_sources(const char* value)
{
    if (value == nullptr) {
        return {true, CacheSun | CacheSky | CacheEmitters};
    }
    std::string_view rest = value;
    uint32_t mask = 0;
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view name = env_flags_detail::trim(rest.substr(0, comma));
        if (name == "sun") mask |= CacheSun;
        else if (name == "sky") mask |= CacheSky;
        else if (name == "emitter") mask |= CacheEmitters;
        else return {false, 0};
        if (comma == std::string_view::npos) return {true, mask};
        rest = rest.substr(comma + 1);
    }
}

static_assert(parse_cache_sources(nullptr) == CacheSources{true, 7u});
static_assert(parse_cache_sources("sun") == CacheSources{true, CacheSun});
static_assert(parse_cache_sources("sky, emitter") == CacheSources{true, CacheSky | CacheEmitters});
static_assert(!parse_cache_sources("").valid);
static_assert(!parse_cache_sources("sun,").valid);
static_assert(!parse_cache_sources("moon").valid);

// MIRABILIS_R4_LIGHT_SOURCES: which scene light sources exist at all, for
// source-isolated references and candidates (R4.13).  It edits the scene's
// reference lighting, which both renderers read, so an isolated reference
// and an isolated candidate see the same light.  Absent keeps both.
inline constexpr uint32_t LightSun = 1u << 0;
inline constexpr uint32_t LightSky = 1u << 1;

constexpr CacheSources parse_light_sources(const char* value)
{
    if (value == nullptr) {
        return {true, LightSun | LightSky};
    }
    std::string_view rest = value;
    uint32_t mask = 0;
    while (true) {
        const std::size_t comma = rest.find(',');
        const std::string_view name = env_flags_detail::trim(rest.substr(0, comma));
        if (name == "sun") mask |= LightSun;
        else if (name == "sky") mask |= LightSky;
        else return {false, 0};
        if (comma == std::string_view::npos) return {true, mask};
        rest = rest.substr(comma + 1);
    }
}

static_assert(parse_light_sources(nullptr) == CacheSources{true, 3u});
static_assert(parse_light_sources("sky") == CacheSources{true, LightSky});
static_assert(!parse_light_sources("emitter").valid);
static_assert(!parse_light_sources("").valid);

}  // namespace r4
