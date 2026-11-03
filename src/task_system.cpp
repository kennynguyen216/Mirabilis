#include "task_system.h"
#include "profiler.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mirabilis {
namespace {
class TaskScratch final : public std::pmr::memory_resource {
public:
    explicit TaskScratch(VirtualArena& arena) : arena_(arena) {}
private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (auto* pointer = arena_.try_allocate(bytes, alignment)) return pointer;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
        if (!arena_.owns(pointer)) std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    VirtualArena& arena_;
};
}
TaskGroup::TaskGroup() : state_(std::make_shared<State>()) {}
size_t TaskGroup::pending() const noexcept { return state_->pending.load(std::memory_order_acquire); }
size_t TaskGroup::cancelled() const noexcept { return state_->cancelled.load(std::memory_order_acquire); }
void TaskGroup::reset() {
    if (pending()) throw std::logic_error("Cannot reset unfinished task group");
    state_ = std::make_shared<State>();
}

struct TaskSystem::Impl {
    struct Job { TaskFunction function; std::shared_ptr<TaskGroup::State> group; std::stop_token stop; };
    struct Worker;
    enum class Phase { Free, Running, Waiting };
    struct Slot {
        Worker& worker;
        VirtualArena scratch;
        std::unique_ptr<Fiber> fiber;
        Job job;
        std::shared_ptr<TaskGroup::State> waiting;
        std::pmr::memory_resource* activeScratch{};
        Phase phase{Phase::Free};
        Slot(Worker& owner, size_t bytes) : worker(owner), scratch(bytes) {}
    };
    struct Worker {
        Impl& system;
        std::unique_ptr<FiberThread> session;
        std::vector<std::unique_ptr<Slot>> slots;
        VirtualArena rootScratch;
        Slot* current{};
        std::pmr::memory_resource* scratch{};
        size_t inlineDepth{};
        TaskGroup::State* activeGroup{};
        std::atomic<FiberBackend> backend{FiberBackend::Automatic};
        std::atomic<size_t> committed{0}, peak{0};
        Worker(Impl& owner) : system(owner), rootScratch(owner.desc.scratchBytes) {}
        void resume(Slot&);
        bool step();
        void execute(Job&, VirtualArena&);
        void report_memory();
    };
    static thread_local Worker* local;
    static void fiber_main(void*);
    void worker_main(Worker&);
    void wake();
    bool pop(Job&);
    void wait_worker(Worker&, const std::shared_ptr<TaskGroup::State>&);

    TaskSystemDesc desc;
    Profiler* profiler;
    SizeClassResource memory;
    std::array<std::pmr::deque<Job>, 3> queues{
        std::pmr::deque<Job>(&memory), std::pmr::deque<Job>(&memory), std::pmr::deque<Job>(&memory)};
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    mutable std::mutex mutex;
    std::condition_variable changed;
    size_t generation{}, ready{};
    bool stopping{};
    std::exception_ptr startupError;
    std::atomic<size_t> queued{0}, running{0}, parked{0}, pending{0}, completed{0}, fallbacks{0}, cancelled{0};
    explicit Impl(TaskSystemDesc d, Profiler* p) : desc(d), profiler(p) {}
};
thread_local TaskSystem::Impl::Worker* TaskSystem::Impl::local = nullptr;

