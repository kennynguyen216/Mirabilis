#include "ecs_test_support.h"
#include "scene_queries.h"
#include <simdjson.h>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace ecs_test;
namespace {
const std::filesystem::path source = MIRABILIS_TEST_SOURCE_DIR;
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mirabilis-ecs-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path,error); }
};
std::string object_json(PersistentID id, std::string parent = "null", int sibling = 0,
    std::string name = "object", std::string extra = "") {
    return "{\"id\":"+std::to_string(id)+",\"parent\":"+parent+",\"siblingIndex\":"+
        std::to_string(sibling)+",\"components\":{\"name\":{\"value\":\""+name+R"("},
        "transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
        "visibility":{"visible":true,"layer":0},"mesh":{"asset":"empty","modelPath":""},
        "material":{"enabled":false,"baseColorTexture":"","normalTexture":"","metallicRoughnessTexture":"",
        "tint":[1,1,1,1],"uvScale":[1,1],"metallic":0,"roughness":0.8,"debugChecker":false,
        "emissionColor":[0,0,0],"emissionStrength":0,"transmission":0,"ior":1.5},
        "collider":{"enabled":false,"shape":0,"center":[0,0,0],"halfExtents":[0.5,0.5,0.5]},
        "timeTrialRole":{"role":"none"})"+extra+"}}";
}
std::string document_json(std::string objects, std::string counter = "100") {
    return "{\"version\":2,\"nextActor\":1,\"nextPersistentId\":"+counter+",\"objects\":["+objects+"],\"authoredPortals\":[]}";
}
glm::vec3 json_vec3(simdjson::dom::element value) {
    glm::vec3 result;
    size_t i = 0; for (auto item : value.get_array()) result[i++] = static_cast<float>(double(item));
    REQUIRE(i == 3); return result;
}
void check_collision(const PlayerCollisionSet& actual, simdjson::dom::element fixture) {
    const simdjson::dom::array walls = fixture["walls"];
    REQUIRE(actual.walls.size() == walls.size());
    size_t i = 0;
    for (auto wall : walls) {
        check_vector(actual.walls[i].min,json_vec3(wall["min"]));
        check_vector(actual.walls[i++].max,json_vec3(wall["max"]));
    }
    const simdjson::dom::array grounds = fixture["grounds"];
    REQUIRE(actual.groundPlanes.size() == grounds.size()); i = 0;
    for (auto ground : grounds) {
        const simdjson::dom::array center = ground["center"], half = ground["halfExtents"];
        size_t axis = 0; for (auto value : center) CHECK(near_float(actual.groundPlanes[i].center[axis++],double(value)));
        axis = 0; for (auto value : half) CHECK(near_float(actual.groundPlanes[i].halfExtents[axis++],double(value)));
        CHECK(near_float(actual.groundPlanes[i++].height,double(ground["height"])));
    }
    const simdjson::dom::array ramps = fixture["ramps"];
    REQUIRE(actual.surfRamps.size() == ramps.size()); i = 0;
    for (auto ramp : ramps) {
        const simdjson::dom::array matrix = ramp["worldToLocal"];
        size_t index = 0;
        for (auto value : matrix) {
            CHECK(near_float(actual.surfRamps[i].worldToLocal[index/4][index%4],double(value))); ++index;
        }
        REQUIRE(index == 16);
        check_vector(actual.surfRamps[i].lowLeft,json_vec3(ramp["lowLeft"]));
        check_vector(actual.surfRamps[i].lowRight,json_vec3(ramp["lowRight"]));
        check_vector(actual.surfRamps[i++].highLeft,json_vec3(ramp["highLeft"]));
    }
}
}

