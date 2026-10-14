#include "ecs_test_support.h"
#include "physics_world.h"
#include "scene_queries.h"
#include <limits>
#include <set>
#include <chrono>

using mirabilis::PhysicsWorld;
using ecs_test::check_matrix;
using ecs_test::check_vector;
namespace {
constexpr float dt = 1.0f / 120.0f;
EntityRef box(Scene& scene, glm::vec3 position = {}, bool dynamic = false,
    std::optional<EntityRef> parent = std::nullopt) {
    const auto ref = scene.create_object("test box", parent);
    REQUIRE(ref);
    Transform transform; transform.position = position;
    REQUIRE(scene.set_local_transform(ref, transform));
    Collider collider; collider.hasCollision = true;
    REQUIRE(scene.set(ref, collider));
    if (dynamic) {
        RigidBody body; body.motion = BodyMotion::Dynamic;
        REQUIRE(scene.set(ref, body));
    }
    return ref;
}
void tick(PhysicsWorld& physics, Scene& scene) {
    scene.apply_commands();
    REQUIRE(physics.synchronize(scene, false));
    physics.step(scene, dt);
    scene.drain_destruction([&](EntityRef ref) { physics.remove(ref); });
}
std::set<PersistentID> resets(PhysicsWorld& physics) {
    const auto values = physics.consume_history_resets();
    return {values.begin(), values.end()};
}
std::string object_json(int id, std::string parent, std::string transform,
    std::string body = "", std::string collider = R"({"enabled":true,"shape":0,"center":[0,0,0],"halfExtents":[0.5,0.5,0.5]})",
    bool portalPlaceable = false) {
    return "{\"id\":" + std::to_string(id) + ",\"parent\":" + parent +
        ",\"siblingIndex\":0,\"components\":{\"transform\":" + transform +
        ",\"collider\":" + collider + (body.empty() ? "" : ",\"rigidBody\":" + body) +
        (portalPlaceable ? ",\"portalPlaceable\":{}" : "") + "}}";
}
const std::string identity = R"({"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]})";
const std::string dynamic = R"({"motion":"Dynamic","mass":2,"friction":0.3,"restitution":0.2,"gravityFactor":1})";
std::string document(std::string objects) {
    return R"({"version":2,"nextActor":1,"nextPersistentId":100,"authoredPortals":[],"objects":[)" + objects + "]}";
}
}

TEST_CASE("physics membership follows collider components and dynamic ancestor") {
    Scene scene; PhysicsWorld physics;
    const auto wall = box(scene), moving = box(scene,{0,4,0},true);
    const auto child = box(scene,{},false,moving);
    REQUIRE(scene.set(child,RigidBody{}));
    const auto floor = box(scene,{0,-2,0}), ramp = box(scene,{4,0,0});
    auto collider = *scene.read<Collider>(floor); collider.collisionShape = CollisionShape::GroundPlane;
    REQUIRE(scene.set(floor,collider)); collider.collisionShape = CollisionShape::SurfRamp;
    REQUIRE(scene.set(ramp,collider));
    const auto disabled = scene.create_object("disabled static settings");
    REQUIRE(scene.set(disabled,RigidBody{}));
    REQUIRE(physics.synchronize(scene,true)); CHECK(physics.body_count() == 4);
    CHECK(physics.has_body(wall)); CHECK(physics.has_body(floor)); CHECK(physics.has_body(ramp));
    CHECK(physics.has_body(moving)); CHECK_FALSE(physics.has_body(child)); CHECK_FALSE(physics.has_body(disabled));
    CHECK_FALSE(scene.has<TransformDrivenExternally>(moving));
    const auto initial = scene.world_matrix(moving);
    REQUIRE(physics.synchronize(scene,true)); check_matrix(scene.world_matrix(moving),initial);
    REQUIRE(physics.synchronize(scene,false)); CHECK(scene.has<TransformDrivenExternally>(moving));
    CHECK(collect_player_collision(scene,{}).walls.size() == 1);
    CHECK(collect_player_collision(scene,{}).groundPlanes.size() == 1);
    CHECK(collect_player_collision(scene,{}).surfRamps.size() == 1);
    REQUIRE(scene.remove<RigidBody>(moving)); REQUIRE(physics.synchronize(scene,true));
    CHECK(physics.has_body(child)); CHECK(physics.body_count() == 5);
    REQUIRE(scene.destroy_object(wall));
    scene.drain_destruction([&](EntityRef ref) { CHECK(scene.valid(ref)); physics.remove(ref); });
    CHECK_FALSE(physics.has_body(wall)); CHECK(physics.body_count() == 4);
    physics.clear(); CHECK(physics.body_count() == 0);
}

