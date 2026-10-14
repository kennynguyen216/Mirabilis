#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "scene_queries.h"
#include "scene_serialization.h"
static_assert(std::is_same_v<decltype(EntityRef::persistentId), uint64_t>);
static_assert(sizeof(entt::entity) == sizeof(uint32_t));
TEST_CASE("extracted core keeps hierarchy and player collision") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("root");
    const auto child = scene.create_object("box", root);
    Transform transform;
    transform.position.x = 2;
    REQUIRE(scene.set_local_transform(root, transform));
    Collider collider;
    collider.hasCollision = true;
    REQUIRE(scene.set(child, collider));
    CHECK(scene.world_matrix(child)[3].x == doctest::Approx(2));
    const auto collision = collect_player_collision(scene, {});
    REQUIRE(collision.walls.size() == 1);
    CHECK(collision.walls[0].min.x == doctest::Approx(1.5));
    CHECK(scene.destroy_object(root));
    CHECK(scene.pending(child));
    CHECK(scene.drain_destruction() == 2);
    CHECK_FALSE(scene.valid(child));
}
