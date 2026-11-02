#include "memory_resources.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mirabilis {
namespace {
bool power_of_two(size_t value) { return value && !(value & (value - 1)); }
bool add(size_t a, size_t b, size_t& result) {
    if (b > std::numeric_limits<size_t>::max() - a) return false;
    result = a + b; return true;
}
size_t padding(const void* address, size_t alignment) {
    return (size_t(0) - reinterpret_cast<uintptr_t>(address)) & (alignment - 1);
}
}

size_t virtual_memory::page_size() noexcept {
    static const size_t result = [] {
#ifdef _WIN32
        SYSTEM_INFO info{}; GetSystemInfo(&info); return size_t(info.dwPageSize);
#else
        return size_t(sysconf(_SC_PAGESIZE));
#endif
    }();
    return result;
}
size_t virtual_memory::page_round(size_t bytes) noexcept {
    const size_t mask = page_size() - 1;
    if (!bytes || bytes > std::numeric_limits<size_t>::max() - mask) return 0;
    return (bytes + mask) & ~mask;
}
void* virtual_memory::reserve(size_t bytes) noexcept {
    if (!bytes) return nullptr;
#ifdef _WIN32
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* result = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return result == MAP_FAILED ? nullptr : result;
#endif
}
bool virtual_memory::commit(void* address, size_t bytes) noexcept {
    if (!bytes) return true;
#ifdef _WIN32
    return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}
bool virtual_memory::decommit(void* address, size_t bytes) noexcept {
    if (!bytes) return true;
#ifdef _WIN32
    return VirtualFree(address, bytes, MEM_DECOMMIT) != FALSE;
#else
    return madvise(address, bytes, MADV_DONTNEED) == 0 && mprotect(address, bytes, PROT_NONE) == 0;
#endif
}
void virtual_memory::release(void* address, size_t bytes) noexcept {
    if (!address) return;
#ifdef _WIN32
    (void)bytes; VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, bytes);
#endif
}

VirtualArena::VirtualArena(size_t reserveBytes) {
    stats_.reserved = virtual_memory::page_round(reserveBytes);
    base_ = static_cast<std::byte*>(virtual_memory::reserve(stats_.reserved));
    if (!base_) throw std::bad_alloc();
}
VirtualArena::~VirtualArena() { virtual_memory::release(base_, stats_.reserved); }
bool VirtualArena::owns(const void* pointer) const noexcept {
    const auto address = reinterpret_cast<uintptr_t>(pointer), base = reinterpret_cast<uintptr_t>(base_);
    return address >= base && address - base < stats_.reserved;
}
bool VirtualArena::ensure_committed(size_t end) noexcept {
    if (end <= stats_.committed) return true;
    if (end > stats_.reserved) return false;
    size_t target = virtual_memory::page_round(end);
    const size_t step = std::min(size_t(65536), stats_.reserved - stats_.committed);
    target = std::max(target, stats_.committed + step);
    if (!virtual_memory::commit(base_ + stats_.committed, target - stats_.committed)) return false;
    stats_.committed = target; return true;
}
void* VirtualArena::try_allocate(size_t bytes, size_t alignment) noexcept {
    if (!power_of_two(alignment)) { ++stats_.failures; return nullptr; }
    bytes = std::max(bytes, size_t(1));
    const size_t gap = padding(base_ + stats_.used, alignment);
    if (gap > stats_.reserved - stats_.used || bytes > stats_.reserved - stats_.used - gap) {
        ++stats_.failures; return nullptr;
    }
    const size_t start = stats_.used + gap, end = start + bytes;
    if (!ensure_committed(end)) { ++stats_.failures; return nullptr; }
    stats_.used = end; stats_.peak = std::max(stats_.peak, end); ++stats_.allocations;
    last_ = start; lastSize_ = bytes;
    return base_ + start;
}
void* VirtualArena::do_allocate(size_t bytes, size_t alignment) {
    if (void* result = try_allocate(bytes, alignment)) return result;
    throw std::bad_alloc();
}
void* VirtualArena::try_reallocate(void* old, size_t oldBytes, size_t newBytes, size_t alignment) noexcept {
    if (!old) return try_allocate(newBytes, alignment);
    const auto address = reinterpret_cast<uintptr_t>(old), begin = reinterpret_cast<uintptr_t>(base_);
    if (!power_of_two(alignment) || address < begin || address - begin > stats_.used ||
        oldBytes > stats_.used - (address - begin)) { ++stats_.failures; return nullptr; }
    newBytes = std::max(newBytes, size_t(1));
    if (old == base_ + last_ && oldBytes == lastSize_ && (address & (alignment - 1)) == 0) {
        if (newBytes > stats_.reserved - last_ || !ensure_committed(last_ + newBytes)) {
            ++stats_.failures; return nullptr;
        }
        stats_.used = last_ + newBytes; lastSize_ = newBytes;
        stats_.peak = std::max(stats_.peak, stats_.used); return old;
    }
    void* replacement = try_allocate(newBytes, alignment);
    if (!replacement) return nullptr;
    std::memcpy(replacement, old, std::min(oldBytes, newBytes));
    return replacement;
}
VirtualArena::Marker VirtualArena::mark() const noexcept { return {this, stats_.used, last_, lastSize_, generation_}; }
bool VirtualArena::rewind(Marker marker) noexcept {
    if (marker.owner != this || marker.generation != generation_ || marker.offset > stats_.used ||
        marker.last > marker.offset || marker.lastSize > marker.offset - marker.last) return false;
    stats_.used = marker.offset; last_ = marker.last; lastSize_ = marker.lastSize; return true;
}
void VirtualArena::reset(bool decommit) noexcept {
    stats_.used = last_ = lastSize_ = 0; ++generation_;
    if (decommit && virtual_memory::decommit(base_, stats_.committed)) stats_.committed = 0;
}

