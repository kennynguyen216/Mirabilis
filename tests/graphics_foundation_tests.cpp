#include "draw_instance.h"
#include "mesh_clusters.h"
#include "scene_serialization.h"
#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <set>

TEST_CASE("draw instances preserve normal orthogonality and mirrored tangent handedness") {
    const auto world = glm::rotate(glm::mat4(1), 0.6f, glm::vec3(0, 1, 0)) *
                       glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f));
    auto previous = world;
    previous[3] = glm::vec4(3, 4, 5, 1);
    const auto instance = make_draw_instance(world, previous, true);
    const glm::vec3 normal = glm::normalize(glm::vec3(1, 1, 0));
    const glm::vec3 tangent = glm::normalize(glm::vec3(1, -1, 0));
    glm::vec3 transformed;
    for (int row = 0; row < 3; ++row)
        transformed[row] = glm::dot(glm::vec3(instance.normalRows[row]), normal);
    CHECK(glm::dot(glm::normalize(transformed), glm::normalize(glm::mat3(world) * tangent)) ==
          doctest::Approx(0).epsilon(1e-5));
    CHECK(instance.flags.x == 1);
    CHECK(instance.flags.y == -1);
    CHECK(instance.previousRows[0].w == 3);
    CHECK(instance.previousRows[1].w == 4);
    CHECK(instance.previousRows[2].w == 5);
    const auto collapsed =
        make_draw_instance(glm::scale(glm::mat4(1), glm::vec3(0)), glm::mat4(1), false);
    for (auto row : collapsed.normalRows)
        for (int i = 0; i < 4; ++i)
            CHECK(std::isfinite(row[i]));
}

TEST_CASE("mesh clusters preserve all triangles and conservatively contain their vertices") {
    struct Vertex {
        glm::vec3 position;
    };
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 300; ++i) {
        vertices.push_back({{float(i), float(i % 7), -float(i % 3)}});
        indices.push_back(i);
    }
    const auto clusters =
        build_mesh_clusters<Vertex>(vertices, indices, 0, uint32_t(indices.size()));
    REQUIRE(clusters.size() > 1);
    uint32_t next = 0;
    for (const auto &cluster : clusters) {
        CHECK(cluster.firstIndex == next);
        CHECK(cluster.indexCount % 3 == 0);
        CHECK(cluster.indexCount <= 180);
        CHECK(cluster.vertexCount <= 32);
        std::set<uint32_t> unique;
        for (uint32_t i = cluster.firstIndex; i < cluster.firstIndex + cluster.indexCount; ++i) {
            unique.insert(indices[i]);
            const auto p = vertices[indices[i]].position;
            CHECK(glm::all(glm::greaterThanEqual(p, glm::vec3(cluster.minimum))));
            CHECK(glm::all(glm::lessThanEqual(p, glm::vec3(cluster.maximum))));
        }
        CHECK(unique.size() == cluster.vertexCount);
        next += cluster.indexCount;
    }
    CHECK(next == indices.size());
    CHECK(build_mesh_clusters<Vertex>(vertices, indices, 1, 300).empty());
    indices[1] = 999;
    CHECK(build_mesh_clusters<Vertex>(vertices, indices, 0, 300).empty());
}

TEST_CASE("lighting and material authoring survive save reload duplication and play restoration") {
    mirabilis::Scene scene;
    const auto ref = scene.create_object("light and material");
    Light light;
    light.type = LightType::Spot;
    light.color = {0.3f, 0.6f, 1};
    light.intensity = 40;
    light.range = 12;
    REQUIRE(scene.set(ref, light));
    SceneMaterial material;
    material.enabled = true;
    material.doubleSided = true;
    material.celShading = true;
    material.specular = 0.25f;
    material.occlusionTexturePath = "ao.png";
    material.occlusionStrength = 0.7f;
    material.specularTexturePath = "specular.png";
    material.subsurface = 0.2f;
    material.subsurfaceColor = {1, 0.4f, 0.2f};
    material.transmission = 0.6f;
    material.ior = 1.4f;
    material.thickness = 0.3f;
    REQUIRE(scene.set(ref, material));
    SceneSettings settings;
    settings.celShading = true;
    settings.bloom = true;
    settings.celBands = 6;
    settings.bloomScatter = 0.8f;
    settings.tonemapOperator = 2;
    std::string json, error;
    REQUIRE(serialize_scene_json(scene, settings, {}, json, error));
    StagedScene loaded;
    REQUIRE(parse_scene_json(json, "graphics.json", {}, loaded, error));
    const auto restored = loaded.scene.get(ref.persistentId);
    REQUIRE(loaded.scene.read<Light>(restored));
    CHECK(loaded.scene.read<Light>(restored)->type == LightType::Spot);
    CHECK(loaded.scene.read<Light>(restored)->intensity == 40);
    REQUIRE(loaded.scene.read<SceneMaterial>(restored));
    CHECK(*loaded.scene.read<SceneMaterial>(restored) == material);
    CHECK(loaded.settings.celShading);
    CHECK(loaded.settings.bloom);
    CHECK(loaded.settings.celBands == 6);
    CHECK(loaded.settings.tonemapOperator == 2);
    const auto duplicate = loaded.scene.duplicate_subtree(restored);
    REQUIRE(duplicate);
    CHECK(loaded.scene.read<Light>(duplicate)->range == 12);
    auto snapshot = loaded.scene.take_snapshot();
    Light changed = light;
    changed.intensity = 100;
    REQUIRE(loaded.scene.set(restored, changed));
    REQUIRE(loaded.scene.restore_snapshot(snapshot));
    CHECK(loaded.scene.read<Light>(loaded.scene.get(ref.persistentId))->intensity == 40);
    snapshot.entities[0].light->range = -1;
    CHECK_FALSE(loaded.scene.restore_snapshot(snapshot));
}

TEST_CASE("invalid lighting and rendering values leave the existing scene untouched") {
    Light light;
    light.innerCone = light.outerCone + 1;
    CHECK_FALSE(valid_component(light));
    light = Light{};
    light.intensity = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(valid_component(light));
    light = Light{};
    light.range = 0;
    CHECK_FALSE(valid_component(light));
    SceneMaterial material;
    material.subsurface = -1;
    CHECK_FALSE(valid_component(material));
    material = SceneMaterial{};
    material.thickness = std::numeric_limits<float>::infinity();
    CHECK_FALSE(valid_component(material));
    mirabilis::Scene scene;
    scene.create_object("existing");
    SceneSettings settings;
    settings.bloomScatter = 2;
    std::string json = "preserved", error;
    CHECK_FALSE(serialize_scene_json(scene, settings, {}, json, error));
    CHECK(json == "preserved");
    StagedScene loaded;
    loaded.scene.create_object("preserved");
    CHECK_FALSE(parse_scene_json(
        R"({"version":2,"rendering":{"celBands":0},"objects":[],"authoredPortals":[]})", "bad.json",
        {}, loaded, error));
    REQUIRE(loaded.scene.creation_order().size() == 1);
    CHECK(loaded.scene.read<Name>(loaded.scene.creation_order()[0])->value == "preserved");
}

TEST_CASE("inactive lights preserve authored values without consuming rendering budgets") {
    Light light;
    CHECK(light.emits_light());
    light.intensity = 0;
    CHECK(valid_component(light));
    CHECK_FALSE(light.emits_light());
    light.intensity = 20;
    light.color = glm::vec3(0);
    CHECK(valid_component(light));
    CHECK_FALSE(light.emits_light());
    light.color.g = 0.1f;
    CHECK(light.emits_light());
    light.enabled = false;
    CHECK_FALSE(light.emits_light());
}
