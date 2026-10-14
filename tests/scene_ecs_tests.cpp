#include "ecs_test_support.h"
#include "scene_queries.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <set>
using ecs_test::check_matrix;
using ecs_test::check_snapshot;
using ecs_test::ids;

TEST_CASE("entity identity validates epoch version and persistent ID") {
    mirabilis::Scene scene;
    const auto first = scene.create_object("first");
    REQUIRE(first); CHECK(first.persistentId == 0); CHECK(scene.get(0) == first);
    auto wrong = first; ++wrong.epoch; CHECK_FALSE(scene.valid(wrong));
    wrong = first; ++wrong.persistentId; CHECK_FALSE(scene.valid(wrong));
    REQUIRE(scene.destroy_object(first)); CHECK(scene.valid(first));
    REQUIRE(scene.drain_destruction() == 1); CHECK_FALSE(scene.valid(first));
    CHECK(scene.read<Name>(first) == nullptr);
    CHECK_FALSE(scene.set_local_transform(first, {}));
    const auto recycled = scene.create_object("recycled");
    REQUIRE(recycled); CHECK(recycled.persistentId > first.persistentId);
    CHECK(entt::to_entity(recycled.entity) == entt::to_entity(first.entity));
    CHECK(recycled.entity != first.entity); CHECK_FALSE(scene.valid(first));
    CHECK(scene.next_persistent_id() > recycled.persistentId);
    const auto high = scene.next_persistent_id();
    REQUIRE(scene.reserve_persistent_ids(1)); CHECK(scene.next_persistent_id() == high);
}

TEST_CASE("replacement invalidates same ID references and old commands") {
    mirabilis::Scene scene, replacement;
    REQUIRE(scene.reserve_persistent_ids(6));
    const auto old = scene.import_object("old five", 5);
    const auto next = replacement.import_object("new five", 5);
    REQUIRE(old); REQUIRE(next);
    int calls = 0;
    REQUIRE(scene.enqueue_command(old, [&](auto&,auto) { ++calls; }));
    const auto epoch = scene.epoch();
    scene.replace(std::move(replacement), [&](EntityRef dying) {
        CHECK(scene.valid(dying)); REQUIRE(scene.read<Name>(dying));
        CHECK(scene.epoch() == epoch);
    });
    CHECK(scene.epoch() > epoch); CHECK_FALSE(scene.valid(old));
    const auto fresh = scene.get(5); REQUIRE(scene.valid(fresh));
    CHECK(scene.read<Name>(fresh)->value == "new five");
    CHECK(scene.queued_command_count() == 0); scene.apply_commands(); CHECK(calls == 0);
}

TEST_CASE("hierarchy keeps ordered roots and children and refuses cycles") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("root");
    const auto a = scene.create_object("a",root), b = scene.create_object("b",root);
    const auto leaf = scene.create_object("leaf",a), other = scene.create_object("other");
    CHECK(ids(scene.hierarchy_order()) == std::vector<PersistentID>{root.persistentId,a.persistentId,leaf.persistentId,b.persistentId,other.persistentId});
    CHECK_FALSE(scene.set_parent(root,leaf)); CHECK_FALSE(scene.set_parent(a,a));
    REQUIRE(scene.set_parent(a,other));
    CHECK(scene.read<Hierarchy>(root)->children == std::vector<PersistentID>{b.persistentId});
    REQUIRE(scene.set_parent(b,std::nullopt)); REQUIRE(scene.set_sibling_index(b,0));
    CHECK(scene.roots() == std::vector<PersistentID>{b.persistentId,root.persistentId,other.persistentId});
    auto stale = other; ++stale.epoch; CHECK_FALSE(scene.set_parent(b,stale));
    CHECK(scene.read<Hierarchy>(b)->parent == std::nullopt);
}

