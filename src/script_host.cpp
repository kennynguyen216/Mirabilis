#include "script_host.h"
#include "scene_serialization.h"
#include <algorithm>
#include <climits>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>
#endif

namespace mirabilis {
using namespace scripting;
void ScriptInput::event(Key key,bool down,bool repeat) {
    if (key >= Key::Count || repeat) return;
    const uint64_t bit = uint64_t{1} << static_cast<uint32_t>(key);
    if (down && !(_down & bit)) { _framePressed |= bit; _fixedPressed |= bit; }
    if (!down && (_down & bit)) { _frameReleased |= bit; _fixedReleased |= bit; }
    if (down) _down |= bit; else _down &= ~bit;
}
void ScriptInput::focus_lost() {
    _framePressed = _fixedPressed = 0;
    _frameReleased |= _down; _fixedReleased |= _down; _down = 0;
}
void ScriptInput::clear() { _down = _framePressed = _frameReleased = _fixedPressed = _fixedReleased = 0; }
void ScriptInput::finish_frame() { _framePressed = _frameReleased = 0; }
void ScriptInput::finish_fixed_tick() { _fixedPressed = _fixedReleased = 0; }
bool ScriptInput::query(Key key,InputQuery query,bool fixed) const {
    if (key >= Key::Count || query > InputQuery::Released) return false;
    const auto mask = query == InputQuery::Down ? _down : query == InputQuery::Pressed ?
        (fixed ? _fixedPressed : _framePressed) : (fixed ? _fixedReleased : _frameReleased);
    return (mask & (uint64_t{1} << static_cast<uint32_t>(key))) != 0;
}
namespace {
Entity bridge_entity(EntityRef ref) { return {ref.epoch,entt::to_integral(ref.entity),0,ref.persistentId}; }
EntityRef native_entity(Entity ref) { return {ref.epoch,static_cast<entt::entity>(ref.entity),ref.persistentId}; }
Utf8 utf8(const std::string& text) { return {reinterpret_cast<const uint8_t*>(text.data()),static_cast<uint32_t>(text.size())}; }
bool valid_utf8_argument(Utf8 value) {
    if (!value.size) return true;
    if (!value.data || value.size > INT_MAX) return false;
#ifdef _WIN32
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(value.data),static_cast<int>(value.size),nullptr,0)>0;
#else
    return true;
#endif
}
std::string owned(Utf8 value) { return value.size ? std::string(reinterpret_cast<const char*>(value.data),value.size) : std::string{}; }
Vec3 bridge_vec(glm::vec3 value) { return {value.x,value.y,value.z}; }
glm::vec3 native_vec(Vec3 value) { return {value.x,value.y,value.z}; }
Status copy_bytes(const std::string& value,uint8_t* output,uint32_t capacity,uint32_t* required) {
    if (!required || (!output && capacity)) return Status::InvalidArgument;
    *required = static_cast<uint32_t>(value.size());
    if (capacity < value.size()) return Status::BufferTooSmall;
    if (!value.empty()) std::copy(value.begin(),value.end(),output);
    return Status::Ok;
}
}
struct ScriptHost::Impl {
    Scene* scene{};
    PhysicsWorld* physics{};
    std::thread::id mainThread{std::this_thread::get_id()};
    ScriptInput input;
    NativeApi native{};
    ManagedApi managed{};
    std::filesystem::path directory;
    std::string error;
    std::vector<std::string> diagnostics;
    size_t diagnosticBytes{};
    std::optional<Script> metadata;
    std::vector<std::string> metadataDiagnostics;
    bool ready{},attempted{},running{},callback{},fixed{},stopping{};
    struct Instance { EntityRef ref; uint64_t handle{}; std::string assembly,type; bool faulted{}; };
    std::unordered_map<PersistentID,Instance> instances;

