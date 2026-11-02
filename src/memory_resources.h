#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>

namespace mirabilis {

struct MemoryStatistics {
    size_t reserved{}, committed{}, used{}, peak{}, allocations{}, failures{};
};

namespace virtual_memory {
size_t page_size() noexcept;
size_t page_round(size_t bytes) noexcept;
void* reserve(size_t bytes) noexcept;
bool commit(void* address, size_t bytes) noexcept;
bool decommit(void* address, size_t bytes) noexcept;
void release(void* address, size_t bytes) noexcept;
}

// One owner at a time. Reset invalidates storage without running destructors.
class VirtualArena final : public std::pmr::memory_resource {
public:
    struct Marker { const VirtualArena* owner; size_t offset, last, lastSize; uint64_t generation; };
    explicit VirtualArena(size_t reserveBytes = 4 * 1024 * 1024);
    ~VirtualArena() override;
    VirtualArena(const VirtualArena&) = delete;
    VirtualArena& operator=(const VirtualArena&) = delete;
    void* try_allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept;
    void* try_reallocate(void* old, size_t oldBytes, size_t newBytes,
                         size_t alignment = alignof(std::max_align_t)) noexcept;
    Marker mark() const noexcept;
    bool rewind(Marker marker) noexcept;
    void reset(bool decommit = false) noexcept;
    bool owns(const void* pointer) const noexcept;
    MemoryStatistics statistics() const noexcept { return stats_; }
private:
    bool ensure_committed(size_t end) noexcept;
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void*, size_t, size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::byte* base_{};
    MemoryStatistics stats_{};
    size_t last_{}, lastSize_{};
    uint64_t generation_{1};
};

// Fixed slots retain their page chunks; invalid and double releases are rejected.
class FixedBlockPool final : public std::pmr::memory_resource {
public:
    explicit FixedBlockPool(size_t blockBytes, size_t alignment = alignof(std::max_align_t), size_t slotsPerChunk = 64);
    ~FixedBlockPool() override;
    void* try_allocate(size_t bytes, size_t alignment) noexcept;
    bool release(void* pointer) noexcept;
    MemoryStatistics statistics() const noexcept;
private:
    struct Chunk;
    struct Free { Free* next; };
    bool grow() noexcept;
    Chunk* find(const void* pointer, size_t& index) const noexcept;
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* pointer, size_t, size_t) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    size_t blockBytes_, alignment_, stride_, slots_;
    Chunk* chunks_{};
    Free* free_{};
    mutable std::mutex mutex_;
    MemoryStatistics stats_{};
};

// Small allocations use 16..2048-byte pools; large/over-aligned ones use the heap.
class SizeClassResource final : public std::pmr::memory_resource {
public:
    SizeClassResource();
    MemoryStatistics statistics() const noexcept;
private:
    static size_t index(size_t bytes) noexcept;
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* pointer, size_t bytes, size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::array<std::unique_ptr<FixedBlockPool>, 8> pools_;
};
}