TEST_CASE("all shipped scenes round trip every saved component and collision golden") {
    Scratch scratch;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(source/"assets/scenes")) {
        if (entry.path().extension() != ".json") continue;
        INFO(entry.path().filename().string()); ++count;
        StagedScene first, reopened; std::string error;
        REQUIRE_MESSAGE(parse_scene_json(read_file(entry.path()),entry.path().filename().string(),{},first,error),error);
        const auto authored = first.scene.take_snapshot();
        const auto emission = ids(scene_emission_order(first.scene));
        const auto path = scratch.path/entry.path().filename();
        REQUIRE_MESSAGE(save_scene_atomic(path,first.scene,first.settings,first.authoredPortals,error),error);
        const auto saved = read_file(path);
        simdjson::dom::parser savedParser; simdjson::dom::element savedJson = savedParser.parse(saved);
        CHECK(uint64_t(savedJson["version"]) == 2);
        REQUIRE_MESSAGE(parse_scene_json(saved,path.filename().string(),{},reopened,error),error);
        check_snapshot(authored,reopened.scene.take_snapshot()); check_settings(first.settings,reopened.settings);
        CHECK(ids(scene_emission_order(reopened.scene)) == emission);
        REQUIRE(first.authoredPortals.size() == reopened.authoredPortals.size());
        std::vector<Portal> portals;
        for (size_t p = 0; p < first.authoredPortals.size(); ++p) {
            check_portal(first.authoredPortals[p].first,reopened.authoredPortals[p].first);
            check_portal(first.authoredPortals[p].second,reopened.authoredPortals[p].second);
            portals.push_back(first.authoredPortals[p].first); portals.push_back(first.authoredPortals[p].second);
        }
        const auto fixturePath = source/"tests/fixtures/ecs_collision"/entry.path().filename();
        const auto fixtureText = read_file(fixturePath);
        simdjson::dom::parser fixtureParser; simdjson::dom::element fixture = fixtureParser.parse(fixtureText);
        check_collision(collect_player_collision(first.scene,portals),fixture);
        check_collision(collect_player_collision(reopened.scene,portals),fixture);
    }
    CHECK(count == 23);
}

TEST_CASE("v2 component schema sibling indices and ID zero are explicit") {
    StagedScene loaded; std::string error;
    const auto input = document_json(object_json(0)+","+object_json(5,"0",2,"last")+","+
        object_json(7,"0",0,"first")+","+object_json(8,"0",1,"equal-a")+","+object_json(9,"0",1,"equal-b"));
    REQUIRE_MESSAGE(parse_scene_json(input,"scratch.json",{},loaded,error),error);
    CHECK(loaded.scene.read<Hierarchy>(loaded.scene.get(0))->children == std::vector<PersistentID>{7,8,9,5});
    REQUIRE(loaded.scene.set_parent(loaded.scene.get(7),std::nullopt));
    REQUIRE(loaded.scene.set_parent(loaded.scene.get(5),loaded.scene.get(7)));
    const auto order = ids(scene_emission_order(loaded.scene));
    std::string output;
    REQUIRE(serialize_scene_json(loaded.scene,loaded.settings,loaded.authoredPortals,output,error));
    simdjson::dom::parser parser; simdjson::dom::element json = parser.parse(output);
    CHECK(uint64_t(json["version"]) == 2); CHECK(uint64_t(json["nextPersistentId"]) == 100);
    std::vector<PersistentID> written;
    for (auto object : json["objects"].get_array()) {
        written.push_back(uint64_t(object["id"]));
        CHECK(object["siblingIndex"].get_uint64().error() == simdjson::SUCCESS);
        const simdjson::dom::object components = object["components"];
        for (const char* key : {"name","transform","visibility","mesh","material","collider","timeTrialRole"})
            CHECK(components.at_key(key).error() == simdjson::SUCCESS);
        if (uint64_t(object["id"]) == 0) CHECK(object["parent"].is_null());
    }
    CHECK(written == order);
    StagedScene again; REQUIRE(parse_scene_json(output,"scratch.json",{},again,error));
    CHECK(ids(scene_emission_order(again.scene)) == order); check_snapshot(loaded.scene.take_snapshot(),again.scene.take_snapshot());
}

TEST_CASE("duplicate IDs reserve later imports and references bind first occurrence") {
    for (const auto filename : {"duplicate_v1.json","duplicate_v2.json"}) {
        INFO(filename); StagedScene loaded; std::string error;
        REQUIRE_MESSAGE(parse_scene_json(read_file(source/"tests/fixtures/ecs_input"/filename),filename,{},loaded,error),error);
        REQUIRE(loaded.scene.get(5)); REQUIRE(loaded.scene.get(6));
        CHECK(loaded.scene.read<Name>(loaded.scene.get(5))->value == "first");
        CHECK(loaded.scene.read<Name>(loaded.scene.get(6))->value == "later-valid");
        CHECK(loaded.scene.read<Hierarchy>(loaded.scene.get(6))->parent == 5);
        std::set<PersistentID> allocated;
        for (auto ref : loaded.scene.creation_order()) allocated.insert(ref.persistentId);
        REQUIRE(allocated.size() == 3); CHECK(*allocated.rbegin() >= 7); CHECK_FALSE(loaded.warnings.empty());
        CHECK(loaded.scene.next_persistent_id() > *allocated.rbegin());
    }
}