TEST_CASE("dynamic body edits validate combined fields and root ownership") {
    Scene scene; const auto moving = box(scene,{},true), parent = scene.create_object("parent");
    CHECK_FALSE(scene.set_parent(moving,parent));
    const auto child = box(scene,{},false,parent);
    auto body = *scene.read<RigidBody>(moving);
    CHECK_FALSE(scene.set(child,body));
    for (auto bad : {0.0f,-1.0f,std::numeric_limits<float>::infinity()}) {
        body.mass = bad; CHECK_FALSE(scene.set(moving,body));
    }
    body.mass = 1; body.friction = -0.1f; CHECK_FALSE(scene.set(moving,body));
    body.friction = .5f;
    for (float bad : {-0.1f,1.1f}) { body.restitution = bad; CHECK_FALSE(scene.set(moving,body)); }
    body.restitution = 0; body.gravityFactor = -2; REQUIRE(scene.set(moving,body));
    body.gravityFactor = std::numeric_limits<float>::quiet_NaN(); CHECK_FALSE(scene.set(moving,body));
    auto collider = *scene.read<Collider>(moving); collider.hasCollision = false;
    CHECK_FALSE(scene.set(moving,collider)); collider.hasCollision = true;
    collider.collisionShape = CollisionShape::GroundPlane; CHECK_FALSE(scene.set(moving,collider));
    collider.collisionShape = CollisionShape::Box; collider.colliderHalfExtents.x = 0;
    CHECK_FALSE(scene.set(moving,collider));
    auto transform = *scene.read<Transform>(moving); transform.scale.y = 0;
    CHECK_FALSE(scene.set_local_transform(moving,transform));
}

TEST_CASE("editor dirty parent and shape edits update native body without stepping") {
    Scene scene; PhysicsWorld physics; const auto parent = scene.create_object("noncollider parent");
    const auto child = box(scene,{1,2,3},false,parent);
    REQUIRE(physics.synchronize(scene,true));
    Transform transform; transform.position = {7,8,9}; transform.rotation.y = .4f; transform.scale = {2,3,4};
    REQUIRE(scene.set_local_transform(parent,transform)); REQUIRE(physics.synchronize(scene,true));
    REQUIRE(physics.body_world(child)); check_matrix(*physics.body_world(child),scene.world_matrix(child));
    const auto before = physics.body_bounds(child); REQUIRE(before);
    auto collider = *scene.read<Collider>(child); collider.colliderHalfExtents *= 2;
    REQUIRE(scene.set(child,collider)); REQUIRE(physics.synchronize(scene,true));
    const auto after = physics.body_bounds(child); REQUIRE(after);
    CHECK(glm::length(after->max-after->min) > glm::length(before->max-before->min)*1.9f);
    auto local = *scene.read<Transform>(child); local.rotation.z = .7f;
    REQUIRE(scene.set_local_transform(child,local)); CHECK(mirabilis::world_transform_has_shear(scene.world_matrix(child)));
    REQUIRE(physics.synchronize(scene,true)); CHECK_FALSE(physics.warnings().empty());
    Transform exact; CHECK_FALSE(mirabilis::exact_world_trs(scene.world_matrix(child),exact));
    const auto best = transform_from_matrix(scene.world_matrix(child));
    REQUIRE(physics.body_world(child)); check_matrix(*physics.body_world(child),to_matrix(best));
    auto singular = transform; singular.scale.x = 0;
    CHECK_FALSE(scene.set_local_transform(parent,singular)); check_matrix(scene.local_matrix(parent),to_matrix(transform));
    const auto precise = box(scene); REQUIRE(physics.synchronize(scene,true));
    REQUIRE(physics.body_bounds(precise)); const auto beforeWidth = physics.body_bounds(precise)->max.x - physics.body_bounds(precise)->min.x;
    Transform preciseTransform; preciseTransform.scale.x = 1.000001f;
    REQUIRE(scene.set_local_transform(precise,preciseTransform)); REQUIRE(physics.synchronize(scene,true));
    REQUIRE(physics.body_bounds(precise)); const auto afterWidth = physics.body_bounds(precise)->max.x - physics.body_bounds(precise)->min.x;
    CHECK(afterWidth > beforeWidth + 5e-7f);
}