void TaskSystem::Impl::wake() {
    { std::lock_guard lock(mutex); ++generation; }
    changed.notify_all();
}
bool TaskSystem::Impl::pop(Job& job) {
    std::lock_guard lock(mutex);
    for (auto& queue : queues) if (!queue.empty()) {
        job = std::move(queue.front()); queue.pop_front(); --queued; return true;
    }
    return false;
}
void TaskSystem::Impl::Worker::report_memory() {
    size_t bytes = rootScratch.statistics().committed;
    for (const auto& slot : slots) bytes += slot->scratch.statistics().committed;
    committed.store(bytes, std::memory_order_relaxed);
    peak.store(std::max(peak.load(std::memory_order_relaxed), bytes), std::memory_order_relaxed);
}
void TaskSystem::Impl::Worker::execute(Job& job, VirtualArena& arena) {
    const auto marker = arena.mark();
    TaskScratch resource(arena);
    auto* previousScratch = scratch; scratch = &resource;
    auto* previousGroup = activeGroup; activeGroup = job.group.get();
    std::exception_ptr error;
    if (job.stop.stop_requested()) { ++job.group->cancelled; ++system.cancelled; }
    else {
        try { job.function(); }
        catch (...) { error = std::current_exception(); }
    }
    job.function.reset();
    if (!arena.rewind(marker)) std::terminate();
    scratch = previousScratch;
    activeGroup = previousGroup;
    report_memory();
    auto state = std::move(job.group);
    {
        std::lock_guard lock(state->mutex);
        if (error && !state->error) state->error = error;
        ++system.completed; --system.pending;
        state->pending.fetch_sub(1, std::memory_order_release);
    }
    state->changed.notify_all();
    system.wake();
}
void TaskSystem::Impl::fiber_main(void* pointer) {
    auto& slot = *static_cast<Slot*>(pointer);
    for (;;) {
        slot.worker.execute(slot.job, slot.scratch);
        slot.activeScratch = nullptr;
        slot.phase = Phase::Free;
        slot.worker.session->switch_between(*slot.fiber, slot.worker.session->root());
    }
}
void TaskSystem::Impl::Worker::resume(Slot& slot) {
    auto* previous = current;
    auto* previousScratch = scratch;
    auto* previousGroup = activeGroup;
    current = &slot;
    scratch = slot.activeScratch;
    activeGroup = slot.job.group.get();
    slot.phase = Phase::Running;
    ++system.running;
    if (system.profiler) {
        auto scope = system.profiler->scope(CpuZone::TaskExecute);
        session->switch_between(session->root(), *slot.fiber);
    } else session->switch_between(session->root(), *slot.fiber);
    --system.running;
    current = previous;
    scratch = previousScratch;
    activeGroup = previousGroup;
    if (slot.phase == Phase::Waiting) ++system.parked;
    report_memory();
}
bool TaskSystem::Impl::Worker::step() {
    for (auto& slot : slots) {
        if (slot->phase == Phase::Waiting && slot->waiting->pending.load(std::memory_order_acquire) == 0) {
            slot->waiting.reset(); --system.parked; resume(*slot); return true;
        }
    }
    Job job;
    if (!system.pop(job)) return false;
    for (auto& slot : slots) if (slot->phase == Phase::Free) {
        slot->job = std::move(job); resume(*slot); return true;
    }
    // Exhausted stacks cannot prevent descendants from running on the worker's root stack.
    ++system.fallbacks;
    ++inlineDepth;
    if (inlineDepth > system.desc.maxInlineDepth && !job.stop.stop_requested()) {
        Job rejected{TaskFunction([] { throw std::runtime_error("Nested task inline-depth limit exceeded"); }),
                     std::move(job.group), {}};
        job.function.reset(); execute(rejected, rootScratch);
    } else execute(job, rootScratch);
    --inlineDepth;
    report_memory();
    return true;
}
void TaskSystem::Impl::wait_worker(Worker& worker, const std::shared_ptr<TaskGroup::State>& state) {
    if (worker.current) {
        auto& slot = *worker.current;
        slot.activeScratch = worker.scratch;
        slot.waiting = state;
        slot.phase = Phase::Waiting;
        worker.session->switch_between(*slot.fiber, worker.session->root());
    } else {
        while (state->pending.load(std::memory_order_acquire)) {
            size_t observed;
            { std::lock_guard lock(mutex); observed = generation; }
            if (worker.step()) continue;
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return generation != observed || state->pending.load(std::memory_order_acquire) == 0; });
        }
    }
}
void TaskSystem::Impl::worker_main(Worker& worker) {
    local = &worker;
    try {
        worker.session = std::make_unique<FiberThread>(desc.backend);
        worker.backend.store(worker.session->backend());
        for (size_t i = 0; i < desc.fibersPerWorker; ++i) {
            auto slot = std::make_unique<Slot>(worker, desc.scratchBytes);
            slot->fiber = std::make_unique<Fiber>(worker.session->backend(), desc.stackBytes, &fiber_main, slot.get());
            worker.slots.push_back(std::move(slot));
        }
    } catch (...) {
        std::lock_guard lock(mutex); if (!startupError) startupError = std::current_exception();
    }
    {
        std::lock_guard lock(mutex); ++ready; ++generation;
    }
    changed.notify_all();
    for (;;) {
        size_t observed;
        { std::lock_guard lock(mutex); if (startupError) break; observed = generation; }
        if (worker.step()) continue;
        std::unique_lock lock(mutex);
        if (stopping && pending.load(std::memory_order_acquire) == 0) break;
        changed.wait(lock, [&] { return generation != observed || (stopping && pending.load(std::memory_order_acquire) == 0); });
    }
    worker.slots.clear(); worker.session.reset(); local = nullptr;
}

