#include "scene_serialization.h"
#include "physics_world.h"
#include <simdjson.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using Json = simdjson::dom::element;
using Object = simdjson::dom::object;
constexpr std::string_view KnownComponents[]{"name", "transform", "visibility", "mesh", "material", "collider", "portalPlaceable", "timeTrialRole", "transformDrivenExternally", "rigidBody"};
[[noreturn]] void invalid(std::string message) { throw std::runtime_error(std::move(message)); }
Object object(Json value) { Object result; if (value.get_object().get(result)) invalid("Expected JSON object"); return result; }
simdjson::dom::array array(Json value) { simdjson::dom::array result; if (value.get_array().get(result)) invalid("Expected JSON array"); return result; }
std::optional<Json> optional(Object value, std::string_view key) {
    Json result; const auto code = value[key].get(result);
    if (code == simdjson::NO_SUCH_FIELD) return {};
    if (code) invalid("Invalid field: " + std::string(key));
    return result;
}
Json required(Object value, std::string_view key) {
    const auto result = optional(value, key);
    if (!result) invalid("Missing field: " + std::string(key));
    return *result;
}
uint64_t integer(Json value) { uint64_t result; if (value.get_uint64().get(result)) invalid("Expected unsigned integer"); return result; }
int64_t signed_integer(Json value) { int64_t result; if (value.get_int64().get(result)) invalid("Expected signed integer"); return result; }
std::string string(Json value) { std::string_view result; if (value.get_string().get(result)) invalid("Expected string"); return std::string(result); }
bool boolean(Json value) { bool result; if (value.get_bool().get(result)) invalid("Expected boolean"); return result; }
float number(Json value) {
    double result; if (value.get_double().get(result) || !std::isfinite(result) || std::abs(result) > std::numeric_limits<float>::max()) invalid("Expected finite float");
    return static_cast<float>(result);
}
template<class T> T vector(Json value) {
    T result{}; const auto values = array(value); size_t index = 0;
    for (Json component : values) {
        if (index >= static_cast<size_t>(result.length())) invalid("Vector has too many components");
        result[static_cast<typename T::length_type>(index++)] = number(component);
    }
    if (index != static_cast<size_t>(result.length())) invalid("Vector has too few components");
    return result;
}
template<class T, class F> void read_optional(Object value, std::string_view key, T& target, F convert) {
    if (const auto item = optional(value, key)) target = convert(*item);
}
void read_float(Object value, std::string_view key, float& target) { read_optional(value,key,target,number); }
void read_bool(Object value, std::string_view key, bool& target) { read_optional(value,key,target,boolean); }
void read_string(Object value, std::string_view key, std::string& target) { read_optional(value,key,target,string); }
const char* asset_name(SceneAssetKind kind) {
    switch (kind) { case SceneAssetKind::None:return "empty"; case SceneAssetKind::FloorQuad:return "floor"; case SceneAssetKind::UnitCube:return "cube"; case SceneAssetKind::SurfRamp:return "ramp"; case SceneAssetKind::ImportedGLTF:return "gltf"; }
    invalid("Invalid asset kind");
}
SceneAssetKind asset_kind(Json value) {
    const std::string name=string(value);
    if(name=="empty")return SceneAssetKind::None; if(name=="floor")return SceneAssetKind::FloorQuad;
    if(name=="cube")return SceneAssetKind::UnitCube; if(name=="ramp")return SceneAssetKind::SurfRamp; if(name=="gltf")return SceneAssetKind::ImportedGLTF;
    invalid("Unsupported asset: "+name);
}
const char* role_name(TimeTrialRole role) {
    switch(role) { case TimeTrialRole::None:return "none"; case TimeTrialRole::SpawnPoint:return "spawn"; case TimeTrialRole::StartTrigger:return "start"; case TimeTrialRole::FinishTrigger:return "finish"; }
    invalid("Invalid time-trial role");
}
TimeTrialRole role_kind(Json value) {
    const std::string name=string(value);
    if(name=="none")return TimeTrialRole::None; if(name=="spawn")return TimeTrialRole::SpawnPoint; if(name=="start")return TimeTrialRole::StartTrigger; if(name=="finish")return TimeTrialRole::FinishTrigger;
    invalid("Unsupported time-trial role: "+name);
}
RenderLayer layer_kind(Json value) { const auto result=integer(value); if(result>1)invalid("Invalid render layer"); return static_cast<RenderLayer>(result); }
CollisionShape shape_kind(Json value) { const auto result=integer(value); if(result>2)invalid("Invalid collider shape"); return static_cast<CollisionShape>(result); }
BodyMotion motion_kind(Json value) {
    const auto name=string(value);
    if(name=="Static")return BodyMotion::Static;
    if(name=="Dynamic")return BodyMotion::Dynamic;
    invalid("Unsupported rigid-body motion: "+name);
}
std::optional<PersistentID> reference(Json value, uint64_t version, bool parent) {
    if (value.is_null()) { if(version==1)invalid("Version 1 references must be integers"); return {}; }
    if (version==1 && parent) { const auto id=signed_integer(value); if(id<0)return {}; if(static_cast<uint64_t>(id)>UINT32_MAX)invalid("Reference ID is out of range"); return static_cast<PersistentID>(id); }
    const auto id=integer(value);
    if(version==1 && !parent && id==UINT32_MAX)return {};
    if(id==UINT64_MAX || (version==1 && id>UINT32_MAX))invalid("Reference ID is out of range");
    return id;
}
void read_material(Object json, SceneMaterial& material, bool legacy) {
    if(legacy && !optional(json,"materialEnabled"))return;
    if(legacy) {
        required(json,"baseColorTexture"); required(json,"materialTint"); required(json,"uvScale");
        required(json,"materialMetallic"); required(json,"materialRoughness");
    }
    const auto key=[&](const char* oldKey,const char* newKey){return legacy?oldKey:newKey;};
    read_bool(json,key("materialEnabled","enabled"),material.enabled);
    read_string(json,"baseColorTexture",material.baseColorTexturePath);
    read_string(json,"normalTexture",material.normalTexturePath);
    read_string(json,"metallicRoughnessTexture",material.metalRoughTexturePath);
    read_optional(json,key("materialTint","tint"),material.colorTint,vector<glm::vec4>);
    read_optional(json,"uvScale",material.uvScale,vector<glm::vec2>);
    read_float(json,key("materialMetallic","metallic"),material.metallic);
    read_float(json,key("materialRoughness","roughness"),material.roughness);
    read_bool(json,key("materialDebugChecker","debugChecker"),material.debugChecker);
    read_optional(json,"emissionColor",material.emissionColor,vector<glm::vec3>);
    read_float(json,"emissionStrength",material.emissionStrength);
    read_float(json,"transmission",material.transmission);
    read_float(json,"ior",material.ior);
    if(legacy) {
        material.emissionColor=glm::clamp(material.emissionColor,glm::vec3(0),glm::vec3(10000));
        material.emissionStrength=std::clamp(material.emissionStrength,0.f,10000.f);
        material.transmission=std::clamp(material.transmission,0.f,1.f);
        material.ior=std::clamp(material.ior,1.f,3.f);
    }
}
void read_settings(Object document, uint64_t version, const SceneSettings& defaults, SceneSettings& settings) {
    settings.skybox=defaults.skybox; settings.sunDirection=defaults.sunDirection; settings.shadowsEnabled=defaults.shadowsEnabled;
    settings.shadowRadius=defaults.shadowRadius; settings.shadowDepthBias=defaults.shadowDepthBias; settings.shadowNormalBias=defaults.shadowNormalBias;
    if(const auto item=optional(document,"nextActor")) { const auto next=integer(*item); if(next>UINT32_MAX)invalid("nextActor is out of range"); settings.nextActor=static_cast<uint32_t>(std::max<uint64_t>(next,1)); }
    if(const auto item=optional(document,"lighting")) {
        const auto json=object(*item); read_string(json,"skybox",settings.skybox); read_optional(json,"sunDirection",settings.sunDirection,vector<glm::vec3>);
        read_bool(json,"shadowsEnabled",settings.shadowsEnabled); read_float(json,"shadowRadius",settings.shadowRadius); read_float(json,"shadowDepthBias",settings.shadowDepthBias); read_float(json,"shadowNormalBias",settings.shadowNormalBias);
    }
    if(const auto item=optional(document,"referenceLighting")) {
        const auto json=object(*item); read_optional(json,"sunRadiance",settings.sunRadiance,vector<glm::vec3>);
        read_float(json,"environmentIntensity",settings.environmentIntensity); read_bool(json,"blackEnvironment",settings.blackEnvironment); read_optional(json,"environmentTint",settings.environmentTint,vector<glm::vec3>);
        if(version==1) {
            settings.sunRadiance=glm::clamp(settings.sunRadiance,glm::vec3(0),glm::vec3(10000));
            settings.environmentTint=glm::clamp(settings.environmentTint,glm::vec3(0),glm::vec3(4)); settings.environmentIntensity=std::clamp(settings.environmentIntensity,0.f,10000.f);
        }
    }
    if(const auto item=optional(document,"colorGrading")) {
        const auto json=object(*item); read_float(json,"warmth",settings.warmth); read_float(json,"tint",settings.tint); read_float(json,"giSaturation",settings.giSaturation);
        if(version==1) { settings.warmth=std::clamp(settings.warmth,-1.f,1.f); settings.tint=std::clamp(settings.tint,-1.f,1.f); settings.giSaturation=std::clamp(settings.giSaturation,0.f,1.f); }
    }
    if(const auto item=optional(document,"ssao")) {
        settings.ssao.emplace(); auto& ao=*settings.ssao; const auto json=object(*item);
        read_bool(json,"enabled",ao.enabled); read_float(json,"radius",ao.radius); read_float(json,"bias",ao.bias); read_float(json,"intensity",ao.intensity); read_float(json,"power",ao.power);
    }
}
std::optional<RigidBody> read_components(Object json, uint64_t version, mirabilis::Scene& scene, EntityRef ref) {
    Transform transform{}; Visibility visibility{}; SceneMeshAsset mesh{}; SceneMaterial material{}; Collider collider{}; TimeTrial role{};
    bool portalPlaceable=false, externallyDriven=false; UnknownComponents unknown;
    std::optional<RigidBody> rigidBody;
    if(version==1) {
        transform.position=vector<glm::vec3>(required(json,"position")); transform.rotation=vector<glm::vec3>(required(json,"rotation")); transform.scale=vector<glm::vec3>(required(json,"scale"));
        visibility.visible=boolean(required(json,"visible")); visibility.layer=layer_kind(required(json,"layer"));
        collider.hasCollision=boolean(required(json,"hasCollision")); collider.collisionShape=shape_kind(required(json,"collisionShape"));
        const auto center=optional(json,"colliderCenter"), extents=optional(json,"colliderHalfExtents");
        if(center.has_value()!=extents.has_value())invalid("Collider center and half extents must appear together");
        if(center) { collider.colliderCenter=vector<glm::vec3>(*center); collider.colliderHalfExtents=vector<glm::vec3>(*extents); }
        portalPlaceable=boolean(required(json,"portalPlaceable")); mesh.assetKind=asset_kind(required(json,"asset")); read_string(json,"modelPath",mesh.modelPath);
        if(const auto item=optional(json,"timeTrialRole"))role.role=role_kind(*item);
        read_material(json,material,true);
    } else {
        const auto components=object(required(json,"components"));
        std::set<std::string> componentKeys;
        for(const auto field:components) {
            if(!componentKeys.emplace(field.key).second)invalid("Duplicate component key: "+std::string(field.key));
            if(std::find(std::begin(KnownComponents),std::end(KnownComponents),field.key)==std::end(KnownComponents)) unknown.blocks.emplace(std::string(field.key),simdjson::minify(field.value));
        }
        if(const auto item=optional(components,"transform")) { const auto value=object(*item); transform.position=vector<glm::vec3>(required(value,"position")); transform.rotation=vector<glm::vec3>(required(value,"rotation")); transform.scale=vector<glm::vec3>(required(value,"scale")); }
        if(const auto item=optional(components,"visibility")) { const auto value=object(*item); read_bool(value,"visible",visibility.visible); if(const auto field=optional(value,"layer"))visibility.layer=layer_kind(*field); }
        if(const auto item=optional(components,"mesh")) { const auto value=object(*item); if(const auto field=optional(value,"asset"))mesh.assetKind=asset_kind(*field); read_string(value,"modelPath",mesh.modelPath); }
        if(const auto item=optional(components,"material"))read_material(object(*item),material,false);
        if(const auto item=optional(components,"collider")) { const auto value=object(*item); read_bool(value,"enabled",collider.hasCollision); if(const auto field=optional(value,"shape"))collider.collisionShape=shape_kind(*field); read_optional(value,"center",collider.colliderCenter,vector<glm::vec3>); read_optional(value,"halfExtents",collider.colliderHalfExtents,vector<glm::vec3>); }
        if(const auto item=optional(components,"rigidBody")) {
            rigidBody.emplace(); const auto value=object(*item);
            if(const auto field=optional(value,"motion"))rigidBody->motion=motion_kind(*field);
            read_float(value,"mass",rigidBody->mass); read_float(value,"friction",rigidBody->friction);
            read_float(value,"restitution",rigidBody->restitution); read_float(value,"gravityFactor",rigidBody->gravityFactor);
            if(!valid_component(*rigidBody))invalid("Invalid rigid-body value");
        }
        if(const auto item=optional(components,"timeTrialRole"))role.role=role_kind(required(object(*item),"role"));
        if(const auto item=optional(components,"portalPlaceable")) { if(object(*item).size()!=0)invalid("PortalPlaceable tag must be empty"); portalPlaceable=true; }
        if(const auto item=optional(components,"transformDrivenExternally")) { if(object(*item).size()!=0)invalid("TransformDrivenExternally tag must be empty"); externallyDriven=true; }
    }
    if(!scene.set_local_transform(ref,transform) || !scene.set(ref,visibility) || !scene.set(ref,mesh) || !scene.set(ref,material) || !scene.set(ref,collider) || !scene.set(ref,role) || !scene.set(ref,unknown))invalid("Invalid component value: "+scene.last_error());
    if(portalPlaceable)scene.set(ref,PortalPlaceable{});
    if(externallyDriven)scene.set(ref,TransformDrivenExternally{});
    return rigidBody;
}
std::string quote(std::string_view value) {
    std::ostringstream out; out << '"';
    for(unsigned char ch:value) {
        switch(ch) { case '"':out<<"\\\"";break; case '\\':out<<"\\\\";break; case '\n':out<<"\\n";break; case '\r':out<<"\\r";break; case '\t':out<<"\\t";break;
        default:if(ch<0x20)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<int(ch)<<std::dec;else out<<char(ch); }
    }
    out << '"'; return out.str();
}
template<class T> void write_vector(std::ostream& out,const T& value) { out<<'['; for(int i=0;i<value.length();++i) { if(i)out<<", "; out<<value[i]; } out<<']'; }
void write_reference(std::ostream& out,std::optional<PersistentID> id) { if(id)out<<*id;else out<<"null"; }
bool finite(float value) { return std::isfinite(value); }
bool finite(glm::vec3 value) { return finite(value.x)&&finite(value.y)&&finite(value.z); }
void validate_settings(const SceneSettings& s) {
    if(s.nextActor==0 || s.skybox.empty() || !finite(s.sunDirection) || glm::dot(s.sunDirection,s.sunDirection)<0.000001f || !finite(s.shadowRadius) || s.shadowRadius<=0 || !finite(s.shadowDepthBias) || s.shadowDepthBias<0 || !finite(s.shadowNormalBias) || s.shadowNormalBias<0 || !finite(s.sunRadiance) || glm::any(glm::lessThan(s.sunRadiance,glm::vec3(0))) || !finite(s.environmentIntensity) || s.environmentIntensity<0 || !finite(s.environmentTint) || glm::any(glm::lessThan(s.environmentTint,glm::vec3(0))) || !finite(s.warmth) || std::abs(s.warmth)>1 || !finite(s.tint) || std::abs(s.tint)>1 || !finite(s.giSaturation) || s.giSaturation<0 || s.giSaturation>1)invalid("Invalid scene settings");
    if(s.ssao) { const auto& ao=*s.ssao; if(!finite(ao.radius)||ao.radius<=0||!finite(ao.bias)||ao.bias<0||!finite(ao.intensity)||ao.intensity<0||!finite(ao.power)||ao.power<=0)invalid("Invalid SSAO settings"); }
}
void validate_portal(const mirabilis::Scene& scene,const Portal& portal) {
    if(!portal.placed || !finite(portal.position)||!finite(portal.normal)||!finite(portal.up)||!finite(portal.halfWidth)||portal.halfWidth<=0||!finite(portal.halfHeight)||portal.halfHeight<=0 || glm::length(portal.normal)<0.99f || glm::length(portal.up)<0.99f || glm::length(glm::cross(portal.normal,portal.up))<0.01f)invalid("Invalid authored portal");
    if(portal.hostWallObject) {
        const auto ref=scene.get(*portal.hostWallObject); const auto collider=scene.read<Collider>(ref);
        if(!scene.valid(ref)||scene.pending(ref)||scene.runtime_only(ref)||!scene.has<PortalPlaceable>(ref)||!collider||!collider->hasCollision||collider->collisionShape!=CollisionShape::Box)invalid("Authored portal host is missing or not placeable");
        if(scene.dynamic_ancestor(ref))invalid("Authored portal host is inside a dynamic subtree");
    }
}
}

