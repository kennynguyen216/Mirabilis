#include "ecs_test_support.h"
#include "script_host.h"
#include "scene_queries.h"
#include <simdjson.h>
#include <limits>
#include <thread>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using mirabilis::ScriptHost;
using mirabilis::ScriptInput;
using mirabilis::PhysicsWorld;
namespace abi = mirabilis::scripting;
namespace {
std::string canonical(std::string_view text) {
    simdjson::dom::parser parser;
    return simdjson::minify(parser.parse(text.data(),text.size()));
}
void check_fields(const std::map<std::string,ScriptField>& a,const std::map<std::string,ScriptField>& b) {
    REQUIRE(a.size()==b.size());
    for(const auto& [name,field]:a) {
        REQUIRE(b.contains(name)); CHECK(field.type==b.at(name).type);
        CHECK(canonical(field.valueJson)==canonical(b.at(name).valueJson));
    }
}
Script script(std::string type="LifecycleProbe",std::map<std::string,ScriptField> fields={}) {
    return {"Mirabilis.TestScripts","Mirabilis.TestScripts."+type,true,std::move(fields)};
}
bool logged(const ScriptHost& host,std::string_view text) {
    return std::any_of(host.diagnostics().begin(),host.diagnostics().end(),[&](const auto& line){return line.find(text)!=std::string::npos;});
}
size_t occurrences(const ScriptHost& host,std::string_view text) {
    return std::count_if(host.diagnostics().begin(),host.diagnostics().end(),[&](const auto& line){return line.find(text)!=std::string::npos;});
}
struct Runtime {
    Scene scene;
    PhysicsWorld physics;
    ScriptHost host;
    Runtime() {
        host.bind(scene,physics);
        REQUIRE_MESSAGE(host.initialize(MIRABILIS_TEST_MANAGED_DIR),host.error());
        REQUIRE(host.available());
    }
    EntityRef add(const Script& value=script(),std::string name="probe") {
        auto ref=scene.create_object(std::move(name)); REQUIRE(scene.set(ref,value)); return ref;
    }
    void tick() {
        host.fixed_update(1.f/120);
        scene.apply_commands(); REQUIRE(physics.synchronize(scene,false));
        physics.step(scene,1.f/120);
        scene.drain_destruction([&](EntityRef r){physics.remove(r);});
    }
    ~Runtime() { host.stop(); scene.clear_queues(); }
};
int field_int(const Scene& scene,EntityRef ref,const char* name) {
    const auto* component=scene.read<Script>(ref); REQUIRE(component); REQUIRE(component->fields.contains(name));
    return std::stoi(component->fields.at(name).valueJson);
}
std::string marker(EntityRef ref,std::string_view phase) {
    return "probe:"+std::to_string(ref.persistentId)+":"+std::string(phase);
}
}

TEST_CASE("script ABI has fixed widths offsets and complete tables") {
    static_assert(sizeof(abi::Entity)==24 && offsetof(abi::Entity,persistentId)==16);
    static_assert(sizeof(abi::Vec3)==12 && offsetof(abi::Trs,rotation)==12 && offsetof(abi::Trs,scale)==24);
    static_assert(sizeof(abi::Status)==4 && sizeof(abi::Key)==4);
    CHECK(sizeof(abi::NativeApi)==16+11*sizeof(void*));
    CHECK(sizeof(abi::ManagedApi)==8+6*sizeof(void*));
    CHECK(offsetof(abi::NativeApi,is_valid)==16);
    CHECK(offsetof(abi::NativeApi,destroy_entity)==16+10*sizeof(void*));
    CHECK(offsetof(abi::ManagedApi,describe)==8);
}

TEST_CASE("script input coalesces edges and keeps independent frame and fixed latches") {
    ScriptInput input; using K=abi::Key; using Q=abi::InputQuery;
    for(unsigned i=0;i<unsigned(K::Count);++i) {
        const auto key=K(i); input.clear();
        input.event(key,true); input.event(key,true,true); input.event(key,false);
        for(bool fixed:{false,true}) {
            CHECK_FALSE(input.query(key,Q::Down,fixed));
            CHECK(input.query(key,Q::Pressed,fixed)); CHECK(input.query(key,Q::Released,fixed));
        }
        input.finish_frame(); CHECK_FALSE(input.query(key,Q::Pressed,false));
        CHECK(input.query(key,Q::Pressed,true));
        for(int observer=0;observer<3;++observer) CHECK(input.query(key,Q::Pressed,true));
        input.finish_fixed_tick();
        for(int tick=1;tick<8;++tick) CHECK_FALSE(input.query(key,Q::Pressed,true));
        CHECK_FALSE(input.query(key,Q::Pressed,false));
        input.event(key,true,true); CHECK_FALSE(input.query(key,Q::Pressed,false));
        input.clear(); CHECK_FALSE(input.query(key,Q::Down,false));
    }
    input.event(K::W,true);input.finish_frame();input.focus_lost();
    for(bool fixed:{false,true}) {
        CHECK_FALSE(input.query(K::W,Q::Down,fixed)); CHECK_FALSE(input.query(K::W,Q::Pressed,fixed));
        CHECK(input.query(K::W,Q::Released,fixed));
    }
    input.clear();
    for(bool fixed:{false,true}) CHECK_FALSE(input.query(K::W,Q::Released,fixed));
    input.event(K::Count,true); CHECK_FALSE(input.query(K::Count,Q::Down,false));
}