TEST_CASE("64-bit IDs and portal host zero survive reference round trips") {
    StagedScene wide; std::string error;
    constexpr PersistentID highId = (uint64_t{1} << 32) + 17;
    REQUIRE(parse_scene_json(document_json(object_json(highId)+","+object_json(0,std::to_string(highId)),
        std::to_string(highId+1)),"wide.json",{},wide,error));
    REQUIRE(wide.scene.get(highId)); CHECK(wide.scene.read<Hierarchy>(wide.scene.get(0))->parent == highId);
    mirabilis::Scene scene; const auto host = scene.create_object("host zero"); REQUIRE(host.persistentId == 0);
    Collider collider; collider.hasCollision = true; collider.colliderHalfExtents = {4,4,0.5f};
    REQUIRE(scene.set(host,collider)); REQUIRE(scene.set(host,PortalPlaceable{}));
    AuthoredPortalPair pair; pair.first.placed = pair.second.placed = true;
    pair.first.hostWallObject = 0; pair.first.position = {0,0,0.5f}; pair.second.position = {10,0,0};
    std::string output; REQUIRE(serialize_scene_json(scene,{},std::span(&pair,1),output,error));
    simdjson::dom::parser parser; simdjson::dom::element json = parser.parse(output);
    const simdjson::dom::array portals = json["authoredPortals"]; REQUIRE(portals.size() == 2);
    CHECK(uint64_t(portals.at(0).value()["hostWallObject"]) == 0);
    CHECK(portals.at(1).value()["hostWallObject"].is_null());
    StagedScene loaded; REQUIRE(parse_scene_json(output,"zero-host.json",{},loaded,error));
    REQUIRE(loaded.authoredPortals.size() == 1); CHECK(loaded.authoredPortals[0].first.hostWallObject == 0);
    CHECK(loaded.authoredPortals[0].second.hostWallObject == std::nullopt);
}

TEST_CASE("malformed documents and exhaustion never replace staging state") {
    StagedScene live; std::string error;
    REQUIRE(parse_scene_json(document_json(object_json(5)),"good.json",{},live,error));
    const auto old = live.scene.get(5); const auto epoch = live.scene.epoch();
    const auto snapshot = live.scene.take_snapshot();
    const std::vector<std::string> invalid = {
        "{", R"({"version":999,"objects":[]})",
        document_json(object_json(1,"999")), document_json(object_json(1,"1")),
        document_json(object_json(1,"2")+","+object_json(2,"1")), document_json(object_json(1,"null",-1)),
        document_json(object_json(UINT64_MAX)), document_json(object_json(1),"18446744073709551616"),
        document_json(object_json(1),"18446744073709551615"),
        document_json(object_json(1)+","+object_json(1),"18446744073709551614"),
        R"({"version":2,"nextActor":1,"nextPersistentId":100,"objects":[{"id":-1,"parent":null,"siblingIndex":0,"components":{}}]})"
    };
    for (const auto& input : invalid) {
        INFO(input); CHECK_FALSE(parse_scene_json(input,"bad.json",{},live,error)); CHECK_FALSE(error.empty());
        CHECK(live.scene.epoch() == epoch); CHECK(live.scene.valid(old)); check_snapshot(snapshot,live.scene.take_snapshot());
    }
    for (const auto name : {"invalid_transform.json","invalid_collider.json","invalid_portal_host.json"}) {
        INFO(name); CHECK_FALSE(parse_scene_json(read_file(source/"tests/fixtures/ecs_input"/name),name,{},live,error));
        CHECK(live.scene.epoch() == epoch); CHECK(live.scene.valid(old)); check_snapshot(snapshot,live.scene.take_snapshot());
    }
}

TEST_CASE("unknown future blocks and unavailable asset identities survive") {
    StagedScene loaded; std::string error;
    const auto input = document_json(object_json(0,"null",0,"future",R"(,"futurePhysics":{"version":3,"nested":[true,null,"\u00e9",{"mass":2.5}]})"));
    REQUIRE(parse_scene_json(input,"unknown.json",{},loaded,error));
    auto ref = loaded.scene.get(0); REQUIRE(loaded.scene.has<UnknownComponents>(ref));
    REQUIRE(loaded.scene.set(ref,SceneMeshAsset{SceneAssetKind::ImportedGLTF,"../../missing/model.gltf"}));
    loaded.settings.skybox = "qwantani_noon";
    std::string output; REQUIRE(serialize_scene_json(loaded.scene,loaded.settings,{},output,error));
    StagedScene again; REQUIRE(parse_scene_json(output,"unknown.json",{},again,error));
    CHECK(again.scene.read<SceneMeshAsset>(again.scene.get(0))->modelPath == "../../missing/model.gltf");
    CHECK(again.settings.skybox == "qwantani_noon");
    const auto& blocks = again.scene.read<UnknownComponents>(again.scene.get(0))->blocks;
    REQUIRE(blocks.contains("futurePhysics"));
    simdjson::dom::parser parser; simdjson::dom::element unknown = parser.parse(blocks.at("futurePhysics"));
    CHECK(uint64_t(unknown["version"]) == 3); CHECK(unknown["nested"].get_array().value().size() == 4);
}