bool parse_scene_json(std::string_view json,std::string_view sourceFilename,const SceneSettings& defaults,StagedScene& out,std::string& error) {
    try {
        simdjson::dom::parser parser; Json element; const simdjson::padded_string padded(json);
        if(parser.parse(padded).get(element))invalid("Could not parse scene JSON");
        const auto document=object(element); const auto version=integer(required(document,"version")); if(version!=1&&version!=2)invalid("Unsupported scene version");
        StagedScene staging; read_settings(document,version,defaults,staging.settings);
        const auto objects=array(required(document,"objects"));
        PersistentID largest=0; bool anyIds=false;
        for(Json item:objects) { const auto id=integer(required(object(item),"id")); if(id==UINT64_MAX||(version==1&&id>UINT32_MAX))invalid("Object ID is out of range"); largest=std::max(largest,id);anyIds=true; }
        PersistentID next=anyIds?largest+1:0;
        if(version==2)next=std::max(next,integer(required(document,"nextPersistentId")));
        if(next==UINT64_MAX)invalid("Persistent ID counter is exhausted");
        if(!staging.scene.reserve_persistent_ids(next))invalid("Invalid persistent ID counter");
        struct Link { EntityRef ref; std::optional<PersistentID> parent; uint64_t sibling; std::optional<RigidBody> rigidBody; };
        std::vector<Link> links; std::unordered_map<PersistentID,PersistentID> firstIDs; size_t fileIndex=0;
        for(Json item:objects) {
            const auto value=object(item); const auto oldId=integer(required(value,"id"));
            if(version==1 && layer_kind(required(value,"layer"))==RenderLayer::PortalViewOnly) { ++fileIndex; continue; }
            std::string name;
            if(version==1)name=string(required(value,"name"));
            else { const auto components=object(required(value,"components")); if(const auto block=optional(components,"name"))name=string(required(object(*block),"value")); }
            EntityRef ref;
            if(firstIDs.contains(oldId)) { ref=staging.scene.create_object(name); staging.warnings.push_back("Duplicate persistent ID "+std::to_string(oldId)+" repaired as "+std::to_string(ref.persistentId)); }
            else { ref=staging.scene.import_object(name,oldId); firstIDs.emplace(oldId,oldId); }
            if(!ref)invalid("Persistent ID allocation exhausted");
            const auto rigidBody=read_components(value,version,staging.scene,ref);
            if(version==1 && std::filesystem::path(sourceFilename).filename().string().rfind("gi_",0)==0) {
                const auto mesh=staging.scene.read<SceneMeshAsset>(ref); auto collider=*staging.scene.read<Collider>(ref); const auto transform=staging.scene.read<Transform>(ref);
                const bool floor=mesh->assetKind==SceneAssetKind::FloorQuad && collider.collisionShape==CollisionShape::GroundPlane && std::abs(transform->position.y)<=0.01f;
                collider.hasCollision=collider.hasCollision||floor||mesh->assetKind==SceneAssetKind::UnitCube; staging.scene.set(ref,collider);
            }
            links.push_back({ref,reference(required(value,"parent"),version,true),version==2?integer(required(value,"siblingIndex")):fileIndex,rigidBody}); ++fileIndex;
        }
        // Imported duplicates have their own identity; every saved reference binds to the first occurrence.
        for(const auto& link:links) if(link.parent) {
            const auto parent=firstIDs.find(*link.parent); if(parent==firstIDs.end()||!staging.scene.set_parent(link.ref,staging.scene.get(parent->second)))invalid("Invalid or cyclic hierarchy");
        }
        std::stable_sort(links.begin(),links.end(),[](const Link& a,const Link& b){return a.sibling<b.sibling;});
        std::map<std::optional<PersistentID>,size_t> siblingCounts;
        for(const auto& link:links) { const auto parent=staging.scene.read<Hierarchy>(link.ref)->parent; if(!staging.scene.set_sibling_index(link.ref,siblingCounts[parent]++))invalid("Invalid sibling order"); }
        // Capture before detaching so every body retains its original hierarchy pose.
        std::vector<std::pair<EntityRef,Transform>> detached;
        std::set<PersistentID> dynamicBodies;
        for(const auto& link:links)if(link.rigidBody && link.rigidBody->motion==BodyMotion::Dynamic)dynamicBodies.insert(link.ref.persistentId);
        for(const auto ref:staging.scene.hierarchy_order()) {
            if(!dynamicBodies.contains(ref.persistentId) || !staging.scene.read<Hierarchy>(ref)->parent)continue;
            Transform world;
            if(!mirabilis::exact_world_trs(staging.scene.world_matrix(ref),world))invalid("Parented dynamic body has a sheared or invalid world transform: "+std::to_string(ref.persistentId));
            detached.emplace_back(ref,world);
        }
        for(const auto& [ref,world]:detached) {
            if(!staging.scene.set_parent(ref,std::nullopt)||!staging.scene.set_local_transform(ref,world))invalid("Could not detach parented dynamic body: "+staging.scene.last_error());
            staging.warnings.push_back("Parented dynamic body "+std::to_string(ref.persistentId)+" detached to ordered roots, preserving world TRS");
        }
        for(const auto& link:links)if(link.rigidBody && !staging.scene.set(link.ref,*link.rigidBody))invalid("Invalid rigid-body/collider combination: "+staging.scene.last_error());
        if(const auto block=optional(document,"authoredPortals")) {
            std::vector<Portal> portals;
            for(Json item:array(*block)) {
                const auto value=object(item); Portal portal; portal.placed=true;
                portal.hostWallObject=reference(required(value,"hostWallObject"),version,false);
                if(portal.hostWallObject) { const auto host=firstIDs.find(*portal.hostWallObject); if(host==firstIDs.end())invalid("Missing authored portal host"); portal.hostWallObject=host->second; }
                portal.position=vector<glm::vec3>(required(value,"position"));portal.normal=vector<glm::vec3>(required(value,"normal"));portal.up=vector<glm::vec3>(required(value,"up"));
                portal.halfWidth=number(required(value,"halfWidth"));portal.halfHeight=number(required(value,"halfHeight"));
                validate_portal(staging.scene,portal); if(version==1)orient_portal(portal,portal.normal); portals.push_back(portal);
            }
            if(portals.size()%2||portals.size()/2>MaxSavedAuthoredPortalPairs)invalid("Authored portals must be complete pairs within the scene limit");
            for(size_t i=0;i<portals.size();i+=2)staging.authoredPortals.push_back({portals[i],portals[i+1]});
        }
        if(!validate_authored_scene(staging.scene,staging.settings,staging.authoredPortals,error))return false;
        std::vector<PersistentID> hosts;
        for(const auto& pair:staging.authoredPortals) {
            if(pair.first.hostWallObject)hosts.push_back(*pair.first.hostWallObject);
            if(pair.second.hostWallObject)hosts.push_back(*pair.second.hostWallObject);
        }
        staging.scene.set_authored_portal_hosts(std::move(hosts));
        for(const auto ref:staging.scene.hierarchy_order()) {
            const auto collider=staging.scene.read<Collider>(ref);
            const auto dynamic=staging.scene.dynamic_ancestor(ref);
            if(dynamic && dynamic!=ref && (collider->hasCollision || staging.scene.has<RigidBody>(ref)))
                staging.warnings.push_back("Entity "+std::to_string(ref.persistentId)+" is visual-only under a dynamic ancestor; collider/static body settings are retained");
            else if(collider->hasCollision && !dynamic && mirabilis::world_transform_has_shear(staging.scene.world_matrix(ref)))
                staging.warnings.push_back("Static collider "+std::to_string(ref.persistentId)+" has shear; physics uses the best-fit oriented shape");
        }
        out=std::move(staging);error.clear();return true;
    } catch(const std::exception& exception) { error=exception.what();return false; }
}