TEST_CASE("script fields validate known types and preserve unknown JSON transactionally") {
    std::map<std::string,ScriptField> fields={
        {"Float",{"float","1.25"}},{"Int",{"int","-2147483648"}},{"Bool",{"bool","true"}},
        {"Text",{"string",R"("\u0000caf\u00e9\u6f22")"}},{"Vector",{"vec3","[1,-2,3.5]"}},
        {"Zero",{"entity","0"}},{"Absent",{"entity","null"}},{"Huge",{"entity","18446744073709551615"}},
        {"Future",{"new-type",R"({"opaque":[1,true,"data"]})"}}};
    std::string error;std::map<std::string,ScriptField> parsed;
    REQUIRE_MESSAGE(parse_script_fields(serialize_script_fields(fields),parsed,error),error); check_fields(fields,parsed);
    for(const auto& [type,value]:std::vector<std::pair<std::string,std::string>>{
        {"float","null"},{"float","1e400"},{"int","2147483648"},{"int","1.5"},{"bool","1"},
        {"string","true"},{"vec3","[1,2]"},{"vec3","[1,2,3,4]"},{"vec3","[1,null,3]"},
        {"entity","-1"},{"entity","18446744073709551616"},{"entity","1.5"}}) {
        INFO(type,value); const auto before=parsed;
        CHECK_FALSE(parse_script_fields("{\"bad\":{\"type\":\""+type+"\",\"value\":"+value+"}}",parsed,error));
        CHECK_FALSE(error.empty()); check_fields(before,parsed);
    }
}

TEST_CASE("script snapshots duplication and scene IO keep authored fields and persistent references") {
    Scene scene; const auto root=scene.import_object("root",0),child=scene.create_object("child",root),outside=scene.create_object("outside");
    const auto original=script("MissingType",{{"Inside",{"entity"," \n"+std::to_string(child.persistentId)+"\t "}},{"Root",{"entity"," 0 "}},
        {"Outside",{"entity",std::to_string(outside.persistentId)}},{"None",{"entity","null"}},
        {"Orphan",{"removed-type",R"({"saved":true})"}},{"Value",{"float","3.5"}}});
    REQUIRE(scene.set(root,original)); const auto snapshot=scene.take_snapshot();
    const auto copy=scene.duplicate_subtree(root); REQUIRE(copy);const auto copiedChild=scene.read<Hierarchy>(copy)->children.at(0);
    const auto* copied=scene.read<Script>(copy); REQUIRE(copied);
    CHECK(copied->fields.at("Inside").valueJson==std::to_string(copiedChild));
    CHECK(copied->fields.at("Root").valueJson==std::to_string(copy.persistentId));
    CHECK(copied->fields.at("Outside").valueJson==std::to_string(outside.persistentId));
    CHECK(copied->fields.at("None").valueJson=="null");
    auto changed=original;changed.fields["Value"].valueJson="99";REQUIRE(scene.set(root,changed));
    REQUIRE(scene.destroy_object(child));scene.drain_destruction();const auto old=root;
    REQUIRE(scene.restore_snapshot(snapshot)); CHECK_FALSE(scene.valid(old));
    const auto restored=scene.get(0);REQUIRE(restored);CHECK(*scene.read<Script>(restored)==original);
    REQUIRE(scene.valid(scene.get(child.persistentId)));CHECK_FALSE(scene.valid(copy));
    std::string json,error; REQUIRE_MESSAGE(serialize_scene_json(scene,{}, {},json,error),error);
    StagedScene reopened;REQUIRE_MESSAGE(parse_scene_json(json,"script-roundtrip.json",{},reopened,error),error);
    REQUIRE(reopened.scene.read<Script>(reopened.scene.get(0)));
    const auto& result=*reopened.scene.read<Script>(reopened.scene.get(0));
    CHECK(result.assembly==original.assembly);CHECK(result.typeName==original.typeName);check_fields(result.fields,original.fields);
    const auto saved=json;auto invalid=original;invalid.fields["Value"].valueJson="null";
    CHECK_FALSE(scene.set(restored,invalid));CHECK(*scene.read<Script>(restored)==original);
    CHECK_FALSE(parse_scene_json(R"({"version":2,"nextPersistentId":3,"objects":[{"id":0,"parent":null,"siblingIndex":0,"components":{"script":{"assembly":"A","type":"B","enabled":true,"fields":{"x":{"type":"int","value":false}}}}}]})","invalid-script.json",{},reopened,error));
}

TEST_CASE("script host without runtime diagnoses failure and preserves saved components") {
    Scene scene;PhysicsWorld physics;ScriptHost host;host.bind(scene,physics);
    const auto ref=scene.create_object("saved");const auto value=script("MissingType",{{"Retained",{"string",R"("data")"}}});
    REQUIRE(scene.set(ref,value));
    CHECK_FALSE(host.initialize(std::filesystem::temp_directory_path()/"mirabilis-no-such-managed-directory"));
    CHECK_FALSE(host.available());CHECK_FALSE(host.error().empty());
    host.start();host.fixed_update(.01f);host.update(.01f);host.stop();
    CHECK(host.live_count()==0);CHECK(*scene.read<Script>(ref)==value);
}