    void report(std::string message) {
        std::cerr << "Scripting: " << message << '\n';
        constexpr size_t maxEntries=256, maxBytes=1024*1024, maxEntryBytes=64*1024;
        constexpr std::string_view suffix=" [truncated]";
        if (message.size()>maxEntryBytes) {
            size_t cut=maxEntryBytes-suffix.size();
            while (cut && (static_cast<unsigned char>(message[cut]) & 0xc0)==0x80) --cut;
            std::string truncated(message,0,cut); truncated.append(suffix); message.swap(truncated);
        }
        while (!diagnostics.empty() && (diagnostics.size()>=maxEntries || diagnosticBytes+message.size()>maxBytes)) {
            diagnosticBytes-=diagnostics.front().size(); diagnostics.erase(diagnostics.begin());
        }
        diagnosticBytes+=message.size(); diagnostics.push_back(std::move(message));
    }
    Status access() const {
        if (std::this_thread::get_id() != mainThread) return Status::WrongThread;
        if (!callback || !scene || !physics) return Status::OutsideCallback;
        return Status::Ok;
    }
    Status entity_access(Entity entity,bool mutation) const {
        auto status = access(); if (status != Status::Ok) return status;
        if (entity.reserved) return Status::InvalidArgument;
        const auto ref = native_entity(entity);
        if (!scene->valid(ref)) return Status::InvalidEntity;
        if (mutation && scene->pending(ref)) return Status::PendingDestroy;
        if (mutation && !scene->mutable_ref(ref)) return Status::InvalidArgument;
        return Status::Ok;
    }
    template<class F> static Status guarded(void* context,F&& action) noexcept {
        if (!context) return Status::InvalidArgument;
        try { return action(*static_cast<Impl*>(context)); }
        catch (...) { return Status::Failure; }
    }
    struct Scope {
        Impl& host; bool oldCallback,oldFixed;
        Scope(Impl& value,bool fixed = false):host(value),oldCallback(value.callback),oldFixed(value.fixed) { host.callback=true; host.fixed=fixed; }
        ~Scope() { host.callback=oldCallback; host.fixed=oldFixed; }
    };
    Impl() {
        native.version=AbiVersion; native.size=sizeof(NativeApi); native.context=this;
        native.is_valid=[](void* p,Entity e,uint8_t* result)->Status { return guarded(p,[&](Impl& h) {
            if (!result) return Status::InvalidArgument; *result=0;
            const auto status=h.entity_access(e,false); if (status==Status::Ok) *result=1; return status;
        }); };
        native.is_pending=[](void* p,Entity e,uint8_t* result)->Status { return guarded(p,[&](Impl& h) {
            if (!result) return Status::InvalidArgument; *result=0;
            const auto status=h.entity_access(e,false); if (status!=Status::Ok) return status;
            *result=h.scene->pending(native_entity(e)) ? 1 : 0; return Status::Ok;
        }); };
        native.resolve=[](void* p,uint64_t id,Entity* result)->Status { return guarded(p,[&](Impl& h) {
            if (!result) return Status::InvalidArgument; *result={};
            const auto status=h.access(); if (status!=Status::Ok) return status;
            const auto ref=h.scene->get(id); if (!h.scene->valid(ref)) return Status::InvalidEntity;
            *result=bridge_entity(ref); return Status::Ok;
        }); };
        native.get_transform=[](void* p,Entity e,uint8_t world,Trs* result)->Status { return guarded(p,[&](Impl& h) {
            if (!result || world>1) return Status::InvalidArgument;
            const auto status=h.entity_access(e,false); if (status!=Status::Ok) return status;
            const auto ref=native_entity(e);
            const auto transform=world ? transform_from_matrix(h.scene->world_matrix(ref)) : *h.scene->read<Transform>(ref);
            *result={bridge_vec(transform.position),bridge_vec(transform.rotation),bridge_vec(transform.scale)}; return Status::Ok;
        }); };
        native.set_transform=[](void* p,Entity e,uint8_t world,const Trs* value)->Status { return guarded(p,[&](Impl& h) {
            if (!value || world>1) return Status::InvalidArgument;
            const auto status=h.entity_access(e,true); if (status!=Status::Ok) return status;
            const Transform transform{native_vec(value->position),native_vec(value->rotation),native_vec(value->scale)};
            if (!valid_component(transform)) return Status::InvalidArgument;
            const auto ref=native_entity(e);
            const bool ok=world ? h.scene->set_world_transform(ref,to_matrix(transform)) : h.scene->set_local_transform(ref,transform);
            return ok ? Status::Ok : Status::InvalidArgument;
        }); };
        native.get_name=[](void* p,Entity e,uint8_t* output,uint32_t capacity,uint32_t* required)->Status { return guarded(p,[&](Impl& h) {
            const auto status=h.entity_access(e,false); if (status!=Status::Ok) return status;
            return copy_bytes(h.scene->read<Name>(native_entity(e))->value,output,capacity,required);
        }); };
        native.set_name=[](void* p,Entity e,Utf8 value)->Status { return guarded(p,[&](Impl& h) {
            if (!valid_utf8_argument(value)) return Status::InvalidArgument;
            const auto status=h.entity_access(e,true); if (status!=Status::Ok) return status;
            return h.scene->set<Name>(native_entity(e),Name{owned(value)}) ? Status::Ok : Status::InvalidArgument;
        }); };
        native.input=[](void* p,uint32_t key,uint32_t query,uint8_t* result)->Status { return guarded(p,[&](Impl& h) {
            if (!result) return Status::InvalidArgument; *result=0;
            const auto status=h.access(); if (status!=Status::Ok) return status;
            if (key>=static_cast<uint32_t>(Key::Count) || query>static_cast<uint32_t>(InputQuery::Released)) return Status::InvalidArgument;
            *result=h.input.query(static_cast<Key>(key),static_cast<InputQuery>(query),h.fixed) ? 1 : 0; return Status::Ok;
        }); };
        native.log=[](void* p,Utf8 value)->Status { return guarded(p,[&](Impl& h) {
            if (std::this_thread::get_id()!=h.mainThread) return Status::WrongThread;
            if (!valid_utf8_argument(value)) return Status::InvalidArgument;
            h.report(owned(value)); return Status::Ok;
        }); };
        native.body_command=[](void* p,Entity e,uint32_t command,Vec3 value)->Status { return guarded(p,[&](Impl& h) {
            const auto status=h.entity_access(e,true); if (status!=Status::Ok) return status;
            const auto ref=native_entity(e); const auto v=native_vec(value);
            bool ok{};
            switch (static_cast<BodyCommand>(command)) {
                case BodyCommand::Force: ok=h.physics->queue_force(*h.scene,ref,v); break;
                case BodyCommand::Impulse: ok=h.physics->queue_impulse(*h.scene,ref,v); break;
                case BodyCommand::Velocity: ok=h.physics->queue_velocity(*h.scene,ref,v); break;
                default: return Status::InvalidArgument;
            }
            return ok ? Status::Ok : Status::InvalidArgument;
        }); };
        native.destroy_entity=[](void* p,Entity e)->Status { return guarded(p,[&](Impl& h) {
            const auto status=h.entity_access(e,false); if (status!=Status::Ok) return status;
            return h.scene->destroy_object(native_entity(e)) ? Status::Ok : Status::InvalidArgument;
        }); };
    }
    std::string read_buffer(const std::function<Status(uint8_t*,uint32_t,uint32_t*)>& getter) {
        uint32_t required{};
        auto status=getter(nullptr,0,&required);
        if (status!=Status::BufferTooSmall && status!=Status::Ok) throw std::runtime_error("Managed metadata/field getter failed: " + std::to_string(static_cast<uint32_t>(status)));
        std::string result(required,'\0');
        status=getter(reinterpret_cast<uint8_t*>(result.data()),required,&required);
        if (status!=Status::Ok || required>result.size()) throw std::runtime_error("Managed field buffer contract failed");
        result.resize(required); return result;
    }
    void sync_fields(const Instance& instance) {
        if (!instance.handle || !scene->valid(instance.ref) || scene->pending(instance.ref)) return;
        auto* component=scene->edit<Script>(instance.ref,true); if (!component) return;
        const auto json=read_buffer([&](uint8_t* p,uint32_t c,uint32_t* n) { return managed.read_fields(instance.handle,p,c,n); });
        std::map<std::string,ScriptField> fields; std::string message;
        if (!parse_script_fields(json,fields,message)) { report(message); return; }
        for (auto& [name,field] : fields) {
            const auto saved=component->fields.find(name);
            if (saved==component->fields.end() || saved->second.type==field.type) component->fields[name]=std::move(field);
        }
    }
    void destroy(EntityRef ref) {
        const auto found=instances.find(ref.persistentId);
        if (found==instances.end() || found->second.ref!=ref) return;
        const auto instance=found->second;
        instances.erase(found); // OnDestroy may request another cleanup.
        if (instance.handle) {
            Scope scope(*this);
            const auto status=managed.destroy(instance.handle);
            if (status!=Status::Ok) report("OnDestroy failed for " + instance.type + " entity " + std::to_string(ref.persistentId));
        }
    }
    void invoke(Instance& instance,Callback kind,float dt) {
        if (!instance.handle || instance.faulted || !scene->valid(instance.ref) || scene->pending(instance.ref)) return;
        Scope scope(*this,kind==Callback::FixedUpdate);
        const auto status=managed.invoke(instance.handle,static_cast<uint32_t>(kind),dt);
        if (status!=Status::Ok) instance.faulted=true;
        try { sync_fields(instance); }
        catch (const std::exception& e) { instance.faulted=true; report(instance.type + " fields: " + e.what()); }
    }
};