TEST_CASE("moving noncollider support parent agrees in Jolt player collision and render in play") {
    Scene scene; PhysicsWorld physics; const auto parent = scene.create_object("support parent");
    const auto support = box(scene,{},false,parent), moving = box(scene,{0,2,0},true);
    auto collider = *scene.read<Collider>(support); collider.colliderHalfExtents = {4,.5f,4};
    REQUIRE(scene.set(support,collider)); REQUIRE(physics.synchronize(scene,false));
    for (int i=0;i<360;++i) tick(physics,scene);
    CHECK(scene.read<Transform>(moving)->position.y == doctest::Approx(1).epsilon(.03));
    Transform translated; translated.position.x = .2f; REQUIRE(scene.set_local_transform(parent,translated));
    tick(physics,scene);
    REQUIRE(physics.body_world(support)); check_matrix(*physics.body_world(support),scene.world_matrix(support));
    check_matrix(physics.render_world(scene,support,.5f),scene.world_matrix(support));
    const auto collision = collect_player_collision(scene,{}); REQUIRE(collision.walls.size() == 1);
    REQUIRE(physics.body_bounds(support)); check_vector(collision.walls[0].min,physics.body_bounds(support)->min);
    check_vector(collision.walls[0].max,physics.body_bounds(support)->max);
    CHECK(scene.read<Transform>(moving)->position.y == doctest::Approx(1).epsilon(.03));
}

TEST_CASE("offset rotated collider sync retains entity origin and interpolation") {
    Scene scene; PhysicsWorld physics; const auto moving = box(scene,{3,5,7},true);
    auto collider = *scene.read<Collider>(moving); collider.colliderCenter = {1,.3f,-.4f};
    REQUIRE(scene.set(moving,collider));
    Transform initial = *scene.read<Transform>(moving); initial.rotation = {.2f,.6f,-.3f}; initial.scale = {2,-1,3};
    REQUIRE(scene.set_local_transform(moving,initial));
    auto body = *scene.read<RigidBody>(moving); body.gravityFactor = 0; REQUIRE(scene.set(moving,body));
    const auto child = scene.create_object("visual",moving); Transform local; local.position = {2,0,0};
    REQUIRE(scene.set_local_transform(child,local)); REQUIRE(physics.synchronize(scene,false));
    REQUIRE(physics.body_bounds(moving));
    check_vector((physics.body_bounds(moving)->min+physics.body_bounds(moving)->max)*.5f,
        glm::vec3(to_matrix(initial)*glm::vec4(collider.colliderCenter,1)));
    const auto reset = resets(physics); CHECK(reset.contains(moving.persistentId)); CHECK(reset.contains(child.persistentId));
    check_matrix(physics.render_world(scene,moving,0),to_matrix(initial));
    REQUIRE(physics.queue_velocity(scene,moving,{12,0,0})); tick(physics,scene);
    CHECK(scene.read<Transform>(moving)->position.x == doctest::Approx(3.1f).epsilon(.0001));
    CHECK(scene.read<Transform>(moving)->position.y == doctest::Approx(5).epsilon(.0001));
    REQUIRE(physics.body_world(moving)); check_matrix(*physics.body_world(moving),scene.world_matrix(moving));
    CHECK(physics.render_world(scene,moving,.5f)[3].x == doctest::Approx(3.05f).epsilon(.0001));
    check_matrix(physics.render_world(scene,child,.5f),physics.render_world(scene,moving,.5f)*to_matrix(local));
    CHECK(resets(physics).empty());
    Transform teleported = *scene.read<Transform>(moving); teleported.position = {30,40,50};
    REQUIRE(scene.set_local_transform(moving,teleported));
    CHECK(scene.read<Transform>(moving)->position.x == doctest::Approx(3.1f).epsilon(.0001));
    CHECK(scene.queued_command_count() == 1); scene.apply_commands();
    CHECK(physics.velocity(moving).x == doctest::Approx(12));
    for (float alpha : {0.0f,.5f,1.0f}) check_matrix(physics.render_world(scene,moving,alpha),to_matrix(teleported));
    const auto teleportReset = resets(physics); CHECK(teleportReset.contains(child.persistentId));
    CHECK(teleportReset.contains(moving.persistentId)); CHECK_FALSE(physics.sleeping(moving));
    REQUIRE(physics.queue_velocity(scene,moving,{})); tick(physics,scene);
    check_matrix(scene.world_matrix(moving),to_matrix(teleported));
}

