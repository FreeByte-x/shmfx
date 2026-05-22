#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_header.h"

#include <cstdint>
#include <pthread.h>
#include <string_view>

namespace shmfx {

/// RAII guard for a robust process-shared pthread mutex in shared memory.
///
/// The guard deliberately does not call pthread_mutex_consistent()
/// automatically. When prior_owner_dead() is true, callers must recover the
/// domain-specific invariant protected by the mutex, then call
/// mark_consistent() before leaving the critical section.
class ShmMutexGuard {
public:
    /// Locks a robust mutex and records whether the prior owner died.
    ///
    /// @param mutex Robust process-shared mutex stored in shared memory.
    explicit ShmMutexGuard(pthread_mutex_t& mutex) noexcept;

    /// Unlocks the mutex when it was successfully acquired.
    ~ShmMutexGuard();

    ShmMutexGuard(const ShmMutexGuard&) = delete;
    ShmMutexGuard& operator=(const ShmMutexGuard&) = delete;

    /// Moves a guard, transferring unlock responsibility.
    ///
    /// @param other Source guard.
    ShmMutexGuard(ShmMutexGuard&& other) noexcept;

    /// Moves a guard, unlocking the current mutex first if needed.
    ///
    /// @param other Source guard.
    /// @return This guard.
    ShmMutexGuard& operator=(ShmMutexGuard&& other) noexcept;

    /// Reports whether the mutex is currently locked by this guard.
    ///
    /// @return true when this guard owns the mutex.
    [[nodiscard]] bool locked() const noexcept;

    /// Reports whether pthread_mutex_lock() returned EOWNERDEAD.
    ///
    /// @return true when caller must recover protected state.
    [[nodiscard]] bool prior_owner_dead() const noexcept;

    /// Returns the lock status.
    ///
    /// @return ErrorCode::Ok for normal lock, ErrorCode::PriorOwnerDead for a
    /// recoverable robust-mutex owner death, or another error when not locked.
    [[nodiscard]] ErrorCode error() const noexcept;

    /// Marks a recovered robust mutex as consistent.
    ///
    /// @return Success when the mutex is now consistent, otherwise an error.
    [[nodiscard]] Result<void> mark_consistent() noexcept;

private:
    /// Unlocks the owned mutex if present.
    void reset() noexcept;

    pthread_mutex_t* mutex_ = nullptr;
    bool locked_ = false;
    bool prior_owner_dead_ = false;
    ErrorCode error_ = ErrorCode::Ok;
};

/// Returns CLOCK_MONOTONIC in nanoseconds.
///
/// @return Monotonic timestamp in nanoseconds, or 0 if clock_gettime fails.
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;

/// Reads /proc/<pid>/stat start_time field 22.
///
/// @param pid Process id to inspect.
/// @param out_start_time Receives field 22 when parsing succeeds.
/// @return true when start_time was read and parsed.
[[nodiscard]] bool read_proc_start_time(std::uint32_t pid,
                                        std::uint64_t& out_start_time) noexcept;

/// Checks whether a segment owner is dead according to pid, start_time, and heartbeat.
///
/// @param header Segment header to inspect.
/// @param now_mono_ns Current CLOCK_MONOTONIC timestamp in nanoseconds.
/// @return true when the owner should be considered dead.
[[nodiscard]] bool owner_is_dead(const ShmHeader& header, std::uint64_t now_mono_ns) noexcept;

/// Registers an owner header for the process-wide heartbeat worker.
///
/// @param header Header owned by this process.
void register_heartbeat_owner(ShmHeader& header);

/// Removes an owner header from the process-wide heartbeat worker.
///
/// @param header Header previously registered with register_heartbeat_owner().
void unregister_heartbeat_owner(ShmHeader& header) noexcept;

/// Checks whether any live process still maps a POSIX shm object.
///
/// @param name POSIX shm object name such as /shmfx.app.foo.
/// @return true when /proc/*/maps contains a live mapping for the object.
[[nodiscard]] bool any_process_maps_segment(std::string_view name);

} // namespace shmfx
