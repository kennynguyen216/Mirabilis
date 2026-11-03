#include "vk_engine.h"

#include <algorithm>
#include <cmath>

namespace {
float lerp_yaw(float from, float to, float t)
{
    constexpr float Pi = 3.14159265358979323846f;
    constexpr float TwoPi = 2.0f * Pi;
    float delta = std::fmod(to - from + Pi, TwoPi);
    if (delta < 0.0f) delta += TwoPi;
    return from + (delta - Pi) * t;
}
}

void VulkanEngine::tick_simulation(float deltaTime, bool gameplayInputAllowed)
{
    auto simulationScope = _profiler.scope(mirabilis::CpuZone::Simulation);
    if (_editorMode) {
        if (!_cinematic.recording) _editorCamera.update(deltaTime);
        _playerStep.accumulator = 0.0f;
        _playerStep.previousPlayerYaw = mainCamera.yaw;
        return;
    }

    if (_scriptHost && !gameplayInputAllowed) _scriptHost->input().focus_lost();
    _playerStep.accumulator = std::min(
        _playerStep.accumulator + deltaTime,
        PlayerMovementDt * static_cast<float>(MaxPlayerMovementSteps));
    _playerStep.targetPlayerYaw = mainCamera.yaw;
    // Keep the original fixed-step accumulator and callback ordering shared by both hosts.
    const int stepCount = std::max(1, static_cast<int>(_playerStep.accumulator / PlayerMovementDt));
    int stepIndex = 0;
    while (_playerStep.accumulator >= PlayerMovementDt) {
        ++stepIndex;
        _playerInput.yaw = lerp_yaw(_playerStep.previousPlayerYaw, _playerStep.targetPlayerYaw,
            static_cast<float>(stepIndex) / static_cast<float>(stepCount));
        if (_scriptHost) {
            auto scope = _profiler.scope(mirabilis::CpuZone::ScriptFixed);
            _scriptHost->fixed_update(PlayerMovementDt);
        }
        {
            auto scope = _profiler.scope(mirabilis::CpuZone::SceneCommands);
            _scene.apply_commands();
        }
        {
            auto scope = _profiler.scope(mirabilis::CpuZone::Movement);
            update_player_movement(PlayerMovementDt);
        }
        sync_physics_world();
        {
            auto scope = _profiler.scope(mirabilis::CpuZone::PhysicsStep);
            _physicsWorld.step(_scene, PlayerMovementDt);
        }
        drain_scene_destruction();
        _playerStep.accumulator -= PlayerMovementDt;
    }
    _playerStep.previousPlayerYaw = _playerStep.targetPlayerYaw;
    if (_scriptHost) {
        auto scope = _profiler.scope(mirabilis::CpuZone::ScriptUpdate);
        _scriptHost->update(deltaTime);
    }
    drain_scene_destruction();
}