TEST_CASE("falling box rests at floor height then sleep and wake change subtree eligibility") {
    Scene scene; PhysicsWorld physics; const auto floor = box(scene), moving = box(scene,{0,4,0},true);
    auto collider = *scene.read<Collider>(floor); collider.collisionShape = CollisionShape::GroundPlane;
    collider.colliderHalfExtents = {20,.5f,20}; REQUIRE(scene.set(floor,collider));
    const auto child = box(scene,{},false,moving); REQUIRE(scene.set(child,RigidBody{}));
    REQUIRE(physics.synchronize(scene,false));
    REQUIRE(physics.body_bounds(floor)); CHECK(physics.body_bounds(floor)->max.y == doctest::Approx(0).epsilon(.001));
    CHECK_FALSE(contributes_to_static_caches(scene,moving,false)); CHECK_FALSE(contributes_to_static_caches(scene,child,false));
    CHECK(contributes_to_static_caches(scene,child,true));
    for (int i=0;i<900 && !physics.sleeping(moving);++i) tick(physics,scene);
    CHECK(physics.sleeping(moving));
    CHECK(scene.read<Transform>(moving)->position.y == doctest::Approx(.5f).epsilon(.03));
    CHECK(contributes_to_static_caches(scene,moving,false)); CHECK(contributes_to_static_caches(scene,child,false));
    check_matrix(physics.render_world(scene,moving,0),scene.world_matrix(moving));
    check_matrix(physics.render_world(scene,moving,1),scene.world_matrix(moving));
    REQUIRE(physics.queue_impulse(scene,moving,{0,5,0})); tick(physics,scene);
    CHECK_FALSE(physics.sleeping(moving)); CHECK_FALSE(contributes_to_static_caches(scene,child,false));
    CHECK(physics.velocity(moving).y > 0);
}

TEST_CASE("deferred in-step deletion remains readable and discards physics commands") {
    Scene scene; PhysicsWorld physics; const auto moving = box(scene,{0,2,0},true);
    REQUIRE(physics.synchronize(scene,false));
    REQUIRE(physics.queue_velocity(scene,moving,{1,0,0}));
    physics.step(scene,dt,[&] {
        REQUIRE(scene.destroy_object(moving)); CHECK(scene.pending(moving)); CHECK(scene.valid(moving));
        CHECK(physics.has_body(moving)); CHECK_FALSE(physics.queue_impulse(scene,moving,{0,2,0}));
        physics.remove(moving); CHECK(physics.has_body(moving));
    });
    CHECK(scene.queued_command_count() == 0); CHECK(scene.valid(moving));
    scene.drain_destruction([&](EntityRef ref) { CHECK(scene.read<Transform>(ref)); physics.remove(ref); });
    CHECK_FALSE(scene.valid(moving)); CHECK(physics.body_count() == 0);
    CHECK_FALSE(physics.queue_velocity(scene,moving,{})); CHECK_FALSE(physics.body_world(moving));
}

