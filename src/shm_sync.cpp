#include "shmfx/shm_sync.h"

#include <chrono>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace shmfx {

std::uint64_t atomic_load_u64(const std::uint64_t& storage,
                              std::memory_order order) noexcept {
    return std::atomic_ref<const std::uint64_t>(storage).load(order);
}

void atomic_store_u64(std::uint64_t& storage,
                      std::uint64_t value,
                      std::memory_order order) noexcept {
    std::atomic_ref<std::uint64_t>(storage).store(value, order);
}

std::uint64_t atomic_fetch_add_u64(std::uint64_t& storage,
                                   std::uint64_t value,
                                   std::memory_order order) noexcept {
    return std::atomic_ref<std::uint64_t>(storage).fetch_add(value, order);
}

bool atomic_compare_exchange_weak_u64(std::uint64_t& storage,
                                      std::uint64_t& expected,
                                      std::uint64_t desired,
                                      std::memory_order success,
                                      std::memory_order failure) noexcept {
    return std::atomic_ref<std::uint64_t>(storage).compare_exchange_weak(
        expected, desired, success, failure);
}

void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

void AdaptiveBackoff::reset() noexcept {
    spin_count_ = 0;
}

void AdaptiveBackoff::idle() noexcept {
    ++spin_count_;
    if (spin_count_ < 64) {
        cpu_relax();
        return;
    }
    if (spin_count_ < 512) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}

std::uint32_t AdaptiveBackoff::count() const noexcept {
    return spin_count_;
}

} // namespace shmfx
