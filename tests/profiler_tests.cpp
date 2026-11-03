#include <doctest/doctest.h>
#include "frame_clock.h"
#include "profiler.h"
#include <simdjson.h>

#include <limits>
#include <locale>
#include <sstream>
#include <thread>

using namespace mirabilis;

TEST_CASE("frame clock preserves wall time and bounds simulation pauses") {
    FrameClock clock;
    const FrameClock::TimePoint start{};
    clock.reset(start);
    const auto first = clock.tick(start + std::chrono::milliseconds(16));
    CHECK(first.wallSeconds == doctest::Approx(0.016));
    CHECK(first.simulationSeconds == doctest::Approx(0.016f));
    const auto paused = clock.tick(start + std::chrono::seconds(2));
    CHECK(paused.wallSeconds == doctest::Approx(1.984));
    CHECK(paused.simulationSeconds == doctest::Approx(0.1f));
    clock.reset(start + std::chrono::seconds(10));
    const auto resumed = clock.tick(start + std::chrono::milliseconds(10008));
    CHECK(resumed.wallSeconds == doctest::Approx(0.008));
    CHECK(resumed.simulationSeconds == doctest::Approx(0.008f));
}

TEST_CASE("disabled profiling records no scopes or trace events") {
    Profiler profiler;
    {
        auto frame = profiler.frame(3, 0.016);
        auto scope = profiler.scope(CpuZone::Simulation);
    }
    CHECK(profiler.history_size() == 0);
    CHECK(profiler.trace_status().events == 0);
    CHECK(profiler.latest().zones[static_cast<size_t>(CpuZone::Simulation)].calls == 0);
}

TEST_CASE("startup remains in the trace without leaking into the first frame summary") {
    Profiler profiler;
    profiler.start_capture();
    { auto startup = profiler.scope(CpuZone::Startup); }
    {
        auto frame = profiler.frame(0, 0.01);
        auto simulation = profiler.scope(CpuZone::Simulation);
    }
    CHECK(profiler.trace_status().events == 3);
    CHECK(profiler.latest().zones[static_cast<size_t>(CpuZone::Startup)].calls == 0);
    CHECK(profiler.latest().zones[static_cast<size_t>(CpuZone::Simulation)].calls == 1);
}

TEST_CASE("profiling aggregates completed scopes across workers and bounds captures") {
    Profiler profiler;
    profiler.start_capture(32);
    {
        auto frame = profiler.frame(42, 0.02);
        std::array<std::thread, 4> workers;
        for (auto& worker : workers) worker = std::thread([&] {
            for (int i = 0; i < 100; ++i) {
                auto scope = profiler.scope(CpuZone::PhysicsStep);
            }
        });
        for (auto& worker : workers) worker.join();
    }
    const auto& result = profiler.latest();
    CHECK(result.frame == 42);
    CHECK(result.wallMilliseconds == doctest::Approx(20.0));
    CHECK(result.zones[static_cast<size_t>(CpuZone::PhysicsStep)].calls == 400);
    CHECK(result.zones[static_cast<size_t>(CpuZone::Frame)].calls == 1);
    CHECK(profiler.history_size() == 1);
    CHECK(profiler.trace_status().events == 32);
    CHECK(profiler.trace_status().dropped == 369);
    profiler.stop_capture();
    std::ostringstream output;
    REQUIRE(profiler.write_trace(output));
    simdjson::dom::parser parser;
    const simdjson::dom::element root = parser.parse(output.str());
    const simdjson::dom::array events = root["traceEvents"];
    CHECK(events.size() == 32);
    CHECK(uint64_t(root["otherData"]["droppedEvents"]) == 369);
    for (const auto event : events) {
        CHECK(std::string_view(event["ph"]) == "X");
        CHECK(double(event["ts"]) >= 0);
        CHECK(double(event["dur"]) >= 0);
        CHECK(uint64_t(event["tid"]) > 0);
    }
}

TEST_CASE("trace restart rejects scopes from the previous capture") {
    Profiler profiler;
    profiler.start_capture();
    auto previous = profiler.scope(CpuZone::AssetLoad);
    profiler.start_capture();
    previous.finish();
    CHECK(profiler.trace_status().events == 0);
    { auto current = profiler.scope(CpuZone::ImageDecode); }
    profiler.stop_capture();
    { auto stopped = profiler.scope(CpuZone::Upload); }
    CHECK(profiler.trace_status().events == 1);
    CHECK_FALSE(profiler.trace_status().capturing);
}

TEST_CASE("trace counters are valid JSON under a nondefault stream locale") {
    struct Comma : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    Profiler profiler;
    profiler.start_capture(4);
    profiler.counter(ProfileCounter::GpuGI, 1.25);
    profiler.counter(ProfileCounter::GpuGI, std::numeric_limits<double>::infinity());
    profiler.counter(ProfileCounter::GpuGI, std::numeric_limits<double>::quiet_NaN());
    std::ostringstream output;
    const std::locale locale(std::locale::classic(), new Comma);
    output.imbue(locale);
    REQUIRE(profiler.write_trace(output));
    CHECK(output.getloc() == locale);
    simdjson::dom::parser parser;
    const simdjson::dom::element root = parser.parse(output.str());
    const simdjson::dom::array events = root["traceEvents"];
    REQUIRE(events.size() == 1);
    CHECK(std::string_view(events.at(0)["ph"]) == "C");
    CHECK(double(events.at(0)["args"]["value"]) == doctest::Approx(1.25));
    CHECK_FALSE(profiler.save_trace({}));
}

TEST_CASE("frame history rolls over and continues publishing after capture fills") {
    Profiler profiler;
    profiler.start_capture(0);
    for (size_t i = 0; i < Profiler::HistorySize + 5; ++i) {
        auto frame = profiler.frame(i, 0.01);
    }
    CHECK(profiler.history_size() == Profiler::HistorySize);
    CHECK(profiler.history_offset() == 5);
    CHECK(profiler.latest().frame == Profiler::HistorySize + 4);
    CHECK(profiler.trace_status().events == 0);
    CHECK(profiler.trace_status().dropped == Profiler::HistorySize + 5);
    profiler.set_enabled(false);
    CHECK_FALSE(profiler.enabled());
    CHECK_FALSE(profiler.trace_status().capturing);
}

TEST_CASE("profiling toggles discard scopes and frames spanning the disabled interval") {
    Profiler profiler;
    profiler.set_enabled(true);
    {
        auto frame = profiler.frame(0, 0.01);
        auto interrupted = profiler.scope(CpuZone::Upload);
        profiler.set_enabled(false);
        profiler.set_enabled(true);
        CHECK_FALSE(interrupted.finish());
    }
    CHECK(profiler.history_size() == 0);
    {
        auto frame = profiler.frame(1, 0.01);
        auto scope = profiler.scope(CpuZone::Simulation);
    }
    CHECK(profiler.history_size() == 1);
    CHECK(profiler.latest().zones[static_cast<size_t>(CpuZone::Upload)].calls == 0);
    CHECK(profiler.latest().zones[static_cast<size_t>(CpuZone::Simulation)].calls == 1);
}