TEST_CASE("snapshot reset rebinds bodies restores deleted entities removes play creations") {
    Scene scene; PhysicsWorld physics; const auto moving = box(scene,{0,4,0},true), erased = box(scene,{4,0,0});
    const auto snapshot = scene.take_snapshot(); const auto high = scene.next_persistent_id();
    REQUIRE(physics.synchronize(scene,false)); resets(physics);
    REQUIRE(scene.destroy_object(erased)); scene.drain_destruction([&](EntityRef ref) { physics.remove(ref); });
    const auto spawned = box(scene,{8,3,0},true); tick(physics,scene); CHECK(physics.has_body(spawned));
    REQUIRE(scene.restore_snapshot(snapshot,[&](EntityRef ref) { physics.remove(ref); }));
    ecs_test::check_snapshot(snapshot,scene.take_snapshot());
    REQUIRE(physics.synchronize(scene,false));
    CHECK_FALSE(physics.has_body(moving)); CHECK_FALSE(physics.has_body(spawned)); CHECK_FALSE(scene.valid(spawned));
    CHECK(scene.next_persistent_id() > high); CHECK(scene.queued_command_count() == 0);
    const auto rebound = scene.get(moving.persistentId); REQUIRE(rebound); CHECK(physics.has_body(rebound));
    CHECK(physics.body_count() == 2); CHECK(physics.has_body(scene.get(erased.persistentId)));
    check_matrix(scene.world_matrix(rebound),to_matrix(snapshot.entities[0].transform));
    check_vector(physics.velocity(rebound),glm::vec3(0));
    for (float alpha : {0.0f,.5f,1.0f}) check_matrix(physics.render_world(scene,rebound,alpha),scene.world_matrix(rebound));
    CHECK(resets(physics).contains(rebound.persistentId));
    REQUIRE(scene.restore_snapshot(snapshot,[&](EntityRef ref) { physics.remove(ref); }));
    REQUIRE(physics.synchronize(scene,true)); CHECK_FALSE(scene.has<TransformDrivenExternally>(scene.get(moving.persistentId)));
    Scene replacement; const auto replacementBody = box(replacement);
    scene.replace(std::move(replacement),[&](EntityRef ref) { physics.remove(ref); });
    CHECK(physics.body_count() == 0); REQUIRE(physics.synchronize(scene,true)); CHECK(physics.body_count() == 1);
    CHECK(physics.has_body(scene.get(replacementBody.persistentId))); CHECK_FALSE(physics.has_body(rebound));
}

TEST_CASE("cache eligibility follows ancestor sleep state including missing state") {
    Scene scene; const auto root = box(scene,{},true), child = box(scene,{},false,root), ordinary = box(scene);
    REQUIRE(scene.set(child,RigidBody{})); CHECK_FALSE(contributes_to_static_caches(scene,child,false));
    CHECK(contributes_to_static_caches(scene,ordinary,false)); CHECK(contributes_to_static_caches(scene,child,true));
    REQUIRE(scene.set(root,PhysicsState{true})); CHECK(contributes_to_static_caches(scene,child,false));
    REQUIRE(scene.set(root,PhysicsState{false})); CHECK_FALSE(contributes_to_static_caches(scene,child,false));
    auto stale = root; ++stale.epoch; CHECK_FALSE(contributes_to_static_caches(scene,stale,false));
}

TEST_CASE("authored portal hosts cannot become dynamic or enter dynamic subtrees") {
    Scene scene; const auto hostParent = box(scene), host = box(scene,{},false,hostParent), rootHost = box(scene);
    const auto moving = box(scene,{},true); REQUIRE(scene.set(host,PortalPlaceable{}));
    scene.set_authored_portal_hosts({host.persistentId,rootHost.persistentId});
    CHECK_FALSE(scene.set_parent(host,moving)); CHECK_FALSE(scene.set_parent(hostParent,moving));
    auto body = *scene.read<RigidBody>(moving); CHECK_FALSE(scene.set(host,body));
    CHECK_FALSE(scene.set(rootHost,body)); CHECK_FALSE(scene.set(hostParent,body));
    CHECK(scene.read<Hierarchy>(host)->parent == hostParent.persistentId);
    AuthoredPortalPair pair; pair.first.placed = pair.second.placed = true;
    pair.first.hostWallObject = host.persistentId;
    std::string saved,error; REQUIRE_MESSAGE(serialize_scene_json(scene,{},std::span(&pair,1),saved,error),error);
    StagedScene reopened; REQUIRE_MESSAGE(parse_scene_json(saved,"scratch.json",{},reopened,error),error);
    CHECK_FALSE(reopened.scene.dynamic_ancestor(reopened.scene.get(host.persistentId)));
}

