#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <vector>

namespace mirabilis {

enum class CpuZone : uint8_t {
    Startup, Frame, Events, Reload, Simulation, ScriptFixed, SceneCommands,
    Movement, PhysicsSync, PhysicsStep, ScriptUpdate, Destruction, UI,
    Render, WaitGPU, AcquireImage, SceneUpdate, Geometry, AssetLoad,
    ImageDecode, Upload, FrameLimit, TaskExecute, TaskWait, Count
};

enum class ProfileCounter : uint8_t {
    GpuAO, GpuGI, GpuExposure, GpuTrace, DrawCalls, Triangles, DrawScratchBytes, DrawScratchGrowths, Count
};

const char* zone_name(CpuZone zone);
const char* counter_name(ProfileCounter counter);

struct ProfileSample {
    double milliseconds{};
    uint64_t calls{};
};

struct FrameProfile {
    uint64_t frame{};
    double wallMilliseconds{};
    std::array<ProfileSample, static_cast<size_t>(CpuZone::Count)> zones{};
};

struct TraceStatus {
    size_t events{};
    uint64_t dropped{};
    bool capturing{};
};

// Scope recording is thread-safe. Frame publication and UI reads belong to the host thread.
class Profiler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaxTraceEvents = 65536;
    static constexpr size_t HistorySize = 240;

    class Scope {
    public:
        Scope(Profiler& profiler, CpuZone zone);
        ~Scope() { finish(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        bool finish();

    private:
        Profiler* profiler_{};
        CpuZone zone_{};
        Clock::time_point start_{};
        uint64_t captureGeneration_{};
        uint64_t enabledGeneration_{};
        uint32_t thread_{};
    };

    class Frame {
    public:
        Frame(Profiler& profiler, uint64_t number, double wallSeconds)
            : profiler_(profiler), scope_(profiler, CpuZone::Frame),
              number_(number), wallSeconds_(wallSeconds) { profiler_.begin_frame(); }
        ~Frame() { profiler_.finish_frame(number_, wallSeconds_, scope_.finish()); }
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;

    private:
        Profiler& profiler_;
        Scope scope_;
        uint64_t number_;
        double wallSeconds_;
    };

    Scope scope(CpuZone zone) { return Scope(*this, zone); }
    Frame frame(uint64_t number, double wallSeconds) { return Frame(*this, number, wallSeconds); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void set_enabled(bool enabled);
    void start_capture(size_t capacity = MaxTraceEvents);
    void stop_capture();
    TraceStatus trace_status() const;
    bool write_trace(std::ostream& output) const;
    bool save_trace(const std::filesystem::path& path) const;
    void counter(ProfileCounter counter, double value);

    const FrameProfile& latest() const { return latest_; }
    const float* frame_history() const { return frameHistory_.data(); }
    int history_size() const { return historyCount_; }
    int history_offset() const { return historyCount_ == HistorySize ? historyHead_ : 0; }

private:
    struct SampleAccumulator {
        std::mutex mutex;
        uint64_t nanoseconds{};
        uint64_t calls{};
    };
    struct Event {
        uint64_t startNanoseconds{};
        uint64_t durationNanoseconds{};
        uint32_t thread{};
        uint8_t name{};
        bool isCounter{};
        double value{};
    };

    bool record(CpuZone zone, Clock::time_point start, Clock::time_point end,
                uint64_t captureGeneration, uint64_t enabledGeneration, uint32_t thread);
    void begin_frame();
    void finish_frame(uint64_t frame, double wallSeconds, bool complete);
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> enabledGeneration_{1};
    std::array<SampleAccumulator, static_cast<size_t>(CpuZone::Count)> totals_{};
    FrameProfile latest_{};
    std::array<float, HistorySize> frameHistory_{};
    int historyHead_{};
    int historyCount_{};

    mutable std::mutex traceMutex_;
    std::atomic<bool> capturing_{false};
    std::atomic<uint64_t> captureGeneration_{0};
    Clock::time_point captureStart_{};
    std::vector<Event> events_;
    size_t capacity_{};
    uint64_t dropped_{};
};

} // namespace mirabilis