TEST_CASE("world matrices and gizmo decomposition preserve existing best fit") {
    mirabilis::Scene scene;
    const auto parent = scene.create_object("parent"), child = scene.create_object("child",parent);
    Transform p{{3,4,-5},{0.2f,0.6f,-0.15f},{2,0.75f,1.5f}};
    Transform c{{1,0.5f,-2},{-0.25f,0.4f,0.3f},{0.8f,1.2f,0.6f}};
    REQUIRE(scene.set_local_transform(parent,p)); REQUIRE(scene.set_local_transform(child,c));
    check_matrix(scene.world_matrix(child),to_matrix(p)*to_matrix(c));
    const auto target = glm::rotate(glm::mat4(1),0.25f,glm::vec3(0,1,0))*scene.world_matrix(child);
    const auto expected = transform_from_matrix(glm::inverse(to_matrix(p))*target);
    REQUIRE(scene.set_world_transform(child,target));
    check_matrix(scene.local_matrix(child),to_matrix(expected));
    const auto before = scene.local_matrix(child);
    for (auto invalid : {std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = c; bad.position.x = invalid; CHECK_FALSE(scene.set_local_transform(child,bad));
        bad = c; bad.rotation.y = invalid; CHECK_FALSE(scene.set(child,bad));
        auto matrix = target; matrix[2][2] = invalid; CHECK_FALSE(scene.set_world_transform(child,matrix));
        check_matrix(scene.local_matrix(child),before);
    }
}

TEST_CASE("parent transform and hierarchy mutations dirty descendants once") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("no collider"), child = scene.create_object("box",root);
    const auto leaf = scene.create_object("leaf",child), separate = scene.create_object("separate");
    scene.consume_transform_dirty();
    Transform transform; transform.position.x = 3;
    REQUIRE(scene.set_local_transform(root,transform)); REQUIRE(scene.set_local_transform(root,transform));
    auto dirty = ids(scene.consume_transform_dirty()); std::sort(dirty.begin(),dirty.end());
    auto expected = std::vector<PersistentID>{root.persistentId,child.persistentId,leaf.persistentId};
    std::sort(expected.begin(),expected.end()); CHECK(dirty == expected); CHECK(scene.consume_transform_dirty().empty());
    REQUIRE(scene.set_parent(child,separate));
    dirty = ids(scene.consume_transform_dirty());
    CHECK(std::count(dirty.begin(),dirty.end(),child.persistentId) == 1);
    CHECK(std::count(dirty.begin(),dirty.end(),leaf.persistentId) == 1);
}

TEST_CASE("pending subtree reads survive while all public mutations fail") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("root"), child = scene.create_object("child",root);
    int commands = 0;
    REQUIRE(scene.enqueue_command(child,[&](auto&,auto) { ++commands; }));
    REQUIRE(scene.destroy_object(root)); REQUIRE(scene.pending(root)); REQUIRE(scene.pending(child));
    REQUIRE(scene.destroy_object(root)); const auto error = scene.last_error();
    CHECK(scene.destroy_object(child)); CHECK(scene.last_error() == error);
    CHECK(scene.valid(child)); REQUIRE(scene.read<Name>(child)); REQUIRE(scene.read<Transform>(child));
    CHECK_FALSE(scene.set(child,Name{"changed"})); CHECK(scene.edit<Name>(child) == nullptr);
    CHECK_FALSE(scene.set_local_transform(child,{})); CHECK_FALSE(scene.set_parent(child,std::nullopt));
    CHECK_FALSE(scene.set(child,PortalPlaceable{})); CHECK_FALSE(scene.remove<Collider>(child));
    CHECK_FALSE(scene.enqueue_command(child,[&](auto&,auto) { ++commands; }));
    scene.apply_commands(); CHECK(commands == 0);
    std::vector<PersistentID> callbacks;
    REQUIRE(scene.drain_destruction([&](EntityRef dying) {
        CHECK(scene.valid(dying)); CHECK(scene.pending(dying)); REQUIRE(scene.read<Name>(dying));
        CHECK_FALSE(scene.set(dying,Name{"forbidden"})); callbacks.push_back(dying.persistentId);
    }) == 2);
    CHECK(callbacks == std::vector<PersistentID>{child.persistentId,root.persistentId});
    CHECK_FALSE(scene.valid(child)); CHECK_FALSE(scene.valid(root)); CHECK(scene.drain_destruction() == 0);
}

