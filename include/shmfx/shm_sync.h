#pragma once

#include "shmfx/shm_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace shmfx {

/// Cache-line size used to separate hot shared-memory cursors.
inline constexpr std::size_t CACHE_LINE_SIZE = SHMFX_CACHE_LINE_BYTES;

/// Cache-line-padded cursor storage for shared-memory lock-free queues.
///
/// The field is plain storage by design. Code must access it through
/// std::atomic_ref<uint64_t> so the shared-memory ABI remains trivially
/// copyable and does not depend on std::atomic object layout.
struct alignas(CACHE_LINE_SIZE) RingCursor {
    /// Monotonic cursor value accessed through std::atomic_ref<uint64_t>.
    std::uint64_t pos_storage;
    /// Padding that prevents head/tail false sharing.
    std::uint8_t _pad[CACHE_LINE_SIZE - sizeof(std::uint64_t)];
};

/// Per-slot sequence storage for the bounded MPSC record ring.
struct RingSlotHeader {
    /// Slot sequence accessed through std::atomic_ref<uint64_t>.
    std::uint64_t seq_storage;
};

static_assert(sizeof(RingCursor) == CACHE_LINE_SIZE);
static_assert(alignof(RingCursor) == CACHE_LINE_SIZE);
static_assert(sizeof(RingSlotHeader) == sizeof(std::uint64_t));
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

/// Loads a shared uint64_t storage field atomically.
///
/// @param storage Plain shared-memory field.
/// @param order Memory ordering for the load.
/// @return Atomically loaded value.
[[nodiscard]] std::uint64_t atomic_load_u64(const std::uint64_t& storage,
                                            std::memory_order order) noexcept;

/// Stores a shared uint64_t storage field atomically.
///
/// @param storage Plain shared-memory field.
/// @param value Value to store.
/// @param order Memory ordering for the store.
void atomic_store_u64(std::uint64_t& storage,
                      std::uint64_t value,
                      std::memory_order order) noexcept;

/// Adds to a shared uint64_t storage field atomically.
///
/// @param storage Plain shared-memory field.
/// @param value Value to add.
/// @param order Memory ordering for the read-modify-write.
/// @return Value observed before the add.
std::uint64_t atomic_fetch_add_u64(std::uint64_t& storage,
                                   std::uint64_t value,
                                   std::memory_order order) noexcept;

/// Compares and swaps a shared uint64_t storage field atomically.
///
/// @param storage Plain shared-memory field.
/// @param expected Expected value; updated with the observed value on failure.
/// @param desired Replacement value when comparison succeeds.
/// @param success Memory ordering for a successful CAS.
/// @param failure Memory ordering for a failed CAS.
/// @return true when the storage was replaced with @p desired.
bool atomic_compare_exchange_weak_u64(std::uint64_t& storage,
                                      std::uint64_t& expected,
                                      std::uint64_t desired,
                                      std::memory_order success,
                                      std::memory_order failure) noexcept;

/// Emits a short CPU relax hint for adaptive polling spin phases.
void cpu_relax() noexcept;

/// Small adaptive backoff helper for consumers that poll multiple rings.
class AdaptiveBackoff {
public:
    /// Resets the backoff after useful work was observed.
    void reset() noexcept;

    /// Advances one idle iteration using spin, yield, then short sleep.
    void idle() noexcept;

    /// Returns the current idle iteration count.
    ///
    /// @return Number of consecutive idle iterations.
    [[nodiscard]] std::uint32_t count() const noexcept;

private:
    std::uint32_t spin_count_ = 0;
};

} // namespace shmfx
