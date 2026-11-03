#include "draw_instance.h"
#include "mesh_clusters.h"
#include "scene_serialization.h"
#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <set>
#include "ray_scene.h"
#include "path_trace_scene.h"
#include "surface_cache_index.h"
#include <numeric>

TEST_CASE("occupied card tiles conservatively retain texel footprints and blend-depth margins") {
    constexpr uint32_t width=27,height=23;
    const glm::uvec4 rect(3,2,19,17);
    std::vector<uint16_t> depth(width*height,glm::packHalf1x16(1.f));
    for(uint32_t y=0;y<rect.w;++y) for(uint32_t x=0;x<rect.z;++x)
        if ((x+y)%7==0) depth[(rect.y+y)*width+rect.x+x]=glm::packHalf1x16(.15f+.02f*((x+3*y)%29));
    for(int transform=0;transform<6;++transform) {
        glm::mat4 matrix=glm::translate(glm::mat4(1),glm::vec3(1200,-800,30))*
            glm::rotate(glm::mat4(1),.27f*transform,glm::normalize(glm::vec3(1,2,3)))*
            glm::scale(glm::mat4(1),glm::vec3(transform%2?-7.f:7.f,.3f,5.f));
        const float worldDepth=glm::length(glm::vec3(matrix[2]));
        std::vector<std::pair<glm::vec3,glm::vec3>> boxes;
        REQUIRE(surface_card_occupied_bounds(depth,width,rect,matrix,worldDepth,.05f,.2f,
            [&](glm::vec3 a,glm::vec3 b){boxes.emplace_back(a,b);}));
        REQUIRE_FALSE(boxes.empty());
        for(uint32_t y=0;y<rect.w;++y) for(uint32_t x=0;x<rect.z;++x) {
            const float stored=glm::unpackHalf1x16(depth[(rect.y+y)*width+rect.x+x]);
            if(stored>=1)continue;
            for(float behind:{-.05f,0.f,.15f,.2f}) for(float corner:{0.f,.5f,1.f}) {
                const auto point=glm::vec3(matrix*glm::vec4((x+corner)/rect.z,(y+corner)/rect.w,stored-behind/worldDepth,1));
                const bool retained=std::any_of(boxes.begin(),boxes.end(),[&](const auto& box){
                    return glm::all(glm::greaterThanEqual(point,box.first))&&glm::all(glm::lessThanEqual(point,box.second));});
                CHECK(retained);
            }
        }
    }
    size_t emitted=0;auto count=[&](glm::vec3,glm::vec3){++emitted;};
    std::fill(depth.begin(),depth.end(),glm::packHalf1x16(1.f));
    CHECK(surface_card_occupied_bounds(depth,width,rect,glm::mat4(1),1,.05f,.2f,count));CHECK(emitted==0);
    depth[rect.y*width+rect.x]=glm::packHalf1x16(std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(surface_card_occupied_bounds(depth,width,rect,glm::mat4(1),1,.05f,.2f,count));
    CHECK_FALSE(surface_card_occupied_bounds(depth,0,rect,glm::mat4(1),1,.05f,.2f,count));
    CHECK_FALSE(surface_card_occupied_bounds(depth,width,glm::uvec4(25,2,19,17),glm::mat4(1),1,.05f,.2f,count));
    CHECK_FALSE(surface_card_occupied_bounds(depth,width,rect,glm::mat4(1),0,.05f,.2f,count));
}

TEST_CASE("cache facing groups preserve every eligible card across transforms and dense cells") {
    std::vector<glm::vec3> directions;
    for (uint32_t i = 0; i < 300; ++i) {
        glm::vec3 direction(0); direction[i % 3] = i % 2 ? -1.f : 1.f;
        if (i % 7 == 0) direction = glm::normalize(glm::vec3(glm::rotate(glm::mat4(1), .37f,
            glm::normalize(glm::vec3(1,2,3))) * glm::vec4(direction,0)));
        directions.push_back(direction);
    }
    std::vector<uint32_t> candidates(directions.size());
    std::iota(candidates.begin(), candidates.end(), 0u);
    std::reverse(candidates.begin(), candidates.end());
    std::vector<glm::uvec2> grouped(5,glm::uvec2(0)), linear(5,glm::uvec2(0));
    append_surface_cache_candidates(directions,candidates,grouped,true);
    append_surface_cache_candidates(directions,candidates,linear,false);
    REQUIRE(grouped.size()==directions.size()+5);
    for (size_t i=5;i<grouped.size();++i) {
        CHECK(grouped[i].x==linear[i].x);
        CHECK(grouped[i].y>i);
        CHECK(grouped[i].y<=grouped.size());
    }
    uint32_t random=216;
    for (uint32_t query=0;query<1000;++query) {
        glm::vec3 normal;
        for (int axis=0;axis<3;++axis) { random=random*1664525u+1013904223u; normal[axis]=float(random%2001)-1000.f; }
        normal=glm::normalize(normal);
        std::vector<uint32_t> expected,actual;
        for (size_t i=5;i<linear.size();++i)
            if (glm::dot(normal,directions[linear[i].x])>=.2f) expected.push_back(linear[i].x);
        for (size_t i=5;i<grouped.size();) {
            const auto entry=grouped[i];
            if (glm::dot(normal,directions[entry.x])<.2f) i=entry.y;
            else {actual.push_back(entry.x);++i;}
        }
        CHECK(actual==expected);
    }
    std::vector<uint32_t> empty;
    const auto previous=grouped.size();
    append_surface_cache_candidates(directions,empty,grouped,true);
    CHECK(grouped.size()==previous);
}

TEST_CASE("compact triangle references preserve full BVH order including equal centroids") {
    for (uint32_t count:{0u,1u,4u,5u,17u,129u,257u}) for(bool ties:{false,true}) {
        std::vector<TraceTriangle> full;std::vector<TraceTriangleRef> refs;
        uint32_t random=216;
        for(uint32_t i=0;i<count;++i) {
            random=random*1664525u+1013904223u;
            glm::vec3 p(float(random%31),float((random>>8)%17),float((random>>16)%23));
            if(ties)p=glm::vec3(2,3,4);
            TraceTriangle t;t.p0=glm::vec4(p+glm::vec3(-1,-1,0),1);t.p1=glm::vec4(p+glm::vec3(1,-1,0),1);t.p2=glm::vec4(p+glm::vec3(0,2,0),1);t.meta.y=i;
            full.push_back(t);refs.push_back({glm::vec3(t.p0+t.p1+t.p2)/3.f,0,i});
        }
        build_trace_bvh(full);order_trace_triangle_refs(refs);
        for(size_t i=0;i<full.size();++i)CHECK(refs[i].first==full[i].meta.y);
    }
}

TEST_CASE("ray mesh groups retain material geometry indices and reuse geometry across transforms") {
    auto moved=glm::translate(glm::mat4(1),glm::vec3(2,0,0));
    std::vector<RayGeometryDraw> draws{
        {1,9,glm::mat4(1),{0,6}}, {2,9,glm::mat4(1),{0,3}},
        {1,9,glm::mat4(1),{6,3}}, {1,9,moved,{0,6}}, {1,9,moved,{6,3}},
        {1,10,glm::mat4(1),{0,6}}};
    const auto groups=group_ray_geometry(draws);
    REQUIRE(groups.size()==4);
    CHECK(groups[0].draws==std::vector<size_t>{0,2});
    CHECK(groups[2].draws==std::vector<size_t>{3,4});
    CHECK(groups[0].ranges==groups[2].ranges);
    CHECK(groups[0].ranges[1]==glm::uvec2(6,3));
    CHECK(groups[3].indices==10);
    CHECK(group_ray_geometry({}).empty());
}

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
    settings.sceneReflections=true; settings.lumenGI=true; settings.reflectionRoughness=0.35f;
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
    CHECK(loaded.settings.sceneReflections); CHECK(loaded.settings.lumenGI);
    CHECK(loaded.settings.reflectionRoughness==doctest::Approx(0.35f));
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

#include "surface_update_schedule.h"
TEST_CASE("surface relighting sweeps uneven cards within a strict strip budget") {
    std::vector<glm::uvec4> rects{{0,0,7,9},{7,0,3,5},{10,0,11,3}};
    SurfaceUpdateSchedule schedule;
    schedule.restart(rects,false);
    std::vector<std::vector<int>> visits{{std::vector<int>(9)},{std::vector<int>(5)},{std::vector<int>(3)}};
    uint64_t total=0;
    for(int frame=0;schedule.remaining && frame<100;++frame) {
        uint64_t used=0;
        for(auto strip:schedule.take(rects,22)) {
            CHECK(strip.row+strip.rows<=rects[strip.card].w);
            for(uint32_t y=strip.row;y<strip.row+strip.rows;++y) ++visits[strip.card][y];
            used+=uint64_t(strip.rows)*rects[strip.card].z;
        }
        CHECK(used<=22); CHECK(used>0); total+=used;
    }
    CHECK(schedule.remaining==0); CHECK(total==111);
    for(const auto& card:visits) for(auto row:card) CHECK(row==1);
}
TEST_CASE("continuous light edits preserve relight cursor and reach later cards") {
    std::vector<glm::uvec4> rects(5,glm::uvec4(0,0,8,8));
    SurfaceUpdateSchedule schedule;
    schedule.restart(rects,false);
    std::set<uint32_t> visited;
    for(int frame=0;frame<40;++frame) {
        schedule.restart(rects,true);
        for(auto strip:schedule.take(rects,16)) visited.insert(strip.card);
    }
    CHECK(visited.size()==5);
    schedule.restart(rects,false); CHECK(schedule.card==0); CHECK(schedule.row==0);
    CHECK(schedule.take(rects,0).empty());
}
TEST_CASE("partial card dispatch preserves full-card world coordinates") {
    glm::mat4 card=glm::translate(glm::mat4(1),glm::vec3(3,4,5))*glm::scale(glm::mat4(1),glm::vec3(2,7,3));
    glm::mat4 strip=card; strip[3]+=strip[1]*(13.f/47); strip[1]*=9.f/47;
    for(int row=0;row<9;++row) {
        glm::vec4 a=card*glm::vec4(.3f,(13+row+.5f)/47,.4f,1);
        glm::vec4 b=strip*glm::vec4(.3f,(row+.5f)/9,.4f,1);
        CHECK(glm::length(a-b)<1e-5f);
    }
}
