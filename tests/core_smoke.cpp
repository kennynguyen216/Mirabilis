#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "scene_queries.h"
TEST_CASE("extracted core keeps hierarchy and player collision") {
    Scene scene;
    const auto root = scene.create_object("root");
    const auto child = scene.create_object("box", root);
    scene.get(root)->localTransform.position.x = 2;
    scene.get(child)->hasCollision = true;
    CHECK(scene.world_matrix(child)[3].x == doctest::Approx(2));
    const auto collision = collect_player_collision(scene, {});
    REQUIRE(collision.walls.size() == 1);
    CHECK(collision.walls[0].min.x == doctest::Approx(1.5));
    CHECK(scene.destroy_object(root));
    CHECK(scene.get(child) == nullptr);
}