TEST_CASE("loader detaches representable parented dynamics preserves world pose and saves settings") {
    const auto parentTransform = R"({"position":[3,4,5],"rotation":[0,0.6,0],"scale":[2,2,2]})";
    const auto childTransform = R"({"position":[1,2,3],"rotation":[0.2,0,0],"scale":[1,2,3]})";
    StagedScene staged; std::string error;
    REQUIRE_MESSAGE(parse_scene_json(document(object_json(1,"null",parentTransform)+","+
        object_json(2,"1",childTransform,dynamic)+","+object_json(3,"2",identity,R"({"motion":"Static","mass":7,"friction":0.8})")),"scratch.json",{},staged,error),error);
    Transform parent; parent.position={3,4,5}; parent.rotation={0,.6f,0}; parent.scale={2,2,2};
    Transform child; child.position={1,2,3}; child.rotation={.2f,0,0}; child.scale={1,2,3};
    const auto ref = staged.scene.get(2); REQUIRE(ref);
    CHECK_FALSE(staged.scene.read<Hierarchy>(ref)->parent); CHECK(staged.scene.roots() == std::vector<PersistentID>{1,2});
    check_matrix(staged.scene.world_matrix(ref),to_matrix(parent)*to_matrix(child));
    CHECK(staged.scene.read<Hierarchy>(staged.scene.get(3))->parent == 2); CHECK_FALSE(staged.warnings.empty());
    std::string saved; REQUIRE_MESSAGE(serialize_scene_json(staged.scene,staged.settings,{},saved,error),error);
    StagedScene reopened; REQUIRE_MESSAGE(parse_scene_json(saved,"scratch.json",{},reopened,error),error);
    ecs_test::check_snapshot(staged.scene.take_snapshot(),reopened.scene.take_snapshot());
    const auto* body = reopened.scene.read<RigidBody>(reopened.scene.get(2)); REQUIRE(body);
    CHECK(body->motion == BodyMotion::Dynamic); CHECK(body->mass == doctest::Approx(2));
    CHECK(body->friction == doctest::Approx(.3)); CHECK(body->restitution == doctest::Approx(.2)); CHECK(body->gravityFactor == 1);
    const auto* dormant = reopened.scene.read<RigidBody>(reopened.scene.get(3)); REQUIRE(dormant);
    CHECK(dormant->motion == BodyMotion::Static); CHECK(dormant->mass == 7);
    CHECK(dormant->friction == doctest::Approx(.8));
}

TEST_CASE("duplicated dynamic subtree creates one fresh native body and preserves saved fields") {
    Scene scene; PhysicsWorld physics; const auto moving = box(scene,{3,4,5},true);
    const auto child = box(scene,{1,0,0},false,moving); REQUIRE(scene.set(child,RigidBody{}));
    auto authored = *scene.read<RigidBody>(moving); authored.mass = 3; authored.gravityFactor = -.5f;
    REQUIRE(scene.set(moving,authored)); const auto copy = scene.duplicate_subtree(moving); REQUIRE(copy);
    REQUIRE(scene.read<RigidBody>(copy)); CHECK(scene.read<RigidBody>(copy)->mass == 3);
    CHECK(scene.read<RigidBody>(copy)->gravityFactor == -.5f);
    REQUIRE(scene.read<Hierarchy>(copy)->children.size() == 1);
    const auto copiedChild = scene.get(scene.read<Hierarchy>(copy)->children.front());
    REQUIRE(copiedChild); CHECK(copiedChild.persistentId != child.persistentId);
    REQUIRE(physics.synchronize(scene,true)); CHECK(physics.body_count() == 2); CHECK_FALSE(physics.has_body(copiedChild));
    REQUIRE(scene.destroy_object(copy)); scene.drain_destruction([&](EntityRef ref) { physics.remove(ref); });
    CHECK(physics.body_count() == 1); CHECK_FALSE(scene.valid(copiedChild));
}

