#include <doctest/doctest.h>
#include "memory_resources.h"
#include "task_system.h"
#include <array>
#include <cstring>
#include <future>
#include <limits>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xmmintrin.h>
#endif

using namespace mirabilis;

TEST_CASE("arena rejects wrapping requests and preserves storage when reallocation fails") {
    VirtualArena arena(65536);
    auto* first = static_cast<unsigned char*>(arena.try_allocate(32, 1));
    REQUIRE(first); std::memset(first, 0x5a, 32);
    REQUIRE(arena.try_allocate(32, 1));
    const auto before = arena.statistics();
    CHECK(arena.try_allocate(SIZE_MAX - 16, 1) == nullptr);
    CHECK(arena.try_reallocate(first, 32, 131072) == nullptr);
    CHECK(arena.statistics().used == before.used);
    for (size_t i = 0; i < 32; ++i) CHECK(first[i] == 0x5a);
    CHECK(arena.try_allocate(1, 3) == nullptr);
}
TEST_CASE("arena absolute alignment markers and decommit preserve their contracts") {
    VirtualArena arena(2 * 1024 * 1024);
    for (size_t alignment = 1; alignment <= 262144; alignment <<= 1) {
        auto* value = arena.try_allocate(31, alignment);
        REQUIRE(value); CHECK(reinterpret_cast<uintptr_t>(value) % alignment == 0);
    }
    auto marker = arena.mark();
    auto* newest = arena.try_allocate(128);
    REQUIRE(newest); std::memset(newest, 9, 128);
    REQUIRE(arena.try_reallocate(newest, 128, 256) == newest);
    CHECK(arena.rewind(marker));
    CHECK(arena.statistics().used == marker.offset);
    arena.reset(true);
    CHECK(arena.statistics().used == 0);
    CHECK(arena.statistics().committed == 0);
    CHECK_FALSE(arena.rewind(marker));
    REQUIRE(arena.try_allocate(64));
    CHECK(arena.statistics().committed > 0);
}
TEST_CASE("fixed pools reject interior and double releases and reuse exact slots") {
    FixedBlockPool pool(48, 16, 8);
    auto* first = static_cast<std::byte*>(pool.try_allocate(48, 16)); REQUIRE(first);
    CHECK_FALSE(pool.release(first + 1));
    CHECK(pool.release(first)); CHECK_FALSE(pool.release(first));
    CHECK(pool.try_allocate(48, 16) == first);
    CHECK(pool.try_allocate(49, 16) == nullptr);
    CHECK(pool.try_allocate(48, 32) == nullptr);
    CHECK(pool.release(first)); CHECK(pool.statistics().used == 0);
    CHECK_THROWS_AS(FixedBlockPool(SIZE_MAX, 16), std::invalid_argument);
}
TEST_CASE("size classes support concurrent PMR users and over-aligned fallback") {
    SizeClassResource resource;
    std::array<std::thread, 4> workers;
    std::atomic<int> good{0};
    for (auto& worker : workers) worker = std::thread([&] {
        for (int i = 0; i < 1000; ++i) {
            auto* memory = static_cast<unsigned char*>(resource.allocate(48, 16));
            std::memset(memory, 0x42, 48);
            if (memory[0] == 0x42 && memory[47] == 0x42) ++good;
            resource.deallocate(memory, 48, 16);
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK(good == 4000); CHECK(resource.statistics().used == 0);
    auto* aligned = resource.allocate(4096, 256);
    CHECK(reinterpret_cast<uintptr_t>(aligned) % 256 == 0);
    resource.deallocate(aligned, 4096, 256);
    { std::pmr::vector<int> values(&resource); values.resize(10000, 42); CHECK(values.back() == 42); }
    CHECK(resource.statistics().used == 0);
}

namespace {
struct PingPong {
    FiberThread* thread; Fiber* fiber{}; size_t visits{}; bool fpGood{true};
};
void ping_pong(void* pointer) {
    auto& data = *static_cast<PingPong*>(pointer);
    volatile uint64_t local = 0x123456789abcdef0;
#ifdef _WIN32
    _mm_setcsr(0x5f80);
#endif
    for (;;) {
        ++data.visits;
        data.fpGood &= local == 0x123456789abcdef0;
        data.thread->switch_between(*data.fiber, data.thread->root());
#ifdef _WIN32
        data.fpGood &= _mm_getcsr() == 0x5f80;
#endif
    }
}
void chain(TaskSystem& tasks, size_t depth, std::atomic<size_t>& visits) {
    ++visits;
    if (!depth) return;
    std::pmr::vector<int> local(TaskSystem::scratch()); local.resize(100, 37);
    TaskGroup children;
    tasks.submit(children, [&tasks, depth, &visits] { chain(tasks, depth - 1, visits); });
    tasks.wait(children);
    if (local.front() != 37 || local.back() != 37) throw std::runtime_error("Task scratch was reused while suspended");
}
}
TEST_CASE("native and OS fibers preserve suspended locals and floating point controls") {
    for (auto backend : {FiberBackend::Assembly, FiberBackend::Windows}) {
        if (backend == FiberBackend::Assembly && !FiberThread::assembly_available()) continue;
        FiberThread thread(backend);
        PingPong data{&thread};
        Fiber fiber(backend, 131072, &ping_pong, &data); data.fiber = &fiber;
#ifdef _WIN32
        const auto original = _mm_getcsr(); _mm_setcsr(0x3f80);
#endif
        for (size_t i = 0; i < 10000; ++i) {
            thread.switch_between(thread.root(), fiber);
            CHECK(data.visits == i + 1);
#ifdef _WIN32
            REQUIRE(_mm_getcsr() == 0x3f80);
#endif
        }
        CHECK(data.fpGood);
#ifdef _WIN32
        _mm_setcsr(original);
#endif
    }
}
TEST_CASE("borrowed Windows fiber conversion stays owned by the outer session") {
#ifdef _WIN32
    const bool originally = IsThreadAFiber() != FALSE;
    {
        FiberThread outer(FiberBackend::Windows);
        { FiberThread borrowed(FiberBackend::Windows); CHECK(IsThreadAFiber()); }
        CHECK(IsThreadAFiber());
    }
    CHECK((IsThreadAFiber() != FALSE) == originally);
#endif
}
TEST_CASE("nested jobs progress with one fiber and preserve per-task scratch") {
    TaskSystemDesc desc; desc.workers = 1; desc.fibersPerWorker = 1;
    TaskSystem tasks(desc);
    TaskGroup group; std::atomic<size_t> visits{0};
    tasks.submit(group, [&] { chain(tasks, 32, visits); });
    tasks.wait(group);
    CHECK(visits == 33); CHECK(group.pending() == 0);
    CHECK(tasks.statistics().inlineFallbacks > 0);
    CHECK(tasks.statistics().scratchCommitted > 0);
}
TEST_CASE("a 129-job chain completes with a 128-fiber pool") {
    TaskSystemDesc desc; desc.workers = 1; desc.fibersPerWorker = 128; desc.stackBytes = 65536;
    TaskSystem tasks(desc);
    TaskGroup group; std::atomic<size_t> visits{0};
    tasks.submit(group, [&] { chain(tasks, 128, visits); });
    tasks.wait(group);
    CHECK(visits == 129); CHECK(tasks.statistics().pending == 0);
}
TEST_CASE("nested ParallelFor completes every index exactly once on each backend") {
    for (auto backend : {FiberBackend::Assembly, FiberBackend::Windows}) {
        if (backend == FiberBackend::Assembly && !FiberThread::assembly_available()) continue;
        TaskSystemDesc desc; desc.workers = 2; desc.fibersPerWorker = 2; desc.backend = backend;
        TaskSystem tasks(desc);
        std::vector<std::atomic<int>> hits(1600);
        tasks.parallel_for(16, 1, [&](size_t outer) {
            tasks.parallel_for(100, 7, [&](size_t inner) { ++hits[outer * 100 + inner]; });
        });
        for (auto& hit : hits) CHECK(hit == 1);
    }
}
TEST_CASE("task failures cancellation self-waits and bounded nesting terminate cleanly") {
    TaskSystemDesc desc; desc.workers = 1; desc.fibersPerWorker = 1; desc.maxInlineDepth = 4;
    TaskSystem tasks(desc);
    TaskGroup errors; tasks.submit(errors, [] { throw std::runtime_error("failure"); });
    CHECK_THROWS_AS(tasks.wait(errors), std::runtime_error);
    errors.reset(); std::atomic<size_t> visits{0};
    tasks.submit(errors, [&] { chain(tasks, 12, visits); });
    CHECK_THROWS_AS(tasks.wait(errors), std::runtime_error);
    CHECK(errors.pending() == 0);
    errors.reset(); tasks.submit(errors, [&] { tasks.wait(errors); });
    CHECK_THROWS_AS(tasks.wait(errors), std::logic_error);
    std::stop_source source; source.request_stop(); TaskGroup cancelled; bool ran = false;
    tasks.submit(cancelled, [&] { ran = true; }, TaskPriority::Normal, source.get_token());
    tasks.wait(cancelled); CHECK_FALSE(ran); CHECK(cancelled.cancelled() == 1);
    tasks.shutdown(); CHECK_THROWS_AS(tasks.submit(cancelled, [] {}), std::logic_error);
}
TEST_CASE("shutdown drains accepted parents and their nested descendants") {
    std::atomic<size_t> visits{0};
    {
        TaskSystemDesc desc; desc.workers = 2; desc.fibersPerWorker = 1;
        TaskSystem tasks(desc); TaskGroup group;
        for (int i = 0; i < 30; ++i) tasks.submit(group, [&] { chain(tasks, 3, visits); });
        tasks.shutdown(); CHECK(group.pending() == 0);
    }
    CHECK(visits == 120);
}

TEST_CASE("task scratch heap fallback survives suspension and is released by PMR containers") {
    TaskSystemDesc desc; desc.workers = 1; desc.fibersPerWorker = 2; desc.scratchBytes = 65536;
    TaskSystem tasks(desc); TaskGroup parent;
    tasks.submit(parent, [&] {
        std::pmr::vector<int> large(TaskSystem::scratch()); large.resize(100000, 81);
        TaskGroup child; tasks.submit(child, [] { std::pmr::vector<int> data(TaskSystem::scratch()); data.resize(50000, 19); });
        tasks.wait(child);
        if (large.front() != 81 || large.back() != 81) throw std::runtime_error("heap scratch fallback corrupted");
        std::pmr::vector<int> after(TaskSystem::scratch()); after.resize(12, 42);
    });
    CHECK_NOTHROW(tasks.wait(parent)); CHECK(parent.pending() == 0);
}