TaskSystem::TaskSystem(TaskSystemDesc desc, Profiler* profiler) {
    if (!desc.workers) desc.workers = std::clamp(size_t(std::max(1u, std::thread::hardware_concurrency())) - 1, size_t(1), size_t(4));
    if (desc.workers > 64 || !desc.fibersPerWorker || desc.fibersPerWorker > 1024 ||
        desc.stackBytes < 65536 || !desc.scratchBytes || !desc.maxInlineDepth)
        throw std::invalid_argument("Invalid task system configuration");
    impl_ = std::make_unique<Impl>(desc, profiler);
    try {
        for (size_t i = 0; i < desc.workers; ++i) impl_->workers.push_back(std::make_unique<Impl::Worker>(*impl_));
        for (auto& worker : impl_->workers) impl_->threads.emplace_back([&, p = worker.get()] { impl_->worker_main(*p); });
        std::unique_lock lock(impl_->mutex);
        impl_->changed.wait(lock, [&] { return impl_->ready == desc.workers; });
        if (impl_->startupError) std::rethrow_exception(impl_->startupError);
    } catch (...) { shutdown(); throw; }
}
TaskSystem::~TaskSystem() { shutdown(); }
void TaskSystem::shutdown() {
    if (!impl_) return;
    if (Impl::local && &Impl::local->system == impl_.get()) throw std::logic_error("A task cannot shut down its own scheduler");
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; ++impl_->generation; }
    impl_->changed.notify_all();
    for (auto& thread : impl_->threads) if (thread.joinable()) thread.join();
    impl_->threads.clear();
}
void TaskSystem::submit_impl(TaskGroup& group, TaskFunction function, TaskPriority priority, std::stop_token stop) {
    const size_t index = static_cast<size_t>(priority);
    if (index >= impl_->queues.size()) throw std::invalid_argument("Invalid task priority");
    auto state = group.state_;
    std::lock_guard lock(impl_->mutex);
    const bool nested = Impl::local && &Impl::local->system == impl_.get();
    if (impl_->stopping && !nested) throw std::logic_error("Task system is shutting down");
    ++state->pending;
    try {
        Impl::Job job{std::move(function), state, stop};
        if (nested) impl_->queues[index].push_front(std::move(job));
        else impl_->queues[index].push_back(std::move(job));
    } catch (...) { --state->pending; throw; }
    ++impl_->pending; ++impl_->queued; ++impl_->generation;
    impl_->changed.notify_one();
}
void TaskSystem::wait(TaskGroup& group, const std::function<void()>& idlePump) {
    auto state = group.state_;
    if (Impl::local && &Impl::local->system == impl_.get()) {
        if (Impl::local->activeGroup == state.get()) throw std::logic_error("Task cannot wait on its own group");
        if (state->pending.load(std::memory_order_acquire)) impl_->wait_worker(*Impl::local, state);
    }
    else {
        std::optional<Profiler::Scope> waitScope;
        if (impl_->profiler) waitScope.emplace(*impl_->profiler, CpuZone::TaskWait);
        std::unique_lock lock(state->mutex);
        while (state->pending.load(std::memory_order_acquire)) {
            if (!idlePump) state->changed.wait(lock, [&] { return state->pending.load(std::memory_order_acquire) == 0; });
            else {
                state->changed.wait_for(lock, std::chrono::milliseconds(10));
                lock.unlock(); idlePump(); lock.lock();
            }
        }
    }
    std::lock_guard lock(state->mutex);
    if (state->error) std::rethrow_exception(state->error);
}
std::pmr::memory_resource* TaskSystem::scratch() {
    if (!Impl::local || !Impl::local->scratch) throw std::logic_error("Task scratch is only available inside a task");
    return Impl::local->scratch;
}
TaskStatistics TaskSystem::statistics() const {
    TaskStatistics result;
    result.workers = impl_->desc.workers; result.fibers = result.workers * impl_->desc.fibersPerWorker;
    result.queued = impl_->queued; result.running = impl_->running; result.parked = impl_->parked;
    result.pending = impl_->pending; result.completed = impl_->completed; result.inlineFallbacks = impl_->fallbacks;
    result.cancelled = impl_->cancelled; result.poolCommitted = impl_->memory.statistics().committed;
    for (const auto& worker : impl_->workers) {
        result.scratchCommitted += worker->committed.load(); result.scratchPeak += worker->peak.load();
    }
    result.backend = impl_->workers.empty() ? impl_->desc.backend : impl_->workers.front()->backend.load();
    return result;
}
}
