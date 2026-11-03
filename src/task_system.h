#pragma once

#include "fiber.h"
#include "memory_resources.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace mirabilis {
class Profiler;
enum class TaskPriority : uint8_t { High, Normal, Low };

class TaskFunction {
public:
    template<class F> TaskFunction(F&& function) {
        using T = std::decay_t<F>;
        static_assert(sizeof(T) <= sizeof(storage_) && alignof(T) <= alignof(std::max_align_t), "Capture owned data by pointer/reference");
        static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>);
        new (storage_) T(std::forward<F>(function));
        invoke_ = [](void* p) { (*static_cast<T*>(p))(); };
        move_ = [](void* to, void* from) { new (to) T(std::move(*static_cast<T*>(from))); static_cast<T*>(from)->~T(); };
        destroy_ = [](void* p) { static_cast<T*>(p)->~T(); };
    }
    TaskFunction() = default;
    TaskFunction(TaskFunction&& other) noexcept { *this = std::move(other); }
    TaskFunction& operator=(TaskFunction&& other) noexcept {
        if (this != &other) {
            reset();
            if (other.invoke_) {
                other.move_(storage_, other.storage_);
                invoke_ = other.invoke_; move_ = other.move_; destroy_ = other.destroy_;
                other.invoke_ = nullptr; other.move_ = nullptr; other.destroy_ = nullptr;
            }
        }
        return *this;
    }
    TaskFunction(const TaskFunction&) = delete;
    ~TaskFunction() { reset(); }
    void operator()() { invoke_(storage_); }
    void reset() noexcept { if (destroy_) destroy_(storage_); invoke_ = nullptr; move_ = nullptr; destroy_ = nullptr; }
private:
    alignas(std::max_align_t) std::byte storage_[64];
    void (*invoke_)(void*){};
    void (*move_)(void*, void*){};
    void (*destroy_)(void*){};
};

class TaskGroup {
public:
    TaskGroup();
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    size_t pending() const noexcept;
    size_t cancelled() const noexcept;
    void reset();
private:
    friend class TaskSystem;
    struct State {
        std::atomic<size_t> pending{0}, cancelled{0};
        std::mutex mutex;
        std::condition_variable changed;
        std::exception_ptr error;
    };
    std::shared_ptr<State> state_;
};

struct TaskSystemDesc {
    size_t workers{0}, fibersPerWorker{16}, stackBytes{256 * 1024}, scratchBytes{1024 * 1024};
    size_t maxInlineDepth{64};
    FiberBackend backend{FiberBackend::Automatic};
};
struct TaskStatistics {
    size_t workers{}, fibers{}, queued{}, running{}, parked{}, pending{}, completed{}, inlineFallbacks{}, cancelled{};
    size_t scratchCommitted{}, scratchPeak{}, poolCommitted{};
    FiberBackend backend{FiberBackend::Automatic};
};

class TaskSystem {
public:
    explicit TaskSystem(TaskSystemDesc desc = {}, Profiler* profiler = nullptr);
    ~TaskSystem();
    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;
    template<class F> void submit(TaskGroup& group, F&& function, TaskPriority priority = TaskPriority::Normal,
                                 std::stop_token stop = {}) {
        submit_impl(group, TaskFunction(std::forward<F>(function)), priority, stop);
    }
    void wait(TaskGroup& group, const std::function<void()>& idlePump = {});
    template<class F> void parallel_for(size_t count, size_t batchSize, F&& function) {
        if (!count) return;
        batchSize = std::max(batchSize, size_t(1));
        TaskGroup group;
        try {
            for (size_t begin = 0; begin < count;) {
                const size_t end = begin + std::min(batchSize, count - begin);
                submit(group, [&function, begin, end] { for (size_t i = begin; i < end; ++i) function(i); });
                begin = end;
            }
        } catch (...) {
            const auto error = std::current_exception();
            try { wait(group); } catch (...) {}
            std::rethrow_exception(error);
        }
        wait(group);
    }
    void shutdown();
    TaskStatistics statistics() const;
    static std::pmr::memory_resource* scratch();
private:
    void submit_impl(TaskGroup&, TaskFunction, TaskPriority, std::stop_token);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