TEST_CASE("queued physics commands reject invalid refs and finite input violations") {
    Scene scene; PhysicsWorld physics; const auto moving = box(scene,{},true); REQUIRE(physics.synchronize(scene,false));
    const glm::vec3 invalid(std::numeric_limits<float>::infinity(),0,0);
    CHECK_FALSE(physics.queue_force(scene,moving,invalid)); CHECK_FALSE(physics.queue_impulse(scene,moving,invalid));
    CHECK_FALSE(physics.queue_velocity(scene,moving,invalid));
    Transform invalidTransform; invalidTransform.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(physics.queue_teleport(scene,moving,invalidTransform)); CHECK(scene.queued_command_count() == 0);
    auto stale = moving; ++stale.epoch; CHECK_FALSE(physics.queue_force(scene,stale,{1,0,0}));
    stale = moving; ++stale.persistentId; CHECK_FALSE(physics.queue_velocity(scene,stale,{1,0,0}));
    REQUIRE(physics.queue_force(scene,moving,{120,0,0})); tick(physics,scene); CHECK(physics.velocity(moving).x > .5f);
}

TEST_CASE("saved authored portal host in dynamic subtree is rejected before replacement") {
    const auto collider = R"({"enabled":true,"shape":0,"center":[0,0,0],"halfExtents":[0.5,0.5,0.5]})";
    auto input = document(object_json(1,"null",identity,dynamic,collider,true)+","+object_json(2,"1",identity,"",collider,true));
    const auto portal = R"({"position":[0,0,0],"normal":[0,0,1],"up":[0,1,0],"halfWidth":0.8,"halfHeight":1.2,"hostWallObject":2})";
    const auto at = input.find("\"authoredPortals\":[]"); REQUIRE(at != std::string::npos);
    input.replace(at,std::string("\"authoredPortals\":[]").size(),"\"authoredPortals\":["+std::string(portal)+","+portal+"]");
    StagedScene live; const auto old = live.scene.create_object("unchanged"); const auto epoch = live.scene.epoch();
    std::string error; CHECK_FALSE(parse_scene_json(input,"bad-host.json",{},live,error)); CHECK_FALSE(error.empty());
    CHECK(live.scene.valid(old)); CHECK(live.scene.epoch() == epoch);
}

