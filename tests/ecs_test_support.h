#pragma once
#include <doctest/doctest.h>
#include "scene_serialization.h"
#include <cmath>
#include <fstream>
#include <iterator>

namespace ecs_test {
inline bool near(double actual, double expected) {
    return std::abs(actual - expected) <= 1e-5 + 1e-6 * std::abs(expected);
}
template<class V> void check_vector(const V& actual, const V& expected) {
    for (int i = 0; i < actual.length(); ++i) CHECK(near(actual[i], expected[i]));
}
inline void check_matrix(const glm::mat4& actual, const glm::mat4& expected) {
    for (int c = 0; c < 4; ++c) check_vector(actual[c], expected[c]);
}
inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.is_open(), path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
inline std::vector<PersistentID> ids(const std::vector<EntityRef>& refs) {
    std::vector<PersistentID> result;
    for (auto ref : refs) result.push_back(ref.persistentId);
    return result;
}
inline void check_state(const EntityState& a, const EntityState& b) {
    CHECK(a.id == b.id); CHECK(a.name.value == b.name.value);
    check_vector(a.transform.position, b.transform.position);
    check_vector(a.transform.rotation, b.transform.rotation);
    check_vector(a.transform.scale, b.transform.scale);
    CHECK(a.hierarchy.parent == b.hierarchy.parent);
    CHECK(a.hierarchy.children == b.hierarchy.children);
    CHECK(a.visibility.visible == b.visibility.visible);
    CHECK(a.visibility.layer == b.visibility.layer);
    CHECK(a.mesh.assetKind == b.mesh.assetKind); CHECK(a.mesh.modelPath == b.mesh.modelPath);
    const auto& m = a.material; const auto& n = b.material;
    CHECK(m.enabled == n.enabled); CHECK(m.debugChecker == n.debugChecker);
    CHECK(m.baseColorTexturePath == n.baseColorTexturePath);
    CHECK(m.normalTexturePath == n.normalTexturePath);
    CHECK(m.metalRoughTexturePath == n.metalRoughTexturePath);
    check_vector(m.colorTint, n.colorTint); check_vector(m.uvScale, n.uvScale);
    CHECK(near(m.metallic, n.metallic)); CHECK(near(m.roughness, n.roughness));
    check_vector(m.emissionColor, n.emissionColor);
    CHECK(near(m.emissionStrength, n.emissionStrength)); CHECK(near(m.transmission, n.transmission));
    CHECK(near(m.ior, n.ior));
    CHECK(a.collider.hasCollision == b.collider.hasCollision);
    CHECK(a.collider.collisionShape == b.collider.collisionShape);
    check_vector(a.collider.colliderCenter, b.collider.colliderCenter);
    check_vector(a.collider.colliderHalfExtents, b.collider.colliderHalfExtents);
    CHECK(a.timeTrial.role == b.timeTrial.role);
    CHECK(a.portalPlaceable == b.portalPlaceable);
    CHECK(a.transformDrivenExternally == b.transformDrivenExternally);
    CHECK(a.unknown.blocks == b.unknown.blocks);
}
inline void check_snapshot(const SceneSnapshot& a, const SceneSnapshot& b) {
    CHECK(a.roots == b.roots);
    REQUIRE(a.entities.size() == b.entities.size());
    for (size_t i = 0; i < a.entities.size(); ++i) check_state(a.entities[i], b.entities[i]);
}
inline void check_settings(const SceneSettings& a, const SceneSettings& b) {
    CHECK(a.nextActor == b.nextActor); CHECK(a.skybox == b.skybox);
    check_vector(a.sunDirection,b.sunDirection); CHECK(a.shadowsEnabled == b.shadowsEnabled);
    CHECK(near(a.shadowRadius,b.shadowRadius)); CHECK(near(a.shadowDepthBias,b.shadowDepthBias));
    CHECK(near(a.shadowNormalBias,b.shadowNormalBias)); check_vector(a.sunRadiance,b.sunRadiance);
    CHECK(near(a.environmentIntensity,b.environmentIntensity)); check_vector(a.environmentTint,b.environmentTint);
    CHECK(a.blackEnvironment == b.blackEnvironment); CHECK(near(a.warmth,b.warmth));
    CHECK(near(a.tint,b.tint)); CHECK(near(a.giSaturation,b.giSaturation));
    REQUIRE(a.ssao.has_value() == b.ssao.has_value());
    if (a.ssao) {
        CHECK(a.ssao->enabled == b.ssao->enabled); CHECK(near(a.ssao->radius,b.ssao->radius));
        CHECK(near(a.ssao->bias,b.ssao->bias)); CHECK(near(a.ssao->intensity,b.ssao->intensity));
        CHECK(near(a.ssao->power,b.ssao->power));
    }
}
inline void check_portal(const Portal& a, const Portal& b) {
    CHECK(a.placed == b.placed); CHECK(a.hostWallObject == b.hostWallObject);
    check_vector(a.position,b.position); check_vector(a.normal,b.normal); check_vector(a.up,b.up);
    CHECK(near(a.halfWidth,b.halfWidth)); CHECK(near(a.halfHeight,b.halfHeight));
}
} // namespace ecs_test