TEST_CASE("destroy callbacks queue live mutations and join bounded destruction drains") {
    mirabilis::Scene scene;
    const auto dying = scene.create_object("dying"), next = scene.create_object("next"), live = scene.create_object("live");
    REQUIRE(scene.destroy_object(dying));
    REQUIRE(scene.drain_destruction([&](EntityRef ref) {
        if (ref.persistentId == dying.persistentId) {
            REQUIRE(scene.set(live,Name{"queued"})); CHECK(scene.read<Name>(live)->value == "live");
            Transform t; t.position.x = 9; REQUIRE(scene.set_local_transform(live,t));
            CHECK(scene.read<Transform>(live)->position.x == 0); REQUIRE(scene.destroy_object(next));
        }
    },1) == 1);
    CHECK(scene.pending(next)); CHECK(scene.valid(next)); CHECK_FALSE(scene.last_error().empty());
    CHECK(scene.read<Name>(live)->value == "live"); CHECK(scene.queued_command_count() == 2);
    CHECK(scene.drain_destruction() == 1); CHECK_FALSE(scene.valid(next));
    CHECK(scene.apply_commands() == 2); CHECK(scene.read<Name>(live)->value == "queued");
    CHECK(scene.read<Transform>(live)->position.x == 9);
    int calls = 0;
    REQUIRE(scene.enqueue_command(live,[&](auto& s,auto ref) {
        ++calls; REQUIRE(s.enqueue_command(ref,[&](auto&,auto) { ++calls; }));
    }));
    scene.apply_commands(); CHECK(calls == 1); scene.apply_commands(); CHECK(calls == 2);
}

TEST_CASE("destruction requests keep registry views readable until the drain") {
    mirabilis::Scene scene;
    std::vector<EntityRef> refs;
    for (int i = 0; i < 8; ++i) refs.push_back(scene.create_object("iterated"));
    size_t visited = 0;
    for (const auto entity : scene.registry().view<const Name>()) {
        const auto ref = scene.reference(entity);
        REQUIRE(scene.destroy_object(ref)); CHECK(scene.valid(ref)); REQUIRE(scene.read<Name>(ref));
        CHECK(scene.pending(ref)); ++visited;
    }
    CHECK(visited == refs.size()); CHECK(scene.drain_destruction() == refs.size());
    for (const auto ref : refs) CHECK_FALSE(scene.valid(ref));
}

TEST_CASE("runtime ownership protects descendant subtrees but external driving does not") {
    mirabilis::Scene scene;
    const auto authored = scene.create_object("authored"), runtime = scene.create_object("player",authored);
    REQUIRE(scene.set(runtime,RuntimeOnly{}));
    const auto runtimeChild = scene.create_object("model",runtime,true); REQUIRE(runtimeChild);
    CHECK(scene.runtime_only(runtimeChild)); CHECK_FALSE(scene.destroy_object(authored));
    CHECK_FALSE(scene.pending(authored)); CHECK_FALSE(scene.pending(runtime));
    CHECK_FALSE(scene.duplicate_subtree(runtime));
    const auto authoredCopy = scene.duplicate_subtree(authored); REQUIRE(authoredCopy);
    CHECK(scene.read<Hierarchy>(authoredCopy)->children.empty());
    CHECK_FALSE(scene.set(runtime,Name{"public"})); REQUIRE(scene.set(runtime,Name{"native"},true));
    const auto externallyDriven = scene.create_object("saved external");
    REQUIRE(scene.set(externallyDriven,TransformDrivenExternally{}));
    REQUIRE(scene.duplicate_subtree(externallyDriven)); REQUIRE(scene.destroy_object(externallyDriven));
    scene.drain_destruction(); CHECK_FALSE(scene.valid(externallyDriven));
    REQUIRE(scene.destroy_object(authored,true)); scene.drain_destruction(); CHECK_FALSE(scene.valid(runtimeChild));
}

TEST_CASE("required value components remain while optional tags can detach") {
    mirabilis::Scene scene; const auto ref = scene.create_object("live");
    REQUIRE(scene.has<Collider>(ref)); CHECK_FALSE(scene.remove<Collider>(ref)); CHECK(scene.has<Collider>(ref));
    REQUIRE(scene.set(ref,PortalPlaceable{})); CHECK(scene.has<PortalPlaceable>(ref));
    REQUIRE(scene.remove<PortalPlaceable>(ref)); CHECK_FALSE(scene.has<PortalPlaceable>(ref));
}

TEST_CASE("duplication allocates every ID and retains ordered components") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("root"), a = scene.create_object("a",root), b = scene.create_object("b",root);
    REQUIRE(scene.set_sibling_index(b,0));
    SceneMaterial material; material.enabled = true; material.normalTexturePath = "missing-normal.png";
    material.colorTint = {0.2f,0.4f,0.6f,0.8f}; REQUIRE(scene.set(a,material));
    REQUIRE(scene.set(root,UnknownComponents{{{"future",R"({"nested":[1,true,"keep"]})"}}}));
    const auto copy = scene.duplicate_subtree(root); REQUIRE(copy);
    CHECK(copy.persistentId > b.persistentId);
    const auto children = scene.read<Hierarchy>(copy)->children; REQUIRE(children.size() == 2);
    CHECK(scene.read<Name>(scene.get(children[0]))->value == "b");
    CHECK(scene.read<Name>(scene.get(children[1]))->value == "a");
    CHECK(scene.read<Hierarchy>(scene.get(children[0]))->parent == copy.persistentId);
    CHECK(scene.read<SceneMaterial>(scene.get(children[1]))->normalTexturePath == "missing-normal.png");
    CHECK(scene.read<UnknownComponents>(copy)->blocks == scene.read<UnknownComponents>(root)->blocks);
}

