#include "shmfx_logging/logger.h"

#include "shmfx/shm_lifecycle.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <unistd.h>

namespace shmfx_logging {
namespace {

constexpr std::size_t kFormatBufferBytes = 1024;

/// Returns true when a service-name tail character is accepted.
bool is_service_tail_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// Validates the service component used after `/shmfx.log.`.
bool valid_service_name(std::string_view service) noexcept {
    if (service.empty() || service.size() > 24 || service.front() < 'a' || service.front() > 'z') {
        return false;
    }
    return std::all_of(service.begin() + 1, service.end(), is_service_tail_char);
}

/// Builds the default per-process logging segment name.
std::string default_segment_name(std::string_view service) {
    return "/shmfx.log." + std::string(service) + "_" + std::to_string(::getpid());
}

/// Returns CLOCK_MONOTONIC nanoseconds for the record user field.
std::uint64_t monotonic_record_time() noexcept {
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return shmfx::monotonic_ns();
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull * 1000ull * 1000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

/// Returns the metadata block as RecordRingMeta.
shmfx::RecordRingMeta& ring_meta(shmfx::ShmHandle& handle) noexcept {
    return *reinterpret_cast<shmfx::RecordRingMeta*>(handle.metadata().data());
}

/// Creates the underlying shm segment options for a logger.
shmfx::CreateOptions make_create_options(const LoggerOptions& options,
                                         std::string_view segment_name) noexcept {
    const std::uint32_t stride = shmfx::MpscRing::record_stride_for(options.max_payload);
    const std::uint64_t payload_size = shmfx::MpscRing::payload_size_for(options.record_max, stride);

    shmfx::CreateOptions create;
    create.name = std::string(segment_name);
    create.type = shmfx::SegmentType::RecordRing;
    create.meta_size = sizeof(shmfx::RecordRingMeta);
    create.total_size = shmfx::align_up(shmfx::SHMFX_HEADER_SIZE + create.meta_size,
                                        shmfx::SHMFX_CACHE_LINE_BYTES) +
                        payload_size;
    create.perm = options.perm;
    create.flags = options.flags | shmfx::LockfreeRing;
    return create;
}

/// Binds and validates a mapped segment as a logging MPSC ring.
shmfx::Result<shmfx::MpscRing> bind_logger_ring(shmfx::ShmHandle& handle) noexcept {
    if (static_cast<shmfx::SegmentType>(handle.header().segment_type) !=
            shmfx::SegmentType::RecordRing ||
        handle.metadata().size() < sizeof(shmfx::RecordRingMeta)) {
        return shmfx::ErrorCode::CorruptedHeader;
    }
    return shmfx::MpscRing::bind(ring_meta(handle), handle.payload());
}

} // namespace

shmfx::Result<Logger> Logger::init(std::string_view service_name) {
    LoggerOptions options;
    options.service_name = std::string(service_name);
    return init(options);
}

shmfx::Result<Logger> Logger::init(const LoggerOptions& options) {
    if (!valid_service_name(options.service_name) ||
        options.record_max == 0 ||
        options.max_payload == 0) {
        return shmfx::ErrorCode::InvalidName;
    }

    std::string segment_name = options.segment_name.empty()
                                   ? default_segment_name(options.service_name)
                                   : options.segment_name;
    if (segment_name.size() >= shmfx::SHMFX_NAME_BYTES) {
        return shmfx::ErrorCode::InvalidName;
    }

    const shmfx::CreateOptions create = make_create_options(options, segment_name);
    auto created = shmfx::ShmManager::create(create);
    if (created) {
        auto ring = shmfx::MpscRing::initialize(
            ring_meta(created.value()), created.value().payload(), options.record_max, options.max_payload);
        if (!ring) {
            [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(segment_name);
            return ring.error();
        }
        return Logger(std::move(created.value()), std::move(ring.value()), std::move(segment_name));
    }
    if (created.error() != shmfx::ErrorCode::AlreadyExists) {
        return created.error();
    }

    auto attached = shmfx::ShmManager::attach(segment_name, shmfx::AttachMode::ReadWrite);
    if (!attached) {
        return attached.error();
    }
    auto ring = bind_logger_ring(attached.value());
    if (!ring) {
        return ring.error();
    }
    return Logger(std::move(attached.value()), std::move(ring.value()), std::move(segment_name));
}

Logger::Logger(Logger&& other) noexcept = default;

Logger& Logger::operator=(Logger&& other) noexcept = default;

Logger::~Logger() = default;

shmfx::Result<void> Logger::log(LogLevel level, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(level, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::log_text(LogLevel level, std::string_view message) noexcept {
    return publish(level,
                   0,
                   {reinterpret_cast<const std::byte*>(message.data()), message.size()});
}

shmfx::Result<void> Logger::log_binary(LogLevel level,
                                       std::span<const std::byte> payload) noexcept {
    return publish(level, Binary, payload);
}

shmfx::Result<void> Logger::trace(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Trace, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::debug(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Debug, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::info(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Info, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::warn(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Warn, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::error(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Error, fmt, args);
    va_end(args);
    return result;
}

shmfx::Result<void> Logger::fatal(const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    auto result = vlog(LogLevel::Fatal, fmt, args);
    va_end(args);
    return result;
}

std::string_view Logger::segment_name() const noexcept {
    return segment_name_;
}

std::uint64_t Logger::lost_count() const noexcept {
    return ring_.lost_count();
}

Logger::Logger(shmfx::ShmHandle handle,
               shmfx::MpscRing ring,
               std::string segment_name) noexcept
    : handle_(std::move(handle)), ring_(std::move(ring)), segment_name_(std::move(segment_name)) {}

shmfx::Result<void> Logger::vlog(LogLevel level, const char* fmt, va_list args) noexcept {
    if (fmt == nullptr) {
        return shmfx::ErrorCode::InvalidName;
    }

    thread_local std::array<char, kFormatBufferBytes> buffer{};
    const int rc = std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    if (rc < 0) {
        return shmfx::ErrorCode::CorruptedHeader;
    }
    const std::size_t written = std::min<std::size_t>(static_cast<std::size_t>(rc), buffer.size() - 1u);
    return publish(level,
                   static_cast<std::uint16_t>(rc >= static_cast<int>(buffer.size()) ? Truncated : 0),
                   {reinterpret_cast<const std::byte*>(buffer.data()), written});
}

shmfx::Result<void> Logger::publish(LogLevel level,
                                    std::uint16_t flags,
                                    std::span<const std::byte> payload) noexcept {
    return ring_.try_push(static_cast<std::uint16_t>(level),
                          flags,
                          monotonic_record_time(),
                          payload);
}

} // namespace shmfx_logging