TEST_CASE("external driving is saved while runtime-only descendants are omitted") {
    mirabilis::Scene scene;
    const auto external = scene.create_object("external"), runtime = scene.create_object("runtime");
    REQUIRE(scene.set(external,TransformDrivenExternally{})); REQUIRE(scene.set(runtime,RuntimeOnly{}));
    const auto descendant = scene.create_object("runtime descendant",runtime,true); REQUIRE(descendant);
    std::string output,error; REQUIRE(serialize_scene_json(scene,{}, {},output,error));
    simdjson::dom::parser parser; simdjson::dom::element json = parser.parse(output);
    const simdjson::dom::array objects = json["objects"]; REQUIRE(objects.size() == 1);
    const auto first = objects.at(0).value(); CHECK(uint64_t(first["id"]) == external.persistentId);
    CHECK(first["components"]["transformDrivenExternally"].is_object());
    CHECK(uint64_t(json["nextPersistentId"]) == scene.next_persistent_id());
    const auto copy = scene.duplicate_subtree(external); REQUIRE(copy); REQUIRE(scene.destroy_object(external));
    CHECK(scene.drain_destruction() == 1);
}

TEST_CASE("saving snapshots writes authored components with current high-water") {
    mirabilis::Scene live;
    const auto root = live.create_object("authored"), child = live.create_object("restore me",root);
    const SceneDocumentSnapshot snapshot{live.take_snapshot(),SceneSettings{}, {}};
    Transform moved; moved.position.x = 42; REQUIRE(live.set_local_transform(root,moved));
    REQUIRE(live.destroy_object(child)); live.drain_destruction();
    const auto created = live.create_object("play-created"); const auto high = live.next_persistent_id();
    mirabilis::Scene authored; REQUIRE(authored.restore_snapshot(snapshot.entities));
    REQUIRE(authored.reserve_persistent_ids(high));
    std::string output,error; REQUIRE(serialize_scene_json(authored,snapshot.settings,snapshot.authoredPortals,output,error));
    StagedScene reopened; REQUIRE(parse_scene_json(output,"snapshot.json",{},reopened,error));
    CHECK_FALSE(reopened.scene.get(created.persistentId)); CHECK(reopened.scene.get(child.persistentId));
    CHECK(reopened.scene.read<Transform>(reopened.scene.get(root.persistentId))->position.x == 0);
    CHECK(reopened.scene.next_persistent_id() == high);
}

TEST_CASE("failed validation and atomic replacement preserve destination bytes") {
    Scratch scratch; const auto destination = scratch.path/"scene.json";
    { std::ofstream file(destination); file << "previous document"; }
    mirabilis::Scene scene; const auto entity = scene.create_object("saved"); REQUIRE(entity);
    SceneSettings invalid; invalid.environmentIntensity = std::numeric_limits<float>::quiet_NaN();
    std::string error; CHECK_FALSE(save_scene_atomic(destination,scene,invalid,{},error));
    CHECK(read_file(destination) == "previous document");
    const auto parentAsFile = scratch.path/"not-a-directory";
    { std::ofstream file(parentAsFile); file << "untouched"; }
    CHECK_FALSE(save_scene_atomic(parentAsFile/"scene.json",scene,{}, {},error));
    CHECK(read_file(parentAsFile) == "untouched");
#ifdef _WIN32
    REQUIRE(SetFileAttributesW(destination.c_str(),FILE_ATTRIBUTE_READONLY));
    const auto saved = save_scene_atomic(destination,scene,{}, {},error);
    REQUIRE(SetFileAttributesW(destination.c_str(),FILE_ATTRIBUTE_NORMAL));
    CHECK_FALSE(saved); CHECK(read_file(destination) == "previous document");
#endif
    REQUIRE_MESSAGE(save_scene_atomic(destination,scene,{}, {},error),error);
    StagedScene reopened; REQUIRE(parse_scene_json(read_file(destination),"scene.json",{},reopened,error));
    CHECK(reopened.scene.read<Name>(reopened.scene.get(entity.persistentId))->value == "saved");
}