TEST_CASE("native script boundary refuses every scene call outside callback and on worker thread") {
    Runtime run;const auto ref=run.add();const auto& api=run.host.native_api();
    const abi::Entity entity{ref.epoch,uint32_t(ref.entity),0,ref.persistentId};
    auto check=[&](abi::Status expected) {
        uint8_t flag=99;uint32_t required=99;abi::Entity resolved{};abi::Trs transform{{0,0,0},{0,0,0},{1,1,1}};
        CHECK(api.is_valid(api.context,entity,&flag)==expected);
        CHECK(api.is_pending(api.context,entity,&flag)==expected);
        CHECK(api.resolve(api.context,ref.persistentId,&resolved)==expected);
        CHECK(api.get_transform(api.context,entity,0,&transform)==expected);
        CHECK(api.set_transform(api.context,entity,0,&transform)==expected);
        CHECK(api.get_name(api.context,entity,nullptr,0,&required)==expected);
        CHECK(api.set_name(api.context,entity,{nullptr,0})==expected);
        CHECK(api.input(api.context,0,0,&flag)==expected);
        CHECK(api.body_command(api.context,entity,0,{1,2,3})==expected);
        CHECK(api.destroy_entity(api.context,entity)==expected);
    };
    check(abi::Status::OutsideCallback);
    std::thread worker([&]{check(abi::Status::WrongThread);});worker.join();
    CHECK(run.scene.valid(ref));CHECK_FALSE(run.scene.pending(ref));CHECK(run.scene.read<Name>(ref)->value=="probe");
}

TEST_CASE("real managed lifecycle follows fixed frame order and reconciles script ownership") {
    Runtime run; const auto a=run.add(),b=run.add();run.host.start();CHECK(run.host.live_count()==2);
    CHECK(field_int(run.scene,a,"Creates")==1);run.tick();run.host.update(.25f);
    CHECK(field_int(run.scene,a,"Fixed")==1);CHECK(field_int(run.scene,a,"Updates")==1);
    const auto& log=run.host.diagnostics();
    auto pos=[&](std::string text){return std::find_if(log.begin(),log.end(),[&](const auto& s){return s.find(text)!=std::string::npos;})-log.begin();};
    CHECK(pos(marker(a,"create"))<pos(marker(a,"fixed")));CHECK(pos(marker(a,"fixed"))<pos(marker(a,"update")));
    REQUIRE(run.scene.remove<Script>(a));CHECK(run.host.live_count()==1);CHECK(occurrences(run.host,marker(a,"destroy"))==1);
    REQUIRE(run.scene.set(a,script()));run.host.reconcile();CHECK(run.host.live_count()==2);
    REQUIRE(run.scene.set(a,script("LifecycleProbe",{{"Label",{"string",R"("replacement")"}}})));
    run.host.reconcile();CHECK(run.host.live_count()==2);CHECK(occurrences(run.host,marker(a,"destroy"))==2);
    auto disabled=*run.scene.read<Script>(b);disabled.enabled=false;REQUIRE(run.scene.set(b,disabled));run.host.reconcile();CHECK(run.host.live_count()==1);
    run.host.stop();CHECK(run.host.live_count()==0);CHECK(occurrences(run.host,marker(a,"destroy"))==3);
}

TEST_CASE("managed callback faults disable updates but always attempt final cleanup once") {
    for(const char* phase:{"create","fixed","update","destroy"}) {
        INFO(phase);Runtime run;const auto ref=run.add(script("LifecycleProbe",{{"Fail",{"string","\""+std::string(phase)+"\""}}}));
        run.host.start();run.host.fixed_update(.01f);run.host.update(.01f);
        if(std::string(phase)!="destroy") {
            CHECK(run.host.faulted(ref));const auto before=*run.scene.read<Script>(ref);
            run.host.fixed_update(.01f);run.host.update(.01f);CHECK(*run.scene.read<Script>(ref)==before);
        }
        REQUIRE(run.scene.destroy_object(ref));CHECK(run.scene.destroy_object(ref));
        run.scene.drain_destruction([&](EntityRef r){run.physics.remove(r);});
        CHECK(run.host.live_count()==0);CHECK(occurrences(run.host,marker(ref,"destroy"))==1);
        CHECK(logged(run.host,"fixture-"+std::string(phase)));CHECK(logged(run.host,"LifecycleProbe"));CHECK(logged(run.host,"at "));
    }
    Runtime run;const auto bad=run.add(script("ConstructorThrows"));run.host.start();CHECK(run.host.live_count()==0);
    CHECK(logged(run.host,"fixture-constructor"));CHECK_FALSE(logged(run.host,marker(bad,"destroy")));
}

TEST_CASE("managed missing assembly type and incompatible fields retain saved data") {
    Runtime run;
    for(const auto& value:{Script{"Absent.Assembly","Absent.Type",true,{{"Saved",{"int","17"}}}},script("AbsentType",{{"Saved",{"bool","true"}}})}) {
        const auto ref=run.add(value);run.host.reconcile();CHECK(run.host.live_count()==0);CHECK(*run.scene.read<Script>(ref)==value);
    }
    auto value=script("FieldProbe",{{"Number",{"string",R"("old-type")"}},{"Removed",{"vec3","[9,8,7]"}}});
    const auto ref=run.add(value);run.host.start();CHECK(run.host.live_count()==1);
    REQUIRE(run.scene.read<Script>(ref));const auto& actual=run.scene.read<Script>(ref)->fields;
    CHECK(actual.at("Number")==value.fields.at("Number"));CHECK(actual.at("Removed")==value.fields.at("Removed"));
    CHECK_FALSE(run.host.diagnostics().empty());
}