TEST_CASE("snapshot restore preserves authored state runtime survivors order and allocator") {
    mirabilis::Scene scene;
    const auto root = scene.create_object("root"), a = scene.create_object("a",root), b = scene.create_object("b",root);
    REQUIRE(scene.set_sibling_index(b,0));
    const auto runtime = scene.create_object("player"); REQUIRE(scene.set(runtime,RuntimeOnly{}));
    const auto snapshot = scene.take_snapshot(); const auto emission = ids(scene_emission_order(scene));
    const auto oldEpoch = scene.epoch(); const auto oldAHandle = a.entity, runtimeHandle = runtime.entity;
    Transform moved; moved.position = {10,20,30}; REQUIRE(scene.set_local_transform(a,moved));
    REQUIRE(scene.destroy_object(b)); scene.drain_destruction();
    const auto spawned = scene.create_object("play-created"), runtimeSpawn = scene.create_object("runtime-created");
    REQUIRE(scene.set(runtimeSpawn,RuntimeOnly{}));
    REQUIRE(scene.destroy_object(spawned));
    REQUIRE(scene.enqueue_command(a,[](auto& s,auto r) { s.set(r,Name{"obsolete"}); }));
    const auto high = scene.next_persistent_id();
    REQUIRE(scene.restore_snapshot(snapshot));
    CHECK(scene.epoch() > oldEpoch); CHECK_FALSE(scene.valid(a)); CHECK_FALSE(scene.valid(runtime));
    CHECK_FALSE(scene.get(spawned.persistentId)); CHECK_FALSE(scene.get(runtimeSpawn.persistentId));
    CHECK(scene.get(a.persistentId).entity == oldAHandle); CHECK(scene.get(runtime.persistentId).entity == runtimeHandle);
    CHECK(scene.next_persistent_id() == high); CHECK(scene.queued_command_count() == 0);
    CHECK(scene.drain_destruction() == 0);
    const auto dirty = ids(scene.consume_transform_dirty());
    CHECK(std::find(dirty.begin(),dirty.end(),a.persistentId) != dirty.end());
    CHECK(std::find(dirty.begin(),dirty.end(),b.persistentId) != dirty.end());
    check_snapshot(snapshot,scene.take_snapshot()); CHECK(ids(scene_emission_order(scene)) == emission);
    const auto next = scene.create_object("after restore"); CHECK(next.persistentId >= high);
}

TEST_CASE("spawn trigger and cache queries depend on authored scene state") {
    mirabilis::Scene scene;
    CHECK_FALSE(find_scene_spawn_point(scene));
    const auto spawn = scene.create_object("spawn"), trigger = scene.create_object("start");
    REQUIRE(scene.set(spawn,TimeTrial{TimeTrialRole::SpawnPoint}));
    Transform pose; pose.position = {3,2,1}; REQUIRE(scene.set_local_transform(spawn,pose));
    REQUIRE(find_scene_spawn_point(scene)); ecs_test::check_vector(*find_scene_spawn_point(scene),pose.position);
    REQUIRE(scene.set(trigger,TimeTrial{TimeTrialRole::StartTrigger}));
    CHECK(collect_time_trial_overlaps(scene,AABB{{-0.1f,-0.1f,-0.1f},{0.1f,0.1f,0.1f}}).start);
    CHECK(collect_time_trial_overlaps(scene,AABB{{0.5f,-0.1f,-0.1f},{0.6f,0.1f,0.1f}}).start);
    CHECK_FALSE(collect_time_trial_overlaps(scene,AABB{{10,10,10},{11,11,11}}).start);
    const auto drawable = scene.create_object("draw"); REQUIRE(scene.set(drawable,SceneMeshAsset{SceneAssetKind::UnitCube,{}}));
    CHECK(contributes_to_static_caches(scene,drawable,true)); CHECK(contributes_to_static_caches(scene,drawable,false));
    auto stale = drawable; ++stale.epoch; CHECK_FALSE(contributes_to_static_caches(scene,stale,false));
}
