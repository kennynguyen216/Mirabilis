#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace mirabilis {
enum class FiberBackend { Automatic, Assembly, Windows };

// Layout is checked against fiber_switch_win64.asm. Windows x64 nonvolatile state.
struct alignas(16) FiberContext {
    void* rsp{}; void* rip{};
    uint64_t rbx{}, rbp{}, rsi{}, rdi{}, r12{}, r13{}, r14{}, r15{};
    void* stackBase{}; void* stackLimit{}; void* deallocationStack{}; void* fiberData{};
    uint32_t mxcsr{0x1f80}; uint16_t x87{0x037f};
    std::byte padding[10]{};
    std::array<std::array<std::byte, 16>, 10> xmm{};
};
static_assert(offsetof(FiberContext, stackBase) == 80);
static_assert(offsetof(FiberContext, mxcsr) == 112);
static_assert(offsetof(FiberContext, xmm) == 128 && sizeof(FiberContext) == 288);

class FiberThread;
class Fiber {
public:
    using Entry = void (*)(void*);
    Fiber(FiberBackend backend, size_t stackBytes, Entry entry, void* argument);
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    size_t stack_bytes() const noexcept { return stackBytes_; }
private:
    friend class FiberThread;
    Fiber() = default;
    static void windows_entry(void* self);
    FiberContext context_{};
    FiberBackend backend_{FiberBackend::Automatic};
    void* handle_{};
    void* reservation_{};
    size_t reservationBytes_{}, stackBytes_{};
    Entry entry_{};
    void* argument_{};
    std::thread::id owner_{std::this_thread::get_id()};
};

// A worker owns this session and all its fibers. Neither jobs nor stacks migrate.
class FiberThread {
public:
    explicit FiberThread(FiberBackend requested = FiberBackend::Automatic);
    ~FiberThread();
    FiberThread(const FiberThread&) = delete;
    FiberThread& operator=(const FiberThread&) = delete;
    static bool assembly_available() noexcept;
    FiberBackend backend() const noexcept { return backend_; }
    Fiber& root() noexcept { return root_; }
    void switch_between(Fiber& from, Fiber& to) noexcept;
private:
    Fiber root_;
    FiberBackend backend_;
    bool converted_{};
};
}
