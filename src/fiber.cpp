#include "fiber.h"
#include "memory_resources.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifdef MIRABILIS_FIBER_ASM_WIN64
extern "C" void mirabilis_fiber_switch(mirabilis::FiberContext*, mirabilis::FiberContext*);
extern "C" void mirabilis_fiber_start();
#endif

namespace mirabilis {
bool FiberThread::assembly_available() noexcept {
#ifdef MIRABILIS_FIBER_ASM_WIN64
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &policy, sizeof(policy)) &&
           !policy.EnableUserShadowStack;
#else
    return false;
#endif
}

FiberThread::FiberThread(FiberBackend requested)
    : backend_(requested == FiberBackend::Automatic ? (assembly_available() ? FiberBackend::Assembly : FiberBackend::Windows) : requested) {
    root_.backend_ = backend_;
    if (backend_ == FiberBackend::Assembly) {
        if (!assembly_available()) throw std::runtime_error("Assembly fibers unavailable under this process stack-protection policy");
    } else {
#ifdef _WIN32
        if (IsThreadAFiber()) root_.handle_ = GetCurrentFiber();
        else {
            root_.handle_ = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            converted_ = root_.handle_ != nullptr;
        }
        if (!root_.handle_) throw std::runtime_error("Could not convert job worker to a fiber");
#else
        throw std::runtime_error("Fiber backend requires Windows x64");
#endif
    }
}
FiberThread::~FiberThread() {
#ifdef _WIN32
    if (converted_) { const bool reverted = ConvertFiberToThread() != FALSE; assert(reverted); (void)reverted; }
#endif
}

Fiber::Fiber(FiberBackend backend, size_t bytes, Entry entry, void* argument)
    : backend_(backend), entry_(entry), argument_(argument) {
    if (backend == FiberBackend::Automatic || !entry || bytes < 65536)
        throw std::invalid_argument("Invalid fiber configuration");
    stackBytes_ = virtual_memory::page_round(bytes);
    if (!stackBytes_ || stackBytes_ > SIZE_MAX - virtual_memory::page_size()) throw std::bad_alloc();
    if (backend_ == FiberBackend::Windows) {
#ifdef _WIN32
        handle_ = CreateFiberEx(stackBytes_, stackBytes_, FIBER_FLAG_FLOAT_SWITCH,
            reinterpret_cast<LPFIBER_START_ROUTINE>(&Fiber::windows_entry), this);
        if (!handle_) throw std::bad_alloc();
#else
        throw std::runtime_error("Windows fibers unavailable");
#endif
    } else {
#ifdef MIRABILIS_FIBER_ASM_WIN64
        if (!FiberThread::assembly_available()) throw std::runtime_error("Assembly fiber stack policy changed");
        reservationBytes_ = stackBytes_ + virtual_memory::page_size();
        reservation_ = virtual_memory::reserve(reservationBytes_);
        if (!reservation_) throw std::bad_alloc();
        auto* usable = static_cast<std::byte*>(reservation_) + virtual_memory::page_size();
        if (!virtual_memory::commit(usable, stackBytes_)) {
            virtual_memory::release(reservation_, reservationBytes_); throw std::bad_alloc();
        }
        auto* top = usable + stackBytes_;
        context_.rsp = top - 40;
        context_.rip = reinterpret_cast<void*>(&mirabilis_fiber_start);
        context_.r12 = reinterpret_cast<uint64_t>(entry);
        context_.r13 = reinterpret_cast<uint64_t>(argument);
        context_.stackBase = top; context_.stackLimit = usable;
        context_.deallocationStack = reservation_;
        context_.fiberData = GetCurrentFiber();
        *static_cast<uint64_t*>(context_.rsp) = 0;
#else
        throw std::runtime_error("Assembly fiber backend was not built");
#endif
    }
}
Fiber::~Fiber() {
#ifdef _WIN32
    if (entry_ && handle_) DeleteFiber(handle_);
#endif
    virtual_memory::release(reservation_, reservationBytes_);
}
void Fiber::windows_entry(void* self) {
    auto& fiber = *static_cast<Fiber*>(self);
    fiber.entry_(fiber.argument_);
    std::abort();
}
void FiberThread::switch_between(Fiber& from, Fiber& to) noexcept {
    assert(from.backend_ == backend_ && to.backend_ == backend_);
    assert(from.owner_ == std::this_thread::get_id() && to.owner_ == std::this_thread::get_id());
    if (&from == &to) return;
    if (backend_ == FiberBackend::Assembly) {
#ifdef MIRABILIS_FIBER_ASM_WIN64
        mirabilis_fiber_switch(&from.context_, &to.context_);
#endif
    } else {
#ifdef _WIN32
        SwitchToFiber(to.handle_);
#endif
    }
}
}
