#pragma once
#include "physics_world.h"
#include "script_abi.h"
#include <array>
#include <filesystem>
#include <memory>

namespace mirabilis {
class ScriptInput {
public:
    void event(scripting::Key key,bool down,bool repeat=false);
    void focus_lost();
    void clear();
    void finish_frame();
    void finish_fixed_tick();
    bool query(scripting::Key,scripting::InputQuery,bool fixed) const;
private:
    uint64_t _down{},_framePressed{},_frameReleased{},_fixedPressed{},_fixedReleased{};
};
class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&)=delete;
    ScriptHost& operator=(const ScriptHost&)=delete;
    void bind(Scene&,PhysicsWorld&);
    bool initialize(const std::filesystem::path& managedDirectory);
    bool available() const;
    const std::string& error() const;
    const std::vector<std::string>& diagnostics() const;
    Script describe(const Script&,std::vector<std::string>& diagnostics);
    void start();
    void reconcile();
    void stop();
    void shutdown();
    void destroy(EntityRef);
    void fixed_update(float);
    void update(float);
    size_t live_count() const;
    bool faulted(EntityRef) const;
    ScriptInput& input();
    const scripting::NativeApi& native_api() const;
    bool reload(const std::filesystem::path& directory);
    void watch_project(const std::filesystem::path& project,const std::filesystem::path& buildDirectory,const std::filesystem::path& dotnet,std::string configuration);
    void request_reload();
    bool poll_reload();
    bool auto_reload() const;
    void set_auto_reload(bool);
    const std::string& reload_status() const;
    bool can_build_scripts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