TEST_CASE("managed teardown can read pending entities and bodies with children before parent") {
    Runtime run;const auto parent=run.add(script("CleanupProbe")),child=run.scene.create_object("child",parent);
    REQUIRE(run.scene.set(child,script("CleanupProbe")));Collider collider;collider.hasCollision=true;REQUIRE(run.scene.set(child,collider));
    REQUIRE(run.physics.synchronize(run.scene,false));run.host.start();
    REQUIRE(run.scene.destroy_object(parent));run.host.update(.01f);run.host.fixed_update(.01f);
    CHECK_FALSE(logged(run.host,marker(parent,"update")));CHECK_FALSE(logged(run.host,marker(child,"fixed")));
    run.scene.drain_destruction([&](EntityRef ref){CHECK(run.scene.valid(ref));if(ref==child)CHECK(run.physics.has_body(ref));run.physics.remove(ref);});
    CHECK(run.host.live_count()==0);CHECK(logged(run.host,marker(child,"cleanup-ok")));CHECK(logged(run.host,marker(parent,"cleanup-ok")));
    const auto& log=run.host.diagnostics();
    auto p=[&](EntityRef r){return std::find_if(log.begin(),log.end(),[&](const auto& s){return s.find(marker(r,"cleanup-ok"))!=std::string::npos;})-log.begin();};
    CHECK(p(child)<p(parent));
}

TEST_CASE("managed destruction queues other mutations and drains nested destruction once") {
    Runtime run;const auto target=run.scene.create_object("untouched"),victim=run.add(script("CleanupProbe",{{"Target",{"entity",std::to_string(target.persistentId)}}}));
    run.host.start();REQUIRE(run.scene.destroy_object(victim));run.scene.drain_destruction();
    CHECK(run.scene.read<Name>(target)->value=="untouched");CHECK(run.scene.queued_command_count()>0);
    run.scene.apply_commands();CHECK(run.scene.read<Name>(target)->value=="cleanup-target");
    const auto nested=run.add(script("CleanupProbe"));const auto outer=run.add(script("CleanupProbe",{{"DestroyTarget",{"bool","true"}},{"Target",{"entity",std::to_string(nested.persistentId)}}}));
    run.host.reconcile();REQUIRE(run.scene.destroy_object(outer));run.scene.drain_destruction();
    CHECK_FALSE(run.scene.valid(nested));CHECK(run.host.live_count()==0);
    CHECK(occurrences(run.host,marker(nested,"cleanup-ok"))==1);CHECK(occurrences(run.host,marker(outer,"cleanup-ok"))==1);
}

TEST_CASE("real managed field defaults metadata orphan preservation and play reset") {
    Runtime run;auto authored=script("FieldProbe",{{"Number",{"float","2.5"}},{"Count",{"int","7"}},{"Flag",{"bool","true"}},
        {"Text",{"string",R"("\u0000caf\u00e9\u6f22")"}},{"Vector",{"vec3","[1,2,3]"}},
        {"MissingEntity",{"entity","null"}},{"Mutate",{"bool","true"}},{"Future",{"new-type","{\"opaque\":true}"}}});
    const auto ref=run.add(authored);const auto snapshot=run.scene.take_snapshot();
    std::vector<std::string> diagnostics;const auto metadata=run.host.describe(script("FieldProbe"),diagnostics);
    for(const char* name:{"Number","Count","Flag","Text","Vector","MissingEntity","Mutate"})CHECK(metadata.fields.contains(name));
    CHECK_FALSE(metadata.fields.contains("StaticField"));CHECK_FALSE(metadata.fields.contains("ReadonlyField"));CHECK_FALSE(metadata.fields.contains("Unsupported"));
    CHECK(canonical(metadata.fields.at("Number").valueJson)=="0");
    run.host.start();CHECK(logged(run.host,"field-bind-ok"));run.host.update(.01f);
    CHECK(run.scene.read<Script>(ref)->fields.at("Number").valueJson!="2.5");
    CHECK(run.scene.read<Script>(ref)->fields.at("Future")==authored.fields.at("Future"));
    run.host.stop();run.scene.clear_queues();REQUIRE(run.scene.restore_snapshot(snapshot));
    CHECK(*run.scene.read<Script>(run.scene.get(ref.persistentId))==authored);CHECK_FALSE(run.scene.valid(ref));
    run.host.start();CHECK(run.host.live_count()==1);CHECK(occurrences(run.host,"field-bind-ok")==2);
    run.host.stop();run.scene.clear_queues();REQUIRE(run.scene.restore_snapshot(snapshot));
    CHECK(*run.scene.read<Script>(run.scene.get(ref.persistentId))==authored);CHECK(run.host.live_count()==0);
}

TEST_CASE("managed stale epoch thread validation and UTF8 strings fail safely") {
    Runtime run;const auto ref=run.add(script("BoundaryProbe"));run.host.start();
    CHECK(logged(run.host,"boundary-ok"));CHECK_FALSE(run.host.faulted(ref));
    const auto snapshot=run.scene.take_snapshot();run.host.stop();run.scene.clear_queues();REQUIRE(run.scene.restore_snapshot(snapshot));run.host.start();
    CHECK(logged(run.host,"stale-epoch-ok"));CHECK(run.host.live_count()==1);
}

TEST_CASE("managed input observes first fixed edge once across eight ticks and zero tick frames") {
    Runtime run;const auto a=run.add(script("InputProbe")),b=run.add(script("InputProbe"));run.host.start();
    run.host.input().event(abi::Key::Space,true);
    for(int i=0;i<8;++i)run.host.fixed_update(.01f);
    run.host.update(.01f);
    for(const auto ref:{a,b}) {CHECK(field_int(run.scene,ref,"FixedPresses")==1);CHECK(field_int(run.scene,ref,"FramePresses")==1);CHECK(field_int(run.scene,ref,"Held")==9);}
    run.host.input().event(abi::Key::Space,false);run.host.input().event(abi::Key::Space,true);run.host.input().event(abi::Key::Space,false);
    run.host.update(.01f); // no fixed tick: the fixed edge survives this frame
    run.host.update(.01f);run.host.fixed_update(.01f);run.host.update(.01f);
    for(const auto ref:{a,b}) {CHECK(field_int(run.scene,ref,"FixedPresses")==2);CHECK(field_int(run.scene,ref,"FramePresses")==2);CHECK(field_int(run.scene,ref,"FixedReleases")==1);CHECK(field_int(run.scene,ref,"FrameReleases")==1);}
    run.host.input().event(abi::Key::Space,true);run.host.input().focus_lost();run.host.fixed_update(.01f);run.host.update(.01f);
    for(const auto ref:{a,b}) {CHECK(field_int(run.scene,ref,"FixedPresses")==2);CHECK(field_int(run.scene,ref,"FixedReleases")==2);}
    run.host.input().event(abi::Key::Space,true);run.host.stop();run.host.start();run.host.fixed_update(.01f);run.host.update(.01f);
    CHECK(field_int(run.scene,a,"FixedPresses")==2);
}