bool validate_authored_scene(const mirabilis::Scene& scene,const SceneSettings& settings,std::span<const AuthoredPortalPair> portals,std::string& error) {
    try {
        validate_settings(settings); if(scene.next_persistent_id()==UINT64_MAX)invalid("Persistent ID counter is exhausted");
        const auto order=scene.hierarchy_order(); std::set<PersistentID> visited;
        for(EntityRef ref:order) {
            if(!scene.valid(ref)||!visited.insert(ref.persistentId).second)invalid("Invalid hierarchy traversal");
            const auto hierarchy=scene.read<Hierarchy>(ref);if(!hierarchy)invalid("Missing hierarchy");
            if(hierarchy->parent&&!scene.valid(scene.get(*hierarchy->parent)))invalid("Missing parent");
            std::set<PersistentID> children;
            for(PersistentID id:hierarchy->children) { const auto child=scene.get(id); const auto childHierarchy=scene.read<Hierarchy>(child);if(!children.insert(id).second||!childHierarchy||childHierarchy->parent!=ref.persistentId)invalid("Invalid ordered child list"); }
            if(scene.runtime_only(ref))continue;
            if(scene.pending(ref))invalid("Cannot save pending authored entity");
            const auto transform=scene.read<Transform>(ref);const auto visibility=scene.read<Visibility>(ref);const auto mesh=scene.read<SceneMeshAsset>(ref);const auto material=scene.read<SceneMaterial>(ref);const auto collider=scene.read<Collider>(ref);const auto role=scene.read<TimeTrial>(ref);
            if(!scene.read<Name>(ref)||!transform||!visibility||!mesh||!material||!collider||!role||!valid_component(*transform)||!valid_component(*visibility)||!valid_component(*mesh)||!valid_component(*material)||!valid_component(*collider)||!valid_component(*role))invalid("Invalid saved component value");
            const auto body=scene.read<RigidBody>(ref);
            if((body&&!valid_component(*body)) || !scene.validate_physics_edit(ref,body,collider,transform))invalid("Invalid saved physics settings: "+scene.last_error());
            if(ref.persistentId>=scene.next_persistent_id())invalid("Persistent ID counter is below an entity ID");
            if(const auto unknown=scene.read<UnknownComponents>(ref))for(const auto& [key,raw]:unknown->blocks) {
                if(std::find(std::begin(KnownComponents),std::end(KnownComponents),key)!=std::end(KnownComponents))invalid("Unknown component shadows a known component");
                simdjson::dom::parser parser;Json parsed; const simdjson::padded_string padded(raw); if(parser.parse(padded).get(parsed))invalid("Invalid unknown component JSON");
            }
        }
        if(visited.size()!=scene.creation_order().size())invalid("Unreachable entity or hierarchy cycle");
        if(portals.size()>MaxSavedAuthoredPortalPairs)invalid("Too many authored portal pairs");
        for(const auto& pair:portals) { validate_portal(scene,pair.first);validate_portal(scene,pair.second); }
        error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}

bool serialize_scene_json(const mirabilis::Scene& scene,const SceneSettings& settings,std::span<const AuthoredPortalPair> portals,std::string& json,std::string& error) {
    if(!validate_authored_scene(scene,settings,portals,error))return false;
    std::ostringstream out;out<<std::setprecision(std::numeric_limits<float>::max_digits10)<<std::boolalpha;
    out<<"{\n  \"version\": 2,\n  \"nextActor\": "<<settings.nextActor<<",\n  \"nextPersistentId\": "<<scene.next_persistent_id()<<",\n  \"lighting\": {\"skybox\": "<<quote(settings.skybox)<<", \"sunDirection\": ";write_vector(out,settings.sunDirection);
    out<<", \"shadowsEnabled\": "<<settings.shadowsEnabled<<", \"shadowRadius\": "<<settings.shadowRadius<<", \"shadowDepthBias\": "<<settings.shadowDepthBias<<", \"shadowNormalBias\": "<<settings.shadowNormalBias<<"},\n  \"referenceLighting\": {\"sunRadiance\": ";write_vector(out,settings.sunRadiance);
    out<<", \"environmentIntensity\": "<<settings.environmentIntensity<<", \"blackEnvironment\": "<<settings.blackEnvironment<<", \"environmentTint\": ";write_vector(out,settings.environmentTint);out<<"},\n  \"colorGrading\": {\"warmth\": "<<settings.warmth<<", \"tint\": "<<settings.tint<<", \"giSaturation\": "<<settings.giSaturation<<'}';
    if(settings.ssao){const auto& ao=*settings.ssao;out<<",\n  \"ssao\": {\"enabled\": "<<ao.enabled<<", \"radius\": "<<ao.radius<<", \"bias\": "<<ao.bias<<", \"intensity\": "<<ao.intensity<<", \"power\": "<<ao.power<<'}';}
    out<<",\n  \"authoredPortals\": [";bool first=true;
    const auto writePortal=[&](const Portal& p){if(!first)out<<", ";first=false;out<<"{\"hostWallObject\": ";write_reference(out,p.hostWallObject);out<<", \"position\": ";write_vector(out,p.position);out<<", \"normal\": ";write_vector(out,p.normal);out<<", \"up\": ";write_vector(out,p.up);out<<", \"halfWidth\": "<<p.halfWidth<<", \"halfHeight\": "<<p.halfHeight<<'}';};
    for(const auto& pair:portals){writePortal(pair.first);writePortal(pair.second);}out<<"],\n  \"objects\": [";first=true;
    std::map<std::optional<PersistentID>,size_t> siblingCounts;
    for(EntityRef ref:scene.hierarchy_order()) {
        if(scene.runtime_only(ref))continue;
        const auto& hierarchy=*scene.read<Hierarchy>(ref);const auto& transform=*scene.read<Transform>(ref);const auto& visibility=*scene.read<Visibility>(ref);const auto& mesh=*scene.read<SceneMeshAsset>(ref);const auto& material=*scene.read<SceneMaterial>(ref);const auto& collider=*scene.read<Collider>(ref);const auto& role=*scene.read<TimeTrial>(ref);
        if(!first)out<<',';first=false;out<<"\n    {\"id\": "<<ref.persistentId<<", \"parent\": ";write_reference(out,hierarchy.parent);out<<", \"siblingIndex\": "<<siblingCounts[hierarchy.parent]++<<", \"components\": {";
        out<<"\"name\": {\"value\": "<<quote(scene.read<Name>(ref)->value)<<"}, \"transform\": {\"position\": ";write_vector(out,transform.position);out<<", \"rotation\": ";write_vector(out,transform.rotation);out<<", \"scale\": ";write_vector(out,transform.scale);out<<"}, \"visibility\": {\"visible\": "<<visibility.visible<<", \"layer\": "<<static_cast<int>(visibility.layer)<<"}, \"mesh\": {\"asset\": "<<quote(asset_name(mesh.assetKind))<<", \"modelPath\": "<<quote(mesh.modelPath)<<"}, \"material\": {\"enabled\": "<<material.enabled<<", \"baseColorTexture\": "<<quote(material.baseColorTexturePath)<<", \"normalTexture\": "<<quote(material.normalTexturePath)<<", \"metallicRoughnessTexture\": "<<quote(material.metalRoughTexturePath)<<", \"tint\": ";write_vector(out,material.colorTint);
        out<<", \"uvScale\": ";write_vector(out,material.uvScale);out<<", \"metallic\": "<<material.metallic<<", \"roughness\": "<<material.roughness<<", \"debugChecker\": "<<material.debugChecker<<", \"emissionColor\": ";write_vector(out,material.emissionColor);out<<", \"emissionStrength\": "<<material.emissionStrength<<", \"transmission\": "<<material.transmission<<", \"ior\": "<<material.ior<<"}, \"collider\": {\"enabled\": "<<collider.hasCollision<<", \"shape\": "<<static_cast<int>(collider.collisionShape)<<", \"center\": ";write_vector(out,collider.colliderCenter);out<<", \"halfExtents\": ";write_vector(out,collider.colliderHalfExtents);out<<"}, \"timeTrialRole\": {\"role\": "<<quote(role_name(role.role))<<'}';
        if(scene.has<PortalPlaceable>(ref))out<<", \"portalPlaceable\": {}";
        if(const auto body=scene.read<RigidBody>(ref))out<<", \"rigidBody\": {\"motion\": "<<quote(body->motion==BodyMotion::Dynamic?"Dynamic":"Static")<<", \"mass\": "<<body->mass<<", \"friction\": "<<body->friction<<", \"restitution\": "<<body->restitution<<", \"gravityFactor\": "<<body->gravityFactor<<'}';
        if(scene.has<TransformDrivenExternally>(ref))out<<", \"transformDrivenExternally\": {}";
        if(const auto unknown=scene.read<UnknownComponents>(ref))for(const auto& [key,value]:unknown->blocks)out<<", "<<quote(key)<<": "<<value;
        out<<"}}";
    }
    out<<"\n  ]\n}\n";if(!out){error="Could not serialize scene";return false;}
    auto result=out.str();
    simdjson::dom::parser parser;Json parsed;const simdjson::padded_string padded(result);
    if(parser.parse(padded).get(parsed)){error="Serialized scene contains invalid JSON or UTF-8";return false;}
    json=std::move(result);error.clear();return true;
}

bool save_scene_atomic(const std::filesystem::path& destination,const mirabilis::Scene& scene,const SceneSettings& settings,std::span<const AuthoredPortalPair> portals,std::string& error) {
    std::string json;if(!serialize_scene_json(scene,settings,portals,json,error))return false;
    static std::atomic<uint64_t> sequence{0};auto temporary=destination;temporary+=std::string(".tmp-")+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++);
    std::error_code code;
    const auto cleanup=[&]{std::error_code ignored;std::filesystem::remove(temporary,ignored);};
    try {
        if(!destination.parent_path().empty())std::filesystem::create_directories(destination.parent_path(),code);
        if(code){error="Could not create scene directory: "+code.message();return false;}
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        if(!file){error="Could not open temporary scene file";return false;}
        file.write(json.data(),static_cast<std::streamsize>(json.size()));file.flush();const bool written=static_cast<bool>(file);file.close();
        if(!written||file.fail()){cleanup();error="Could not write or close temporary scene file";return false;}
#ifdef _WIN32
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){const auto winError=GetLastError();cleanup();error="Could not replace scene file: "+std::to_string(winError);return false;}
#else
        std::filesystem::rename(temporary,destination,code);if(code){cleanup();error="Could not replace scene file: "+code.message();return false;}
#endif
        error.clear();return true;
    }catch(const std::exception& exception){cleanup();error=exception.what();return false;}
}
