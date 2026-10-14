#include "physics_world.h"
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
JPH::Vec3 jvec(glm::vec3 v) { return {v.x,v.y,v.z}; }
glm::vec3 gvec(JPH::Vec3 v) { return {v.GetX(),v.GetY(),v.GetZ()}; }
JPH::Quat jquat(glm::quat q) { return {q.x,q.y,q.z,q.w}; }
glm::quat gquat(JPH::Quat q) { return {q.GetW(),q.GetX(),q.GetY(),q.GetZ()}; }
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool same_matrix(const glm::mat4& a, const glm::mat4& b) {
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(std::abs(a[c][r]-b[c][r])>1e-5f) return false;
    return true;
}
struct JoltRuntime {
    JoltRuntime() { JPH::RegisterDefaultAllocator(); JPH::Factory::sInstance = new JPH::Factory; JPH::RegisterTypes(); }
    ~JoltRuntime() { JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance=nullptr; }
};
struct Layers final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer(layer); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer.GetValue()==0?"Static":"Dynamic"; }
#endif
};
struct PairFilter final : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b) const override { return a==1 || b==1; }
};
struct BroadFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b) const override { return a==1 || b.GetValue()==1; }
};
glm::quat rotation(const Transform& t) {
    Transform r=t; r.position=glm::vec3(0); r.scale=glm::vec3(1); return glm::quat_cast(to_matrix(r));
}
bool same_settings(const RigidBody& a,const RigidBody& b) {
    return a.motion==b.motion && a.mass==b.mass && a.friction==b.friction && a.restitution==b.restitution && a.gravityFactor==b.gravityFactor;
}
bool same_collider(const Collider& a,const Collider& b) {
    return a.hasCollision==b.hasCollision && a.collisionShape==b.collisionShape && a.colliderCenter==b.colliderCenter && a.colliderHalfExtents==b.colliderHalfExtents;
}
}
namespace mirabilis {
bool world_transform_has_shear(const glm::mat4& world) {
    glm::vec3 x(world[0]),y(world[1]),z(world[2]);
    if(glm::length(glm::dvec3(x))==0 || glm::length(glm::dvec3(y))==0 || glm::length(glm::dvec3(z))==0) return true;
    x=glm::vec3(glm::normalize(glm::dvec3(x))); y=glm::vec3(glm::normalize(glm::dvec3(y))); z=glm::vec3(glm::normalize(glm::dvec3(z)));
    return std::abs(glm::dot(x,y))>1e-4f || std::abs(glm::dot(x,z))>1e-4f || std::abs(glm::dot(y,z))>1e-4f;
}
bool exact_world_trs(const glm::mat4& world,Transform& out) {
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(world[c][r])) return false;
    if(world_transform_has_shear(world)) return false;
    glm::mat4 unmirrored=world;
    const bool mirrored=glm::determinant(glm::mat3(world))<0;
    if(mirrored) unmirrored[0]=-unmirrored[0];
    glm::vec3 scale;
    for(int c=0;c<3;++c) {
        scale[c]=static_cast<float>(glm::length(glm::dvec3(unmirrored[c])));
        if(!std::isfinite(scale[c]) || scale[c]==0) return false;
        unmirrored[c]/=scale[c];
    }
    out=transform_from_matrix(unmirrored);
    out.scale=scale;
    if(mirrored) out.scale.x=-out.scale.x;
    return valid_component(out) && same_matrix(world,to_matrix(out));
}
struct PhysicsWorld::Impl {
    Layers layers; PairFilter pairs; BroadFilter broad;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl temporary{16*1024*1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    struct Body {
        EntityRef ref; JPH::BodyID id; Collider collider; RigidBody settings; glm::vec3 scale;
        glm::vec3 previousPosition,currentPosition; glm::quat previousRotation,currentRotation;
        glm::mat4 lastWorld; bool sleeping{false};
    };
    std::unordered_map<PersistentID,Body> bodies;
    std::unordered_set<PersistentID> resetIds;
    std::vector<std::string> diagnostics;
    uint64_t epoch{0}; bool stepping{false}; bool editorMode{true};
    Impl() { system.Init(65536,0,65536,10240,layers,broad,pairs); system.SetGravity({0,-9.81f,0}); }
    void warn(std::string message) {
        if(std::find(diagnostics.begin(),diagnostics.end(),message)==diagnostics.end()) {
            std::cerr<<"Physics: "<<message<<'\n'; diagnostics.push_back(std::move(message));
        }
    }
    void reset_subtree(const Scene& scene,EntityRef root) {
        for(auto ref:scene.hierarchy_order()) {
            auto ancestor=ref;
            while(scene.valid(ancestor)) {
                if(ancestor.persistentId==root.persistentId) { resetIds.insert(ref.persistentId); break; }
                const auto& h=*scene.read<Hierarchy>(ancestor);
                if(!h.parent) break;
                ancestor=scene.get(*h.parent);
            }
        }
    }
    JPH::ShapeRefC shape(const Collider& c,glm::vec3 scale) {
        JPH::ShapeSettings::ShapeResult result;
        glm::vec3 offset(0);
        if(c.collisionShape==CollisionShape::SurfRamp) {
            JPH::Array<JPH::Vec3> points;
            for(float x:{-.5f,.5f}) {
                points.push_back(jvec(glm::vec3(x,0,-.5f)*scale));
                points.push_back(jvec(glm::vec3(x,0,.5f)*scale));
                points.push_back(jvec(glm::vec3(x,1,.5f)*scale));
            }
            JPH::ConvexHullShapeSettings settings(points); settings.mMaxConvexRadius=0;
            result=settings.Create();
        } else {
            glm::vec3 half;
            if(c.collisionShape==CollisionShape::GroundPlane) {
                half={std::abs(scale.x)*.5f,.025f,std::abs(scale.z)*.5f}; offset.y=-half.y;
            } else { half=glm::abs(scale)*c.colliderHalfExtents; offset=scale*c.colliderCenter; }
            JPH::BoxShapeSettings settings(jvec(half),0); result=settings.Create();
        }
        if(result.HasError()) throw std::runtime_error(result.GetError().c_str());
        if(offset!=glm::vec3(0)) {
            JPH::RotatedTranslatedShapeSettings shifted(jvec(offset),JPH::Quat::sIdentity(),result.Get());
            result=shifted.Create(); if(result.HasError()) throw std::runtime_error(result.GetError().c_str());
        }
        return result.Get();
    }
    Body* find(EntityRef ref) {
        auto it=bodies.find(ref.persistentId); return it!=bodies.end() && it->second.ref==ref?&it->second:nullptr;
    }
    const Body* find(EntityRef ref) const {
        auto it=bodies.find(ref.persistentId); return it!=bodies.end() && it->second.ref==ref?&it->second:nullptr;
    }
};
PhysicsWorld::PhysicsWorld() { static JoltRuntime runtime; _impl=std::make_unique<Impl>(); }
PhysicsWorld::~PhysicsWorld() { clear(); }
void PhysicsWorld::remove(EntityRef ref) {
    if(_impl->stepping) { _impl->warn("Body removal refused during a physics step; drain deferred destruction after the step"); return; }
    if(auto* body=_impl->find(ref)) {
        auto& api=_impl->system.GetBodyInterface(); api.RemoveBody(body->id); api.DestroyBody(body->id); _impl->bodies.erase(ref.persistentId);
    }
}
void PhysicsWorld::clear() {
    if(_impl->stepping) throw std::logic_error("Body teardown during a physics step");
    auto& api=_impl->system.GetBodyInterface();
    for(const auto& [id,body]:_impl->bodies) { (void)id; api.RemoveBody(body.id); api.DestroyBody(body.id); }
    _impl->bodies.clear(); _impl->resetIds.clear(); _impl->epoch=0;
}
size_t PhysicsWorld::body_count() const { return _impl->bodies.size(); }
bool PhysicsWorld::has_body(EntityRef ref) const { return _impl->find(ref)!=nullptr; }
bool PhysicsWorld::sleeping(EntityRef ref) const { const auto* body=_impl->find(ref); return body && body->sleeping; }
const std::vector<std::string>& PhysicsWorld::warnings() const { return _impl->diagnostics; }
bool PhysicsWorld::synchronize(Scene& scene,bool editorMode) {
    if(_impl->epoch!=scene.epoch()) { clear(); _impl->epoch=scene.epoch(); }
    _impl->editorMode=editorMode;
    scene.set_physics_play(!editorMode);
    scene.set_teleport_handler([this,&scene](EntityRef ref,const Transform& transform) {return queue_teleport(scene,ref,transform);});
    auto& api=_impl->system.GetBodyInterface();
    std::vector<EntityRef> retired;
    for(const auto& [id,body]:_impl->bodies) {
        (void)id; const auto* collider=scene.read<Collider>(body.ref);
        const auto ancestor=scene.dynamic_ancestor(body.ref);
        if(!scene.valid(body.ref) || !collider || !collider->hasCollision || (ancestor && ancestor!=body.ref)) retired.push_back(body.ref);
    }
    for(auto ref:retired) { remove(ref); if(scene.valid(ref) && !scene.pending(ref)) scene.remove<PhysicsState>(ref,true); }
    bool success=true;
    for(auto ref:scene.hierarchy_order()) {
        if(scene.pending(ref)) continue;
        const auto* collider=scene.read<Collider>(ref);
        const auto* saved=scene.read<RigidBody>(ref);
        const bool dynamic=saved && saved->motion==BodyMotion::Dynamic;
        const auto ancestor=scene.dynamic_ancestor(ref);
        if(ancestor && ancestor!=ref) {
            if(collider && collider->hasCollision) _impl->warn("Entity "+std::to_string(ref.persistentId)+" collider is visual-only under a Dynamic ancestor");
            continue;
        }
        if(dynamic && !editorMode) scene.set(ref,TransformDrivenExternally{},true);
        else if(auto* old=_impl->find(ref); old && old->settings.motion==BodyMotion::Dynamic) scene.remove<TransformDrivenExternally>(ref,true);
        if(!collider || !collider->hasCollision) continue;
        const RigidBody settings=saved?*saved:RigidBody{};
        const auto world=scene.world_matrix(ref);
        Transform pose;
        const bool exact=exact_world_trs(world,pose);
        if(dynamic && exact) pose=*scene.read<Transform>(ref);
        if(!exact) {
            if(dynamic) { _impl->warn("Dynamic entity "+std::to_string(ref.persistentId)+" has unrepresentable world TRS"); success=false; continue; }
            pose=transform_from_matrix(world);
            _impl->warn("Static entity "+std::to_string(ref.persistentId)+" shear uses a best-fit oriented collider");
        }
        if(!scene.validate_physics_edit(ref,saved,collider,scene.read<Transform>(ref)) ||
            glm::any(glm::lessThanEqual(glm::abs(pose.scale),glm::vec3(0)))) {success=false;continue;}
        auto* old=_impl->find(ref);
        const bool rebuild=!old || !same_collider(old->collider,*collider) || !same_settings(old->settings,settings) || old->scale!=pose.scale;
        try {
            if(rebuild) {
                const auto preservedLinear = old && dynamic ? api.GetLinearVelocity(old->id) : JPH::Vec3::sZero();
                const auto preservedAngular = old && dynamic ? api.GetAngularVelocity(old->id) : JPH::Vec3::sZero();
                JPH::ShapeRefC shape=_impl->shape(*collider,pose.scale);
                JPH::BodyCreationSettings creation(shape,jvec(pose.position),jquat(rotation(pose)),
                    dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,dynamic?1:0);
                creation.mLinearDamping=0; creation.mAngularDamping=0;
                creation.mFriction=settings.friction; creation.mRestitution=settings.restitution; creation.mGravityFactor=settings.gravityFactor;
                if(dynamic) {creation.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;creation.mMassPropertiesOverride.mMass=settings.mass;}
                const auto id=api.CreateAndAddBody(creation,dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
                if(id.IsInvalid()) throw std::runtime_error("Jolt body capacity exhausted");
                if(old) remove(ref);
                if(dynamic) { api.SetLinearVelocity(id,preservedLinear); api.SetAngularVelocity(id,preservedAngular); }
                const auto q=rotation(pose);
                _impl->bodies.emplace(ref.persistentId,Impl::Body{ref,id,*collider,settings,pose.scale,pose.position,pose.position,q,q,world,false});
                _impl->reset_subtree(scene,ref);
                old=_impl->find(ref);
            } else if(!same_matrix(world,old->lastWorld)) {
                api.SetPositionAndRotation(old->id,jvec(pose.position),jquat(rotation(pose)),dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
                old->currentPosition=old->previousPosition=pose.position;
                old->currentRotation=old->previousRotation=rotation(pose);
                old->lastWorld=world; _impl->reset_subtree(scene,ref);
            }
            old->sleeping=dynamic && !api.IsActive(old->id);
            scene.set(ref,PhysicsState{old->sleeping},true);
        } catch(const std::exception& error) { _impl->warn("Entity "+std::to_string(ref.persistentId)+": "+error.what()); success=false; }
    }
    scene.consume_transform_dirty();
    return success;
}
void PhysicsWorld::step(Scene& scene,float dt,const std::function<void()>& duringStep) {
    if(!std::isfinite(dt) || dt<=0) throw std::invalid_argument("Invalid physics step");
    for(auto& [id,body]:_impl->bodies) { (void)id; body.previousPosition=body.currentPosition; body.previousRotation=body.currentRotation; }
    _impl->stepping=true;
    try {
        const auto result=_impl->system.Update(dt,1,&_impl->temporary,&_impl->jobs);
        if(result!=JPH::EPhysicsUpdateError::None) throw std::runtime_error("Jolt step capacity exceeded");
        if(duringStep) duringStep();
    } catch(...) { _impl->stepping=false; throw; }
    _impl->stepping=false;
    auto& api=_impl->system.GetBodyInterface();
    for(auto& [id,body]:_impl->bodies) {
        (void)id;
        if(body.settings.motion!=BodyMotion::Dynamic || !scene.valid(body.ref) || scene.pending(body.ref)) continue;
        body.currentPosition=gvec(api.GetPosition(body.id)); body.currentRotation=gquat(api.GetRotation(body.id));
        body.sleeping=!api.IsActive(body.id);
        if(body.sleeping) {body.previousPosition=body.currentPosition;body.previousRotation=body.currentRotation;}
        const auto matrix=glm::translate(glm::mat4(1),body.currentPosition)*glm::mat4_cast(body.currentRotation);
        Transform pose=transform_from_matrix(matrix);
        pose.scale=body.scale;
        if(!valid_component(pose)) throw std::runtime_error("Invalid Jolt transform");
        scene.set_local_transform(body.ref,pose,true); scene.set(body.ref,PhysicsState{body.sleeping},true);
        body.lastWorld=scene.world_matrix(body.ref);
    }
}
std::optional<glm::mat4> PhysicsWorld::body_world(EntityRef ref) const {
    const auto* body=_impl->find(ref); if(!body)return std::nullopt;
    const auto& api=_impl->system.GetBodyInterface();
    return glm::translate(glm::mat4(1),gvec(api.GetPosition(body->id)))*glm::mat4_cast(gquat(api.GetRotation(body->id)))*glm::scale(glm::mat4(1),body->scale);
}
std::optional<AABB> PhysicsWorld::body_bounds(EntityRef ref) const {
    const auto* body=_impl->find(ref); if(!body)return std::nullopt;
    JPH::BodyLockRead lock(_impl->system.GetBodyLockInterface(),body->id); if(!lock.Succeeded())return std::nullopt;
    const auto bounds=lock.GetBody().GetWorldSpaceBounds(); return AABB{gvec(bounds.mMin),gvec(bounds.mMax)};
}
glm::vec3 PhysicsWorld::velocity(EntityRef ref) const {const auto* b=_impl->find(ref);return b?gvec(_impl->system.GetBodyInterface().GetLinearVelocity(b->id)):glm::vec3(0);}
bool PhysicsWorld::ensure_body(Scene& scene,EntityRef ref) {
    const auto* settings=scene.read<RigidBody>(ref);
    if(!settings || settings->motion!=BodyMotion::Dynamic) return false;
    const auto* body=_impl->find(ref);
    return (body && body->settings.motion==BodyMotion::Dynamic) ||
        (synchronize(scene,_impl->editorMode) && has_body(ref));
}
bool PhysicsWorld::queue_teleport(Scene& scene,EntityRef ref,const Transform& pose) {
    const auto* body=scene.read<RigidBody>(ref);
    if(!scene.mutable_ref(ref) || !body || body->motion!=BodyMotion::Dynamic || !valid_component(pose) ||
        !scene.validate_physics_edit(ref,body,scene.read<Collider>(ref),&pose)) return false;
    return scene.enqueue_command(ref,[this,pose](Scene& target,EntityRef entity) {
        if(!ensure_body(target,entity))return;
        auto* b=_impl->find(entity); if(!b)return;
        auto& api=_impl->system.GetBodyInterface();
        // Shape scaling follows the same dirty reconciliation as authoring edits.
        target.set_local_transform(entity,pose,true);
        if(glm::any(glm::greaterThan(glm::abs(b->scale-pose.scale),glm::vec3(1e-5f)))) {synchronize(target,false);b=_impl->find(entity);if(!b)return;}
        api.SetPositionAndRotation(b->id,jvec(pose.position),jquat(rotation(pose)),JPH::EActivation::Activate);
        b->previousPosition=b->currentPosition=pose.position; b->previousRotation=b->currentRotation=rotation(pose);
        b->lastWorld=target.world_matrix(entity);b->sleeping=false;target.set(entity,PhysicsState{false},true);
        _impl->reset_subtree(target,entity);
    });
}
bool PhysicsWorld::queue_velocity(Scene& scene,EntityRef ref,glm::vec3 value) {
    const auto* b=scene.read<RigidBody>(ref); if(!scene.mutable_ref(ref) || !b || b->motion!=BodyMotion::Dynamic || !finite(value))return false;
    return scene.enqueue_command(ref,[this,value](Scene& target,EntityRef entity) { if(!ensure_body(target,entity))return; if(auto* body=_impl->find(entity)) {
        auto& api=_impl->system.GetBodyInterface();api.SetLinearVelocity(body->id,jvec(value));api.ActivateBody(body->id);
        body->sleeping=false;target.set(entity,PhysicsState{false},true);
    }});
}
bool PhysicsWorld::queue_impulse(Scene& scene,EntityRef ref,glm::vec3 value) {
    const auto* b=scene.read<RigidBody>(ref);if(!scene.mutable_ref(ref) || !b || b->motion!=BodyMotion::Dynamic || !finite(value))return false;
    return scene.enqueue_command(ref,[this,value](Scene& target,EntityRef entity) {if(!ensure_body(target,entity))return; if(auto* body=_impl->find(entity)) {
        _impl->system.GetBodyInterface().AddImpulse(body->id,jvec(value));body->sleeping=false;target.set(entity,PhysicsState{false},true);
    }});
}
bool PhysicsWorld::queue_force(Scene& scene,EntityRef ref,glm::vec3 value) {
    const auto* b=scene.read<RigidBody>(ref);if(!scene.mutable_ref(ref) || !b || b->motion!=BodyMotion::Dynamic || !finite(value))return false;
    return scene.enqueue_command(ref,[this,value](Scene& target,EntityRef entity) {if(!ensure_body(target,entity))return; if(auto* body=_impl->find(entity)) {
        _impl->system.GetBodyInterface().AddForce(body->id,jvec(value));body->sleeping=false;target.set(entity,PhysicsState{false},true);
    }});
}
glm::mat4 PhysicsWorld::render_world(const Scene& scene,EntityRef ref,float alpha) const {
    const auto ancestor=scene.dynamic_ancestor(ref);const auto* body=_impl->find(ancestor);
    if(!body)return scene.world_matrix(ref);
    alpha=std::clamp(alpha,0.f,1.f);
    auto world=glm::translate(glm::mat4(1),glm::mix(body->previousPosition,body->currentPosition,alpha))*
        glm::mat4_cast(glm::slerp(body->previousRotation,body->currentRotation,alpha))*glm::scale(glm::mat4(1),body->scale);
    glm::mat4 local(1);auto child=ref;
    while(child!=ancestor && scene.valid(child)) {
        local=scene.local_matrix(child)*local;
        const auto& h=*scene.read<Hierarchy>(child);if(!h.parent)break;child=scene.get(*h.parent);
    }
    return world*local;
}
std::vector<PersistentID> PhysicsWorld::consume_history_resets() {
    std::vector<PersistentID> result(_impl->resetIds.begin(),_impl->resetIds.end());_impl->resetIds.clear();std::sort(result.begin(),result.end());return result;
}
}