TEST_CASE("invalid known rigid bodies and sheared dynamic loads fail transactionally") {
    StagedScene staged; std::string error;
    REQUIRE(parse_scene_json(document(object_json(5,"null",identity)),"good.json",{},staged,error));
    const auto ref = staged.scene.get(5); const auto epoch = staged.scene.epoch(); const auto snapshot = staged.scene.take_snapshot();
    const auto shearedParent = R"({"position":[0,0,0],"rotation":[0,0,0],"scale":[2,1,3]})";
    const auto rotatedChild = R"({"position":[0,0,0],"rotation":[0,0.6,0],"scale":[1,1,1]})";
    const std::vector<std::string> invalid = {
        document(object_json(1,"null",shearedParent)+","+object_json(2,"1",rotatedChild,dynamic)),
        document(object_json(1,"null",R"({"position":[0,0,0],"rotation":[0,0,0],"scale":[0,1,1]})","",
            R"({"enabled":false,"shape":0,"center":[0,0,0],"halfExtents":[0.5,0.5,0.5]})")+","+object_json(2,"1",identity)),
        document(object_json(1,"null",identity,R"({"motion":"Flying"})")),
        document(object_json(1,"null",identity,R"({"motion":"Dynamic","mass":0})")),
        document(object_json(1,"null",identity,R"({"motion":"Static","friction":-1})")),
        document(object_json(1,"null",identity,R"({"motion":"Static","restitution":2})")),
        document(object_json(1,"null",identity,R"({"motion":"Static","gravityFactor":1e100})")),
        document(object_json(1,"null",identity,dynamic,R"({"enabled":false,"shape":0,"center":[0,0,0],"halfExtents":[1,1,1]})")),
        document(object_json(1,"null",identity,dynamic,R"({"enabled":true,"shape":2,"center":[0,0,0],"halfExtents":[1,1,1]})"))
    };
    for (const auto& input : invalid) {
        INFO(input); CHECK_FALSE(parse_scene_json(input,"bad.json",{},staged,error)); CHECK_FALSE(error.empty());
        CHECK(staged.scene.epoch() == epoch); CHECK(staged.scene.valid(ref)); ecs_test::check_snapshot(snapshot,staged.scene.take_snapshot());
    }
    StagedScene disabled;
    REQUIRE_MESSAGE(parse_scene_json(document(object_json(1,"null",identity,R"({"motion":"Static","mass":0,"gravityFactor":-2})",
        R"({"enabled":false,"shape":0,"center":[0,0,0],"halfExtents":[0.5,0.5,0.5]})")),"static.json",{},disabled,error),error);
    PhysicsWorld physics; REQUIRE(physics.synchronize(disabled.scene,true)); CHECK(physics.body_count() == 0);
    const auto* body = disabled.scene.read<RigidBody>(disabled.scene.get(1)); REQUIRE(body); CHECK(body->gravityFactor == -2);
    StagedScene tiny;
    REQUIRE_MESSAGE(parse_scene_json(document(object_json(1,"null",R"({"position":[0,0,0],"rotation":[0,0,0],"scale":[0.0000001,1,1]})")),
        "tiny-static.json",{},tiny,error),error);
    PhysicsWorld tinyPhysics; REQUIRE(tinyPhysics.synchronize(tiny.scene,true)); CHECK(tinyPhysics.body_count() == 1);
    REQUIRE(tinyPhysics.body_world(tiny.scene.get(1)));
    CHECK(glm::length(glm::vec3((*tinyPhysics.body_world(tiny.scene.get(1)))[0])) == doctest::Approx(1e-7f).epsilon(.01));
}

TEST_CASE("physics demo scratch save reopens all saved components and replaces cleanly") {
    const std::filesystem::path source = MIRABILIS_TEST_SOURCE_DIR;
    StagedScene demo,reopened,sandbox; std::string error;
    REQUIRE_MESSAGE(parse_scene_json(ecs_test::read_file(source/"assets/scenes/physics_drop_demo.json"),
        "physics_drop_demo.json",{},demo,error),error);
    size_t dynamicCount = 0;
    for (auto ref : demo.scene.hierarchy_order()) {
        if (const auto* body = demo.scene.read<RigidBody>(ref); body && body->motion == BodyMotion::Dynamic) ++dynamicCount;
    }
    CHECK(dynamicCount == 1);
    const auto snapshot = demo.scene.take_snapshot();
    const auto path = std::filesystem::temp_directory_path() / ("mirabilis-physics-demo-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    REQUIRE_MESSAGE(save_scene_atomic(path,demo.scene,demo.settings,demo.authoredPortals,error),error);
    const auto saved = ecs_test::read_file(path); std::filesystem::remove(path);
    REQUIRE_MESSAGE(parse_scene_json(saved,"scratch-demo.json",{},reopened,error),error);
    ecs_test::check_snapshot(snapshot,reopened.scene.take_snapshot()); ecs_test::check_settings(demo.settings,reopened.settings);
    PhysicsWorld physics; REQUIRE(physics.synchronize(reopened.scene,true)); CHECK(physics.body_count() >= 3);
    REQUIRE_MESSAGE(parse_scene_json(ecs_test::read_file(source/"assets/scenes/sandbox.json"),"sandbox.json",{},sandbox,error),error);
    size_t staticCount = 0;
    for (auto ref : sandbox.scene.hierarchy_order()) if (sandbox.scene.read<Collider>(ref)->hasCollision) ++staticCount;
    reopened.scene.replace(std::move(sandbox.scene),[&](EntityRef ref) { physics.remove(ref); });
    CHECK(physics.body_count() == 0); REQUIRE(physics.synchronize(reopened.scene,true)); CHECK(physics.body_count() == staticCount);
}
