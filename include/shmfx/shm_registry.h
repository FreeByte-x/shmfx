#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_header.h"
#include "shmfx/shm_manager.h"

#include <string_view>
#include <vector>

namespace shmfx {

/// Singleton registry facade backed by the /shmfx.registry segment.
class Registry {
public:
    /// Returns the process-local registry instance, creating the backing segment if needed.
    ///
    /// @return Registry singleton.
    static Registry& instance();

    /// Registers or replaces a segment entry.
    ///
    /// @param entry Registry entry payload. seq_storage is managed by Registry.
    /// @return Success or failure code.
    [[nodiscard]] Result<void> register_segment(const RegistryEntry& entry);

    /// Removes a segment entry by name.
    ///
    /// @param name POSIX shm object name.
    /// @return Success or failure code.
    [[nodiscard]] Result<void> unregister(std::string_view name);

    /// Lists consistent registry snapshots.
    ///
    /// @param prefix Optional name prefix filter.
    /// @return Registry entries matching @p prefix.
    [[nodiscard]] std::vector<RegistryEntry> list(std::string_view prefix = {});

    /// Runs a cheap janitor pass.
    ///
    /// The pass removes stale registry entries, marks dead-owner segments
    /// DRAINING, and force-unlinks old zombies only after /proc maps scan says
    /// no process still maps the object.
    void gc();

private:
    /// Opens or creates the backing registry segment.
    Registry();

    /// Returns the mapped registry payload.
    ///
    /// @return Mutable registry payload reference.
    RegistryPayload& payload() noexcept;

    ShmHandle handle_;
};

} // namespace shmfx