TEST_CASE("managed dynamic writes teleport at command phase and discarded commands cannot survive destruction") {
    for(const bool destroy:{false,true}) {
        Runtime run;const auto ref=run.add(script("PhysicsProbe",{{"DestroyAfter",{"bool",destroy?"true":"false"}}}));
        Collider collider;collider.hasCollision=true;REQUIRE(run.scene.set(ref,collider));RigidBody body;body.motion=BodyMotion::Dynamic;body.gravityFactor=0;REQUIRE(run.scene.set(ref,body));
        run.scene.set_physics_play(true);run.scene.set_teleport_handler([&](EntityRef e,const Transform& t){return run.physics.queue_teleport(run.scene,e,t);});
        REQUIRE(run.physics.synchronize(run.scene,false));run.host.start();run.host.fixed_update(1.f/120);
        CHECK(run.scene.read<Transform>(ref)->position.x==doctest::Approx(0));
        run.scene.apply_commands();REQUIRE(run.physics.synchronize(run.scene,false));run.physics.step(run.scene,1.f/120);
        if(destroy) {run.scene.drain_destruction([&](EntityRef e){run.physics.remove(e);});CHECK_FALSE(run.scene.valid(ref));CHECK(run.physics.body_count()==0);CHECK(run.host.live_count()==0);}
        else {CHECK(run.scene.read<Transform>(ref)->position.x==doctest::Approx(10).epsilon(.01));CHECK(run.physics.velocity(ref).y>0);CHECK(logged(run.host,"teleport-queued"));run.tick();CHECK(run.scene.read<Transform>(ref)->position.x==doctest::Approx(10).epsilon(.01));}
    }
}

TEST_CASE("managed scene replacement and shutdown release handles while teardown reads stay valid") {
    Runtime run;const auto old=run.add(script("CleanupProbe"));run.host.start();
    run.host.stop();run.scene.clear_queues();Scene next;const auto replacement=next.import_object("same-id",old.persistentId);REQUIRE(next.set(replacement,script("CleanupProbe")));
    run.scene.replace(std::move(next));run.physics.clear();run.host.start();CHECK_FALSE(run.scene.valid(old));CHECK(run.host.live_count()==1);
    CHECK(occurrences(run.host,marker(old,"cleanup-ok"))==1);
    run.host.shutdown();CHECK(run.host.live_count()==0);CHECK(occurrences(run.host,marker(old,"cleanup-ok"))==2);
    uint8_t valid=1;const auto& api=run.host.native_api();const auto fresh=run.scene.get(old.persistentId);
    CHECK(api.is_valid(api.context,{fresh.epoch,uint32_t(fresh.entity),0,fresh.persistentId},&valid)!=abi::Status::Ok);
}


TEST_CASE("OnCreate self destruction keeps its instance until deferred cleanup and suppresses updates") {
    Runtime run;const auto ref=run.add(script("SelfDestroyProbe"));run.host.start();
    CHECK(run.scene.valid(ref));CHECK(run.scene.pending(ref));CHECK(run.host.live_count()==1);
    run.host.update(.01f);run.host.fixed_update(.01f);
    CHECK_FALSE(logged(run.host,marker(ref,"fixed")));CHECK_FALSE(logged(run.host,marker(ref,"update")));
    run.scene.drain_destruction();CHECK(run.host.live_count()==0);CHECK_FALSE(run.scene.valid(ref));
    CHECK(occurrences(run.host,marker(ref,"destroy"))==1);
}

TEST_CASE("teardown generated commands are discarded before reset replacement and shutdown") {
    for(const int mode:{0,1,2}) {
        INFO(mode);Runtime run;const auto target=run.scene.create_object("authored");
        const auto ref=run.add(script("CleanupProbe",{{"Target",{"entity",std::to_string(target.persistentId)}}}));
        const auto snapshot=run.scene.take_snapshot();run.host.start();
        if(mode==2)run.host.shutdown();else run.host.stop();
        CHECK(logged(run.host,marker(ref,"cleanup-ok")));
        if(mode==2)CHECK(run.scene.queued_command_count()==0);
        else run.scene.clear_queues();
        if(mode==0)REQUIRE(run.scene.restore_snapshot(snapshot));
        else if(mode==1){Scene next;next.import_object("authored",target.persistentId);run.scene.replace(std::move(next));}
        CHECK(run.scene.apply_commands()==0);CHECK(run.scene.read<Name>(run.scene.get(target.persistentId))->value=="authored");
        CHECK(run.host.live_count()==0);
    }
}