struct FixedBlockPool::Chunk {
    Chunk* next;
    size_t bytes;
    std::byte* first;
    unsigned char* allocated;
};
FixedBlockPool::FixedBlockPool(size_t bytes, size_t alignment, size_t slots)
    : blockBytes_(std::max(bytes, sizeof(Free))), alignment_(std::max(alignment, alignof(Free))), slots_(slots) {
    if (!power_of_two(alignment) || !slots || blockBytes_ > SIZE_MAX - (alignment_ - 1))
        throw std::invalid_argument("Invalid fixed pool geometry");
    stride_ = (blockBytes_ + alignment_ - 1) & ~(alignment_ - 1);
    if (slots_ > SIZE_MAX / stride_) throw std::invalid_argument("Fixed pool is too large");
}
FixedBlockPool::~FixedBlockPool() {
    while (chunks_) { auto* next = chunks_->next; virtual_memory::release(chunks_, chunks_->bytes); chunks_ = next; }
}
bool FixedBlockPool::grow() noexcept {
    size_t bytes, header;
    if (!add(sizeof(Chunk), slots_, header) || !add(header, alignment_ - 1, header) ||
        !add(header, stride_ * slots_, bytes) || !(bytes = virtual_memory::page_round(bytes))) return false;
    auto* memory = static_cast<std::byte*>(virtual_memory::reserve(bytes));
    if (!memory) return false;
    if (!virtual_memory::commit(memory, bytes)) { virtual_memory::release(memory, bytes); return false; }
    auto* chunk = new (memory) Chunk{chunks_, bytes, nullptr, reinterpret_cast<unsigned char*>(memory + sizeof(Chunk))};
    std::memset(chunk->allocated, 0, slots_);
    auto* start = memory + sizeof(Chunk) + slots_;
    chunk->first = start + padding(start, alignment_);
    chunks_ = chunk;
    for (size_t i = slots_; i-- > 0;) {
        auto* slot = reinterpret_cast<Free*>(chunk->first + i * stride_);
        slot->next = free_; free_ = slot;
    }
    stats_.reserved += bytes; stats_.committed += bytes;
    return true;
}
FixedBlockPool::Chunk* FixedBlockPool::find(const void* pointer, size_t& index) const noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    for (auto* chunk = chunks_; chunk; chunk = chunk->next) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(chunk->first);
        if (address >= first && address - first < slots_ * stride_ && (address - first) % stride_ == 0) {
            index = (address - first) / stride_; return chunk;
        }
    }
    return nullptr;
}
void* FixedBlockPool::try_allocate(size_t bytes, size_t alignment) noexcept {
    std::lock_guard lock(mutex_);
    if (!power_of_two(alignment) || bytes > blockBytes_ || alignment > alignment_ || (!free_ && !grow())) {
        ++stats_.failures; return nullptr;
    }
    Free* result = free_; free_ = free_->next;
    size_t index = 0; auto* chunk = find(result, index); assert(chunk);
    chunk->allocated[index] = 1;
    stats_.used += blockBytes_; stats_.peak = std::max(stats_.peak, stats_.used); ++stats_.allocations;
    return result;
}
bool FixedBlockPool::release(void* pointer) noexcept {
    if (!pointer) return true;
    std::lock_guard lock(mutex_);
    size_t index = 0; auto* chunk = find(pointer, index);
    if (!chunk || !chunk->allocated[index]) return false;
    chunk->allocated[index] = 0;
    auto* slot = static_cast<Free*>(pointer); slot->next = free_; free_ = slot;
    stats_.used -= blockBytes_; return true;
}
void* FixedBlockPool::do_allocate(size_t bytes, size_t alignment) {
    if (void* result = try_allocate(bytes, alignment)) return result;
    throw std::bad_alloc();
}
void FixedBlockPool::do_deallocate(void* pointer, size_t, size_t) {
    const bool valid = release(pointer); assert(valid); (void)valid;
}
MemoryStatistics FixedBlockPool::statistics() const noexcept { std::lock_guard lock(mutex_); return stats_; }

SizeClassResource::SizeClassResource() {
    for (size_t i = 0; i < pools_.size(); ++i) pools_[i] = std::make_unique<FixedBlockPool>(size_t(16) << i, 16);
}
size_t SizeClassResource::index(size_t bytes) noexcept {
    size_t result = 0, size = 16;
    while (size < bytes) { size <<= 1; ++result; }
    return result;
}
void* SizeClassResource::do_allocate(size_t bytes, size_t alignment) {
    if (bytes <= 2048 && alignment <= 16) return pools_[index(bytes)]->allocate(bytes, alignment);
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
}
void SizeClassResource::do_deallocate(void* pointer, size_t bytes, size_t alignment) {
    if (bytes <= 2048 && alignment <= 16) pools_[index(bytes)]->deallocate(pointer, bytes, alignment);
    else std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
}
MemoryStatistics SizeClassResource::statistics() const noexcept {
    MemoryStatistics result;
    for (const auto& pool : pools_) {
        const auto part = pool->statistics();
        result.reserved += part.reserved; result.committed += part.committed; result.used += part.used;
        result.peak += part.peak; result.allocations += part.allocations; result.failures += part.failures;
    }
    return result;
}
}
