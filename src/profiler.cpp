#include "profiler.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <ostream>

namespace mirabilis {
namespace {
constexpr std::array ZoneNames{
    "Startup", "Frame", "Events", "Reload", "Simulation", "Script fixed update", "Scene commands",
    "Movement", "Physics sync", "Physics step", "Script update", "Destruction", "UI",
    "Render", "GPU fence wait", "Acquire image", "Scene update", "Geometry", "Asset load",
    "Image decode", "Upload", "Frame limiter", "Task execution", "Task wait"
};
constexpr std::array CounterNames{
    "GPU AO ms (completed)", "GPU GI ms (completed)", "GPU exposure ms (completed)",
    "GPU trace ms (completed)", "Draw calls", "Triangles", "Draw scratch bytes", "Draw scratch growths"
};
static_assert(ZoneNames.size() == static_cast<size_t>(CpuZone::Count));
static_assert(CounterNames.size() == static_cast<size_t>(ProfileCounter::Count));

uint32_t thread_id() {
    static std::atomic<uint32_t> next{1};
    thread_local const uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}
uint64_t nanoseconds(Profiler::Clock::duration duration) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}
}

const char* zone_name(CpuZone zone) { return ZoneNames.at(static_cast<size_t>(zone)); }
const char* counter_name(ProfileCounter counter) { return CounterNames.at(static_cast<size_t>(counter)); }

Profiler::Scope::Scope(Profiler& profiler, CpuZone zone) : zone_(zone) {
    if (!profiler.enabled()) return;
    profiler_ = &profiler;
    enabledGeneration_ = profiler.enabledGeneration_.load(std::memory_order_acquire);
    start_ = Clock::now();
    if (profiler.capturing_.load(std::memory_order_acquire)) {
        captureGeneration_ = profiler.captureGeneration_.load(std::memory_order_acquire);
        thread_ = thread_id();
    }
}

bool Profiler::Scope::finish() {
    if (!profiler_) return false;
    const bool recorded = profiler_->record(zone_, start_, Clock::now(), captureGeneration_, enabledGeneration_, thread_);
    profiler_ = nullptr;
    return recorded;
}

void Profiler::set_enabled(bool enabled) {
    if (enabled_.exchange(enabled, std::memory_order_acq_rel) != enabled)
        enabledGeneration_.fetch_add(1, std::memory_order_release);
    if (!enabled) stop_capture();
}

void Profiler::start_capture(size_t capacity) {
    std::lock_guard lock(traceMutex_);
    capturing_.store(false, std::memory_order_release);
    capacity_ = std::min(capacity, MaxTraceEvents);
    events_.clear();
    events_.reserve(capacity_);
    dropped_ = 0;
    captureStart_ = Clock::now();
    captureGeneration_.fetch_add(1, std::memory_order_release);
    enabled_.store(true, std::memory_order_relaxed);
    capturing_.store(true, std::memory_order_release);
}

void Profiler::stop_capture() {
    std::lock_guard lock(traceMutex_);
    capturing_.store(false, std::memory_order_release);
}

TraceStatus Profiler::trace_status() const {
    std::lock_guard lock(traceMutex_);
    return {events_.size(), dropped_, capturing_.load(std::memory_order_acquire)};
}

bool Profiler::record(CpuZone zone, Clock::time_point start, Clock::time_point end,
                      uint64_t generation, uint64_t enabledGeneration, uint32_t thread) {
    if (!enabled()) return false;
    const uint64_t duration = nanoseconds(end - start);
    auto& total = totals_[static_cast<size_t>(zone)];
    {
        std::lock_guard lock(total.mutex);
        if (enabledGeneration != enabledGeneration_.load(std::memory_order_acquire)) return false;
        total.nanoseconds += duration;
        ++total.calls;
    }
    if (!generation || !capturing_.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(traceMutex_);
    if (!capturing_.load(std::memory_order_relaxed) ||
        generation != captureGeneration_.load(std::memory_order_relaxed) || start < captureStart_) return true;
    if (events_.size() == capacity_) { ++dropped_; return true; }
    events_.push_back({nanoseconds(start - captureStart_), duration, thread,
                      static_cast<uint8_t>(zone), false, 0.0});
    return true;
}

void Profiler::counter(ProfileCounter counter, double value) {
    if (!std::isfinite(value) || !capturing_.load(std::memory_order_acquire)) return;
    const auto now = Clock::now();
    std::lock_guard lock(traceMutex_);
    if (!capturing_.load(std::memory_order_relaxed) || now < captureStart_) return;
    if (events_.size() == capacity_) { ++dropped_; return; }
    events_.push_back({nanoseconds(now - captureStart_), 0, thread_id(),
                      static_cast<uint8_t>(counter), true, value});
}

void Profiler::begin_frame() {
    if (!enabled()) return;
    for (auto& sample : totals_) {
        std::lock_guard lock(sample.mutex);
        sample.nanoseconds = sample.calls = 0;
    }
}

void Profiler::finish_frame(uint64_t frame, double wallSeconds, bool complete) {
    if (!enabled()) return;
    FrameProfile completed;
    completed.frame = frame;
    completed.wallMilliseconds = std::max(0.0, wallSeconds) * 1000.0;
    for (size_t i = 0; i < totals_.size(); ++i) {
        std::lock_guard lock(totals_[i].mutex);
        completed.zones[i].milliseconds = static_cast<double>(totals_[i].nanoseconds) * 1e-6;
        completed.zones[i].calls = totals_[i].calls;
        totals_[i].nanoseconds = totals_[i].calls = 0;
    }
    if (!complete || !enabled()) return;
    latest_ = completed;
    frameHistory_[historyHead_] = static_cast<float>(latest_.zones[static_cast<size_t>(CpuZone::Frame)].milliseconds);
    historyHead_ = (historyHead_ + 1) % HistorySize;
    historyCount_ = std::min(historyCount_ + 1, static_cast<int>(HistorySize));
}

bool Profiler::write_trace(std::ostream& output) const {
    std::vector<Event> snapshot;
    uint64_t dropped;
    {
        std::lock_guard lock(traceMutex_);
        snapshot = events_;
        dropped = dropped_;
    }
    // Fixed names need no user-string escaping. Export runs outside the recording lock.
    const auto locale = output.getloc();
    const auto flags = output.flags();
    const auto precision = output.precision();
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(3);
    output << "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"droppedEvents\":" << dropped << "},\"traceEvents\":[";
    bool first = true;
    for (const auto& event : snapshot) {
        if (!first) output << ',';
        first = false;
        const char* name = event.isCounter ? counter_name(static_cast<ProfileCounter>(event.name))
                                           : zone_name(static_cast<CpuZone>(event.name));
        output << "{\"name\":\"" << name << "\",\"cat\":\"" << (event.isCounter ? "counter" : "cpu")
               << "\",\"pid\":1,\"tid\":" << event.thread << ",\"ts\":" << static_cast<double>(event.startNanoseconds) * 1e-3;
        if (event.isCounter) output << ",\"ph\":\"C\",\"args\":{\"value\":" << event.value << "}}";
        else output << ",\"ph\":\"X\",\"dur\":" << static_cast<double>(event.durationNanoseconds) * 1e-3 << '}';
    }
    output << "]}\n";
    output.imbue(locale);
    output.flags(flags);
    output.precision(precision);
    return static_cast<bool>(output);
}

bool Profiler::save_trace(const std::filesystem::path& path) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !write_trace(output)) return false;
    output.close();
    return static_cast<bool>(output);
}

} // namespace mirabilis