TEST_CASE("script moving static parent flushes collider player and render transforms before physics") {
    Runtime run;const auto parent=run.scene.create_object("support-parent"),support=run.scene.create_object("support",parent),body=run.scene.create_object("resting-box");
    Collider collider;collider.hasCollision=true;collider.colliderHalfExtents={2,.5f,2};REQUIRE(run.scene.set(support,collider));
    collider.colliderHalfExtents={.5f,.5f,.5f};REQUIRE(run.scene.set(body,collider));
    RigidBody dynamic;dynamic.motion=BodyMotion::Dynamic;REQUIRE(run.scene.set(body,dynamic));
    Transform pose;pose.position={0,2,0};REQUIRE(run.scene.set_local_transform(body,pose));
    run.scene.set_physics_play(true);for(int i=0;i<600;++i)run.tick();CHECK(run.physics.sleeping(body));
    const auto mover=run.add(script("ParentMoveProbe",{{"Target",{"entity",std::to_string(parent.persistentId)}}}));run.host.start();run.host.fixed_update(1.f/120);
    CHECK_FALSE(run.host.faulted(mover));CHECK(logged(run.host,"parent-moved"));
    CHECK(run.scene.world_matrix(support)[3].x==doctest::Approx(10));
    run.scene.apply_commands();REQUIRE(run.physics.synchronize(run.scene,false));
    REQUIRE(run.physics.body_world(support));ecs_test::check_matrix(*run.physics.body_world(support),run.scene.world_matrix(support));
    const auto collision=collect_player_collision(run.scene,{});REQUIRE(collision.walls.size()==1);
    CHECK(collision.walls[0].min.x==doctest::Approx(8));CHECK(collision.walls[0].max.x==doctest::Approx(12));
    run.physics.step(run.scene,1.f/120);CHECK_FALSE(run.physics.sleeping(body));
}


TEST_CASE("metadata does not construct scripts and unresolved saved references retain their IDs") {
    Runtime run;std::vector<std::string> messages;
    const auto described=run.host.describe(script("ConstructorThrows"),messages);
    CHECK(described.fields.contains("VisibleField"));CHECK_FALSE(logged(run.host,"fixture-constructor"));CHECK(run.host.live_count()==0);
    const auto ref=run.add(script("FieldProbe",{{"MissingEntity",{"entity","987654321"}}}));
    run.host.start();run.host.update(.01f);run.host.fixed_update(.01f);
    CHECK_FALSE(run.host.faulted(ref));CHECK(run.scene.read<Script>(ref)->fields.at("MissingEntity").valueJson=="987654321");
}

TEST_CASE("direct live Scene restore and replacement clean old managed instances and discard teardown writes") {
    for(const bool replace:{false,true}) {
        Runtime run;const auto target=run.scene.import_object("authored-target",0);
        const auto ref=run.add(script("CleanupProbe",{{"Target",{"entity","0"}}}));const auto snapshot=run.scene.take_snapshot();
        run.host.start();CHECK(run.host.live_count()==1);
        if(replace) {Scene next;next.import_object("authored-target",0);const auto copied=next.import_object("new-script",ref.persistentId);REQUIRE(next.set(copied,script()));run.scene.replace(std::move(next));}
        else REQUIRE(run.scene.restore_snapshot(snapshot));
        CHECK(run.host.live_count()==0);CHECK(logged(run.host,marker(ref,"cleanup-ok")));
        CHECK_FALSE(run.scene.valid(ref));CHECK_FALSE(run.scene.valid(target));CHECK(run.scene.apply_commands()==0);
        CHECK(run.scene.read<Name>(run.scene.get(0))->value=="authored-target");
        run.host.start();CHECK(run.host.live_count()==1);CHECK_FALSE(run.host.faulted(run.scene.get(ref.persistentId)));
    }
}

TEST_CASE("scripted authored demo round trips and real PushBox forces the native falling box") {
    const auto source=std::filesystem::path(MIRABILIS_TEST_SOURCE_DIR)/"assets/scenes/scripted_physics_demo.json";
    StagedScene staged,reopened;std::string error,json;
    REQUIRE_MESSAGE(parse_scene_json(ecs_test::read_file(source),source.filename().string(),{},staged,error),error);
    REQUIRE_MESSAGE(serialize_scene_json(staged.scene,staged.settings,staged.authoredPortals,json,error),error);
    REQUIRE_MESSAGE(parse_scene_json(json,"scripted-copy.json",{},reopened,error),error);
    ecs_test::check_snapshot(staged.scene.take_snapshot(),reopened.scene.take_snapshot());
    Runtime run;run.scene.replace(std::move(reopened.scene));run.host.bind(run.scene,run.physics);
    const auto moving=run.scene.get(3);REQUIRE(run.scene.read<Script>(moving));
    CHECK(run.scene.read<Script>(moving)->assembly=="Mirabilis.SampleScripts");
    CHECK(run.scene.read<Script>(moving)->typeName=="Mirabilis.SampleScripts.PushBox");
    run.scene.set_physics_play(true);run.scene.set_teleport_handler([&](EntityRef r,const Transform& t){return run.physics.queue_teleport(run.scene,r,t);});
    REQUIRE(run.physics.synchronize(run.scene,false));run.host.start();CHECK(run.host.live_count()==1);
    const auto original=run.scene.world_matrix(moving)[3];
    for(int i=0;i<240;++i)run.tick();
    CHECK_FALSE(run.host.faulted(moving));CHECK(run.scene.world_matrix(moving)[3].x>original.x);
    CHECK(run.scene.world_matrix(moving)[3].y>=.45f);CHECK(run.scene.world_matrix(moving)[3].y<original.y);
    CHECK(run.physics.body_count()==3);
}


