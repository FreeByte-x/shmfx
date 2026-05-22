#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_manager.h"
#include "shmfx/shm_ring.h"
#include "shmfx_logging/logger.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shmfx_logging {

/// Decoded logging record delivered by LogCenter to its sink.
struct LogRecord {
    /// Source shm segment name that produced the record.
    std::string source;
    /// Logging severity decoded from the core record type.
    LogLevel level = LogLevel::Info;
    /// Logging-app flags decoded from the core record flags field.
    std::uint16_t flags = 0;
    /// Producer CLOCK_MONOTONIC timestamp in nanoseconds.
    std::uint64_t timestamp_ns = 0;
    /// Text or binary payload copied out of the shared ring slot.
    std::string payload;
};

/// Consumer-side LogCenter configuration.
struct LogCenterOptions {
    /// Registry prefix watched for logging producer rings.
    std::string prefix = "/shmfx.log.";
    /// Maximum records drained from one ring per pass.
    std::uint32_t drain_batch = 256;
    /// Scratch buffer size used when copying one record payload.
    std::uint32_t scratch_bytes = 4096;
};

/// Reference logging consumer that discovers and drains producer MPSC rings.
class LogCenter {
public:
    /// Sink callback invoked synchronously for each drained record.
    using Sink = std::function<void(const LogRecord&)>;

    /// Creates a LogCenter with default options and stdout-like no-op sink.
    LogCenter();

    /// Creates a LogCenter with explicit options.
    ///
    /// @param options Consumer behavior options.
    explicit LogCenter(LogCenterOptions options);

    /// Moves a LogCenter and its attached ring mappings.
    ///
    /// @param other Source LogCenter.
    LogCenter(LogCenter&& other) noexcept;

    /// Moves a LogCenter and releases previous attached mappings.
    ///
    /// @param other Source LogCenter.
    /// @return This LogCenter.
    LogCenter& operator=(LogCenter&& other) noexcept;

    /// Copying is disabled because LogCenter owns shm mapping handles.
    LogCenter(const LogCenter&) = delete;

    /// Copy assignment is disabled because LogCenter owns shm mapping handles.
    LogCenter& operator=(const LogCenter&) = delete;

    /// Releases attached ring mappings.
    ~LogCenter();

    /// Replaces the record sink.
    ///
    /// @param sink Callback invoked for every drained record.
    void set_sink(Sink sink);

    /// Discovers registry entries matching the configured prefix and attaches new rings.
    ///
    /// LogCenter intentionally attaches ReadWrite so it participates in lifecycle
    /// ref_count and can act as a co-owner while draining producer rings.
    ///
    /// @return Success or first attach/bind error.
    [[nodiscard]] shmfx::Result<void> discover_once();

    /// Drains currently attached rings once.
    ///
    /// Rings whose header state is DEAD are drained once more and then detached.
    ///
    /// @return Number of records delivered to the sink.
    [[nodiscard]] std::uint32_t drain_once();

    /// Runs one discover+drain polling iteration with adaptive idle backoff.
    ///
    /// @return Number of records delivered to the sink.
    [[nodiscard]] std::uint32_t poll_once();

    /// Returns the number of currently attached producer rings.
    ///
    /// @return Attached ring count.
    [[nodiscard]] std::size_t attached_count() const noexcept;

private:
    struct AttachedRing {
        /// Source shm segment name.
        std::string name;
        /// RW mapping so LogCenter participates in lifecycle ref_count.
        shmfx::ShmHandle handle;
        /// Bound MPSC ring view over the mapped segment payload.
        shmfx::MpscRing ring;
        /// Whether this ring should be detached after the current drain pass.
        bool remove_after_drain = false;

        /// Constructs an attached ring record from mapped resources.
        ///
        /// @param ring_name Stable segment name.
        /// @param ring_handle Mapped shmfx handle.
        /// @param bound_ring Bound MPSC ring view.
        AttachedRing(std::string ring_name,
                     shmfx::ShmHandle ring_handle,
                     shmfx::MpscRing bound_ring) noexcept;
    };

    /// Attaches one named ring if it is not already attached.
    ///
    /// @param name POSIX shm segment name.
    /// @return Success or first attach/bind error.
    [[nodiscard]] shmfx::Result<void> attach_ring(std::string_view name);

    /// Drains one attached ring and optionally marks it removable.
    ///
    /// @param ring Attached ring state.
    /// @return Number of records delivered to the sink.
    std::uint32_t drain_ring(AttachedRing& ring);

    LogCenterOptions options_{};
    Sink sink_{};
    std::vector<AttachedRing> rings_{};
    shmfx::AdaptiveBackoff backoff_{};
};

} // namespace shmfx_logging