ScriptHost::ScriptHost():_impl(std::make_unique<Impl>()) {}
ScriptHost::~ScriptHost() { shutdown(); }
void ScriptHost::bind(Scene& scene,PhysicsWorld& physics) {
    stop();
    if (_impl->scene) _impl->scene->set_script_cleanup_handler({});
    _impl->scene=&scene; _impl->physics=&physics;
    scene.set_script_cleanup_handler([this](EntityRef ref) { _impl->destroy(ref); });
}
bool ScriptHost::initialize(const std::filesystem::path& directory) {
    auto& h=*_impl;
    if (h.ready) return true;
    if (h.attempted) return false;
    h.attempted=true;
    try {
        h.directory=std::filesystem::absolute(directory);
#ifdef _WIN32
        // Host modules and the CLR intentionally live until process exit.
        const auto module=LoadLibraryExW((h.directory/L"nethost.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) throw std::runtime_error("nethost.dll unavailable; install/deploy the .NET 10 hosting pack");
        const auto getPath=reinterpret_cast<decltype(&get_hostfxr_path)>(GetProcAddress(module,"get_hostfxr_path"));
        if (!getPath) throw std::runtime_error("nethost get_hostfxr_path unavailable");
        size_t length{}; getPath(nullptr,&length,nullptr);
        std::wstring path(length,L'\0');
        if (!length || getPath(path.data(),&length,nullptr)!=0) throw std::runtime_error(".NET hostfxr not found; install the x64 .NET 10 runtime");
        const auto fxr=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!fxr) throw std::runtime_error("Cannot load .NET hostfxr");
        const auto initialize=reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetProcAddress(fxr,"hostfxr_initialize_for_runtime_config"));
        const auto getDelegate=reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(fxr,"hostfxr_get_runtime_delegate"));
        const auto close=reinterpret_cast<hostfxr_close_fn>(GetProcAddress(fxr,"hostfxr_close"));
        if (!initialize || !getDelegate || !close) throw std::runtime_error("Incompatible .NET hostfxr API");
        hostfxr_handle context{};
        const auto config=h.directory/L"Mirabilis.ScriptCore.runtimeconfig.json";
        const auto rc=initialize(config.c_str(),nullptr,&context);
        if (rc<0 || !context) { if (context) close(context); throw std::runtime_error(".NET 10 runtime initialization failed (" + std::to_string(rc) + "); native editor remains available"); }
        load_assembly_fn load{};
        get_function_pointer_fn getFunction{};
        const auto delegateStatus=getDelegate(context,hdt_load_assembly,reinterpret_cast<void**>(&load));
        const auto functionStatus=getDelegate(context,hdt_get_function_pointer,reinterpret_cast<void**>(&getFunction));
        close(context);
        if (delegateStatus<0 || functionStatus<0 || !load || !getFunction) throw std::runtime_error("Cannot obtain .NET default-context assembly loader");
        Initialize bridge{};
        const auto assembly=h.directory/L"Mirabilis.ScriptCore.dll";
        if (load(assembly.c_str(),nullptr,nullptr)<0) throw std::runtime_error("Cannot load Mirabilis.ScriptCore in the default context");
        const auto result=getFunction(L"Mirabilis.ScriptCore.NativeBridge, Mirabilis.ScriptCore",L"Initialize",UNMANAGEDCALLERSONLY_METHOD,nullptr,nullptr,reinterpret_cast<void**>(&bridge));
        if (result<0 || !bridge) throw std::runtime_error("Cannot load compatible Mirabilis.ScriptCore native bridge");
        h.managed.version=AbiVersion; h.managed.size=sizeof(ManagedApi);
        const auto status=bridge(&h.native,&h.managed);
        if (status!=Status::Ok || h.managed.version!=AbiVersion || h.managed.size!=sizeof(ManagedApi) || !h.managed.describe || !h.managed.create || !h.managed.invoke || !h.managed.destroy || !h.managed.read_fields || !h.managed.live_count)
            throw std::runtime_error("Script ABI version/layout mismatch; scripting disabled and saved fields retained");
        h.ready=true; return true;
#else
        throw std::runtime_error("The .NET 10 native host currently requires Windows x64");
#endif
    } catch (const std::exception& e) { h.error=e.what(); h.report(h.error); }
    catch (...) { h.error="Unknown .NET initialization failure"; h.report(h.error); }
    return false;
}
bool ScriptHost::available() const { return _impl->ready; }
const std::string& ScriptHost::error() const { return _impl->error; }
const std::vector<std::string>& ScriptHost::diagnostics() const { return _impl->diagnostics; }
Script ScriptHost::describe(const Script& component,std::vector<std::string>& diagnostics) {
    auto& h=*_impl; Script result=component; result.fields.clear();
    if (!h.ready) { diagnostics.push_back(h.error.empty() ? "Scripting runtime is not initialized" : h.error); return result; }
    if (!h.metadata || h.metadata->assembly!=component.assembly || h.metadata->typeName!=component.typeName) {
        h.metadataDiagnostics.clear();
        try {
            const auto json=h.read_buffer([&](uint8_t* p,uint32_t c,uint32_t* n) { return h.managed.describe(utf8(component.assembly),utf8(component.typeName),p,c,n); });
            std::string message;
            if (!parse_script_fields(json,result.fields,message)) h.metadataDiagnostics.push_back(message);
        } catch (const std::exception& e) { h.metadataDiagnostics.push_back(component.assembly + "::" + component.typeName + ": " + e.what()); }
        h.metadata=result;
    }
    result.fields=h.metadata->fields;
    diagnostics.insert(diagnostics.end(),h.metadataDiagnostics.begin(),h.metadataDiagnostics.end());
    return result;
}
void ScriptHost::start() { _impl->input.clear(); _impl->running=true; reconcile(); }
void ScriptHost::reconcile() {
    auto& h=*_impl;
    if (!h.running || !h.ready || !h.scene || h.stopping) return;
    if (h.instances.empty() && h.scene->registry().view<Script>().empty()) return;
    std::vector<EntityRef> retire;
    for (const auto& [id,instance] : h.instances) {
        const auto* component=h.scene->read<Script>(instance.ref);
        if (!component || !component->enabled || component->assembly!=instance.assembly || component->typeName!=instance.type) retire.push_back(instance.ref);
    }
    for (const auto ref : retire) destroy(ref);
    for (const auto ref : h.scene->creation_order()) {
        const auto* component=h.scene->read<Script>(ref);
        if (!component || !component->enabled || h.scene->pending(ref) || h.instances.contains(ref.persistentId)) continue;
        Impl::Instance instance{ref,0,component->assembly,component->typeName,false};
        const auto fields=serialize_script_fields(component->fields);
        Impl::Scope scope(h);
        const auto status=h.managed.create(bridge_entity(ref),utf8(component->assembly),utf8(component->typeName),utf8(fields),&instance.handle);
        if (status!=Status::Ok) { instance.faulted=true; h.report("Cannot construct " + instance.type + " entity " + std::to_string(ref.persistentId)); }
        auto [it,inserted]=h.instances.emplace(ref.persistentId,std::move(instance));
        if (inserted) h.invoke(it->second,Callback::Create,0);
    }
}
void ScriptHost::stop() {
    auto& h=*_impl; h.running=false; h.stopping=true;
    if (h.scene) {
        const auto order=h.scene->hierarchy_order();
        for (auto it=order.rbegin(); it!=order.rend(); ++it) h.scene->cleanup_script(*it);
    }
    // Stale entries cannot call scene APIs, but must still release their managed roots.
    while (!h.instances.empty()) h.destroy(h.instances.begin()->second.ref);
    h.input.clear(); h.stopping=false;
}
void ScriptHost::shutdown() {
    stop();
    if (_impl->scene) {
        _impl->scene->clear_queues();
        _impl->scene->set_script_cleanup_handler({});
    }
    _impl->scene=nullptr; _impl->physics=nullptr;
}
void ScriptHost::destroy(EntityRef ref) {
    if (_impl->scene && _impl->scene->valid(ref)) _impl->scene->cleanup_script(ref);
    else _impl->destroy(ref);
}
void ScriptHost::fixed_update(float dt) {
    reconcile(); auto& h=*_impl;
    if (h.running && h.ready && h.scene && !h.instances.empty()) for (const auto ref : h.scene->creation_order()) {
        const auto it=h.instances.find(ref.persistentId); if (it!=h.instances.end()) h.invoke(it->second,Callback::FixedUpdate,dt);
    }
    h.input.finish_fixed_tick();
}
void ScriptHost::update(float dt) {
    reconcile(); auto& h=*_impl;
    if (h.running && h.ready && h.scene && !h.instances.empty()) for (const auto ref : h.scene->creation_order()) {
        const auto it=h.instances.find(ref.persistentId); if (it!=h.instances.end()) h.invoke(it->second,Callback::Update,dt);
    }
    h.input.finish_frame();
}
size_t ScriptHost::live_count() const {
    return std::count_if(_impl->instances.begin(),_impl->instances.end(),[](const auto& entry) { return entry.second.handle!=0; });
}
bool ScriptHost::faulted(EntityRef ref) const { const auto it=_impl->instances.find(ref.persistentId); return it!=_impl->instances.end() && it->second.ref==ref && it->second.faulted; }
ScriptInput& ScriptHost::input() { return _impl->input; }
const NativeApi& ScriptHost::native_api() const { return _impl->native; }
}