TEST_CASE("managed callback iteration skips a later entity marked pending by an earlier script") {
    Runtime run;const auto killer=run.add(script("DestroyOtherProbe")),victim=run.add();
    REQUIRE(run.scene.set(killer,script("DestroyOtherProbe",{{"Target",{"entity",std::to_string(victim.persistentId)}}})));
    run.host.start();run.host.update(.01f);run.host.fixed_update(.01f);
    CHECK(run.scene.valid(victim));CHECK(run.scene.pending(victim));CHECK(run.host.live_count()==2);
    CHECK(field_int(run.scene,victim,"Updates")==0);CHECK(field_int(run.scene,victim,"Fixed")==0);
    CHECK_FALSE(logged(run.host,marker(victim,"update")));CHECK_FALSE(logged(run.host,marker(victim,"fixed")));
    run.scene.drain_destruction();CHECK(run.host.live_count()==1);CHECK(occurrences(run.host,marker(victim,"destroy"))==1);
}

TEST_CASE("OnDestroy world and body teleports reach live dynamic targets on the very next fixed step") {
    for(const int mode:{0,1,2}) {
        INFO(mode);Runtime run;const auto target=run.scene.create_object("dynamic target");
        Collider collider;collider.hasCollision=true;REQUIRE(run.scene.set(target,collider));
        RigidBody body;body.motion=BodyMotion::Dynamic;body.gravityFactor=0;REQUIRE(run.scene.set(target,body));
        run.scene.set_physics_play(true);run.scene.set_teleport_handler([&](EntityRef ref,const Transform& transform){return run.physics.queue_teleport(run.scene,ref,transform);});
        REQUIRE(run.physics.synchronize(run.scene,false));
        const auto owner=run.add(script("CleanupWorldProbe",{{"Target",{"entity",std::to_string(target.persistentId)}},{"Mode",{"int",std::to_string(mode)}}}));
        run.host.start();REQUIRE(run.scene.destroy_object(owner));run.scene.drain_destruction();
        CHECK(logged(run.host,"cleanup-world-queued"));CHECK(run.host.live_count()==0);
        CHECK(run.scene.read<Transform>(target)->position.x==doctest::Approx(0));
        CHECK(run.scene.queued_command_count()>0);
        run.scene.apply_commands(); // exactly one step-2 command phase, not a recursive drain
        REQUIRE(run.physics.synchronize(run.scene,false));run.physics.step(run.scene,1.f/120);
        Transform expected;expected.position={7,8,9};expected.rotation={.2f,.4f,.6f};
        ecs_test::check_matrix(run.scene.world_matrix(target),to_matrix(expected));
        REQUIRE(run.physics.body_world(target));ecs_test::check_matrix(*run.physics.body_world(target),to_matrix(expected));
        CHECK(run.scene.queued_command_count()==0);
    }
}

