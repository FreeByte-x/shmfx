#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_manager.h"
#include "shmfx/shm_ring.h"

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <span>
#include <string>
#include <string_view>

namespace shmfx_logging {

/// Logging record levels stored in the core ring record type field.
enum class LogLevel : std::uint16_t {
    /// High-volume diagnostic detail.
    Trace = 0,
    /// Developer-oriented diagnostic message.
    Debug = 1,
    /// Normal operational message.
    Info = 2,
    /// Unexpected but recoverable condition.
    Warn = 3,
    /// Operation failed but process can continue.
    Error = 4,
    /// Process or service cannot continue reliably.
    Fatal = 5,
};

/// Logging-app record flags stored in the core ring record flags field.
enum LogRecordFlags : std::uint16_t {
    /// Payload was truncated by the core ring slot capacity.
    Truncated = shmfx::RECORD_FLAG_TRUNCATED,
    /// Payload is binary structured data rather than formatted UTF-8 text.
    Binary = 1u << 1,
};

/// Producer-side logger configuration.
struct LoggerOptions {
    /// Service name used in the default shm name `/shmfx.log.<service>_<pid>`.
    std::string service_name;
    /// Optional explicit POSIX shm name; when empty the default name is generated.
    std::string segment_name;
    /// Bounded ring slot count.
    std::uint32_t record_max = 4096;
    /// Maximum formatted payload bytes before ring truncation policy applies.
    std::uint32_t max_payload = 1024;
    /// POSIX permissions for the producer ring segment.
    mode_t perm = 0600;
    /// Extra SegmentFlags for the underlying shmfx segment.
    std::uint32_t flags = 0;
};

/// Producer API for the reference distributed logging app.
class Logger {
public:
    /// Initializes or attaches a per-process logging ring for a service.
    ///
    /// @param service_name Service identifier used in the generated segment name.
    /// @return Logger instance or framework error.
    [[nodiscard]] static shmfx::Result<Logger> init(std::string_view service_name);

    /// Initializes or attaches a logging ring with explicit options.
    ///
    /// @param options Producer logger configuration.
    /// @return Logger instance or framework error.
    [[nodiscard]] static shmfx::Result<Logger> init(const LoggerOptions& options);

    /// Moves a logger and transfers the underlying mapping.
    ///
    /// @param other Source logger.
    Logger(Logger&& other) noexcept;

    /// Moves a logger and releases the previous mapping.
    ///
    /// @param other Source logger.
    /// @return This logger.
    Logger& operator=(Logger&& other) noexcept;

    /// Copying is disabled because a Logger owns one shm mapping handle.
    Logger(const Logger&) = delete;

    /// Copy assignment is disabled because a Logger owns one shm mapping handle.
    Logger& operator=(const Logger&) = delete;

    /// Releases the mapped ring segment.
    ~Logger();

    /// Formats and publishes one non-blocking text log record.
    ///
    /// @param level Logging severity.
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error; never blocks for consumer progress.
    [[nodiscard]] shmfx::Result<void> log(LogLevel level, const char* fmt, ...) noexcept;

    /// Publishes one already formatted text payload.
    ///
    /// @param level Logging severity.
    /// @param message UTF-8/text payload bytes.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> log_text(LogLevel level,
                                               std::string_view message) noexcept;

    /// Publishes one binary structured payload.
    ///
    /// @param level Logging severity.
    /// @param payload Binary payload bytes.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> log_binary(LogLevel level,
                                                 std::span<const std::byte> payload) noexcept;

    /// Formats and publishes a TRACE record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> trace(const char* fmt, ...) noexcept;

    /// Formats and publishes a DEBUG record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> debug(const char* fmt, ...) noexcept;

    /// Formats and publishes an INFO record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> info(const char* fmt, ...) noexcept;

    /// Formats and publishes a WARN record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> warn(const char* fmt, ...) noexcept;

    /// Formats and publishes an ERROR record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> error(const char* fmt, ...) noexcept;

    /// Formats and publishes a FATAL record.
    ///
    /// @param fmt printf-compatible format string.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> fatal(const char* fmt, ...) noexcept;

    /// Returns the backing POSIX shm segment name.
    ///
    /// @return Segment name published to the shmfx registry.
    [[nodiscard]] std::string_view segment_name() const noexcept;

    /// Returns the current producer-side drop counter from the core ring.
    ///
    /// @return Number of records dropped by non-blocking ring push attempts.
    [[nodiscard]] std::uint64_t lost_count() const noexcept;

private:
    /// Constructs a logger from initialized framework resources.
    ///
    /// @param handle Mapped shmfx ring segment.
    /// @param ring Bound MPSC ring view.
    /// @param segment_name Stable segment name string.
    Logger(shmfx::ShmHandle handle, shmfx::MpscRing ring, std::string segment_name) noexcept;

    /// Formats a varargs message and publishes it at a fixed level.
    ///
    /// @param level Logging severity.
    /// @param fmt printf-compatible format string.
    /// @param args Varargs list matching @p fmt.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> vlog(LogLevel level,
                                           const char* fmt,
                                           va_list args) noexcept;

    /// Publishes one payload with explicit logging flags.
    ///
    /// @param level Logging severity.
    /// @param flags Logging record flags.
    /// @param payload Payload bytes.
    /// @return Success or RingFull/validation error.
    [[nodiscard]] shmfx::Result<void> publish(LogLevel level,
                                              std::uint16_t flags,
                                              std::span<const std::byte> payload) noexcept;

    shmfx::ShmHandle handle_{};
    shmfx::MpscRing ring_;
    std::string segment_name_;
};

} // namespace shmfx_logging