namespace {
#ifdef _WIN32
#define SCRIPT_TEST_FD _fileno
#define SCRIPT_TEST_DUP _dup
#define SCRIPT_TEST_DUP2 _dup2
#define SCRIPT_TEST_CLOSE _close
#else
#define SCRIPT_TEST_FD fileno
#define SCRIPT_TEST_DUP dup
#define SCRIPT_TEST_DUP2 dup2
#define SCRIPT_TEST_CLOSE close
#endif
struct DiagnosticConsoleCapture {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("mirabilis-script-console-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".log");
    std::FILE* file{};
    int out{-1},err{-1};
    void restore() {
        std::cout.flush();std::cerr.flush();std::fflush(nullptr);
        if(out>=0){SCRIPT_TEST_DUP2(out,SCRIPT_TEST_FD(stdout));SCRIPT_TEST_CLOSE(out);out=-1;}
        if(err>=0){SCRIPT_TEST_DUP2(err,SCRIPT_TEST_FD(stderr));SCRIPT_TEST_CLOSE(err);err=-1;}
    }
    DiagnosticConsoleCapture() {
        file=std::fopen(path.string().c_str(),"w+b");
        if(!file)throw std::runtime_error("Cannot create console capture");
        std::cout.flush();std::cerr.flush();std::fflush(nullptr);
        out=SCRIPT_TEST_DUP(SCRIPT_TEST_FD(stdout));err=SCRIPT_TEST_DUP(SCRIPT_TEST_FD(stderr));
        if(out<0 || err<0 || SCRIPT_TEST_DUP2(SCRIPT_TEST_FD(file),SCRIPT_TEST_FD(stdout))<0 ||
            SCRIPT_TEST_DUP2(SCRIPT_TEST_FD(file),SCRIPT_TEST_FD(stderr))<0) {
            restore();std::fclose(file);file=nullptr;std::error_code error;std::filesystem::remove(path,error);
            throw std::runtime_error("Cannot redirect console capture");
        }
    }
    std::string finish() {
        restore();std::rewind(file);std::string text;char chunk[8192];
        while(const auto size=std::fread(chunk,1,sizeof(chunk),file))text.append(chunk,size);
        if(std::ferror(file))throw std::runtime_error("Cannot read console capture");
        return text;
    }
    ~DiagnosticConsoleCapture() {
        restore();if(file)std::fclose(file);std::error_code error;std::filesystem::remove(path,error);
    }
};
#undef SCRIPT_TEST_FD
#undef SCRIPT_TEST_DUP
#undef SCRIPT_TEST_DUP2
#undef SCRIPT_TEST_CLOSE
}

TEST_CASE("managed diagnostic history bounds count bytes and UTF8 while console stays complete") {
    for(const int mode:{0,1,2,3}) {
        INFO(mode);Runtime run;const auto ref=run.add(script("DiagnosticFloodProbe",{{"Mode",{"int",std::to_string(mode)}}}));
        std::string console;
        {DiagnosticConsoleCapture capture;run.host.start();console=capture.finish();}
        CHECK_FALSE(run.host.faulted(ref));CHECK(run.host.live_count()==1);
        const auto& history=run.host.diagnostics();REQUIRE_FALSE(history.empty());
        CHECK(history.size()<=256);
        size_t bytes=0,capacity=0;
        for(const auto& line:history) {
            bytes+=line.size();CHECK(line.size()<=64*1024);CHECK(simdjson::validate_utf8(line));
            capacity+=line.capacity();CHECK(line.capacity()<=2*64*1024);
        }
        CHECK(bytes<=1024*1024);
        CHECK(capacity<=2*1024*1024);
        if(mode==0) {
            CHECK(history.back().find("history-count-319")!=std::string::npos);
            for(int i=0;i<320;++i) {
                char marker[32];std::snprintf(marker,sizeof(marker),"history-count-%03d:",i);
                CHECK(console.find(marker)!=std::string::npos);
            }
        } else if(mode==1) {
            CHECK(history.back().find("history-byte-tail-39")!=std::string::npos);
            for(int i=0;i<40;++i) {
                char marker[40];std::snprintf(marker,sizeof(marker),":history-byte-tail-%02d",i);
                CHECK(console.find(std::string(40000,'x')+marker)!=std::string::npos);
            }
        } else if(mode==2) {
            CHECK(history.back().find("history-large-3")!=std::string::npos);
            CHECK(history.back().find("history-complete-tail-3")==std::string::npos);
            std::string payload;for(int i=0;i<12000;++i)payload+="\xe6\xbc\xa2\xf0\x9f\x99\x82";
            for(int i=0;i<4;++i) {
                const auto message="history-large-"+std::to_string(i)+":"+std::string(i,'a')+payload+":history-complete-tail-"+std::to_string(i);
                CHECK(console.find(message)!=std::string::npos);
            }
        } else {
            CHECK(history.back().find("history-giant-latest:")!=std::string::npos);
            CHECK(history.back().find("history-giant-complete-tail")==std::string::npos);
            std::string payload;for(int i=0;i<310000;++i)payload+="\xe6\xbc\xa2\xf0\x9f\x99\x82";
            REQUIRE(payload.size()>=2*1024*1024);
            CHECK(console.find("history-giant-latest:"+payload+":history-giant-complete-tail")!=std::string::npos);
        }
        std::cout<<"Diagnostic flood mode "<<mode<<": console bytes="<<console.size()<<", retained messages="<<history.size()<<", retained bytes="<<bytes<<", retained capacity="<<capacity<<'\n';
    }
}

TEST_CASE("native schema cache avoids repeated errors and preserves caller state across keys") {
    Runtime run;auto authored=script("CacheMissingType",{{"Saved",{"string",R"("retained")"}},{"Future",{"new-type","{\"opaque\":true}"}}});
    authored.enabled=false;const auto ref=run.add(authored);
    std::vector<std::string> errors;const auto missing=run.host.describe(authored,errors);
    REQUIRE_FALSE(errors.empty());CHECK(missing.fields.empty());
    const auto firstErrors=errors;const auto history=run.host.diagnostics();
    for(int i=0;i<32;++i) {
        auto query=authored;query.enabled=i%2==0;query.fields["Saved"]={"string","\"value-"+std::to_string(i)+"\""};
        const auto before=query;std::vector<std::string> repeated;
        const auto result=run.host.describe(query,repeated);
        CHECK(result.assembly==query.assembly);CHECK(result.typeName==query.typeName);CHECK(result.enabled==query.enabled);
        CHECK(result.fields.empty());CHECK(repeated==firstErrors);CHECK(query==before);
    }
    CHECK(run.host.diagnostics()==history);CHECK(*run.scene.read<Script>(ref)==authored);
    auto valid=script("FieldProbe",{{"Number",{"float","4.5"}},{"Orphan",{"string",R"("keep")"}}});valid.enabled=false;
    std::vector<std::string> messages;const auto first=run.host.describe(valid,messages);CHECK(messages.empty());
    CHECK(first.assembly==valid.assembly);CHECK(first.typeName==valid.typeName);CHECK_FALSE(first.enabled);
    REQUIRE(first.fields.contains("Number"));CHECK(first.fields.at("Number").type=="float");CHECK(canonical(first.fields.at("Number").valueJson)=="0");
    const auto saved=valid;valid.enabled=true;valid.fields["Number"].valueJson="99";
    messages.clear();const auto second=run.host.describe(valid,messages);CHECK(messages.empty());CHECK(second.enabled);
    check_fields(first.fields,second.fields);CHECK(valid.fields.at("Number").valueJson=="99");CHECK(saved.fields.at("Number").valueJson=="4.5");
    messages.clear();const auto different=run.host.describe(script("LifecycleProbe"),messages);CHECK(messages.empty());
    CHECK(different.fields.contains("Creates"));CHECK_FALSE(different.fields.contains("Number"));
    auto differentAssembly=authored;differentAssembly.assembly="Absent.Cache.Assembly";
    const auto previousCount=run.host.diagnostics().size();messages.clear();const auto absent=run.host.describe(differentAssembly,messages);
    CHECK_FALSE(messages.empty());CHECK(absent.assembly==differentAssembly.assembly);CHECK(absent.fields.empty());
    CHECK(run.host.diagnostics().size()>previousCount);
    messages.clear();const auto constructor=run.host.describe(script("ConstructorThrows"),messages);
    CHECK(messages.empty());CHECK(constructor.fields.contains("VisibleField"));CHECK_FALSE(logged(run.host,"fixture-constructor"));
    CHECK(run.host.live_count()==0);CHECK(*run.scene.read<Script>(ref)==authored);
}

