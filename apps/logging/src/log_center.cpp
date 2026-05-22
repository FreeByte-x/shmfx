#include "shmfx_logging/log_center.h"

#include "shmfx/shm_registry.h"
#include "shmfx/shm_sync.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace shmfx_logging {
namespace {

/// Returns a stable name view over a fixed shmfx ABI name buffer.
std::string_view entry_name(const shmfx::RegistryEntry& entry) noexcept {
    return {entry.name, strnlen(entry.name, shmfx::SHMFX_NAME_BYTES)};
}

/// Returns the metadata block as RecordRingMeta.
shmfx::RecordRingMeta& ring_meta(shmfx::ShmHandle& handle) noexcept {
    return *reinterpret_cast<shmfx::RecordRingMeta*>(handle.metadata().data());
}

/// Converts a core ring record type into a logging severity.
LogLevel decode_level(std::uint16_t type) noexcept {
    if (type <= static_cast<std::uint16_t>(LogLevel::Fatal)) {
        return static_cast<LogLevel>(type);
    }
    return LogLevel::Info;
}

/// Returns true when the segment header reached terminal state.
bool segment_dead(const shmfx::ShmHeader& header) noexcept {
    const auto state = static_cast<shmfx::SegmentState>(
        std::atomic_ref<const std::uint32_t>(header.state).load(std::memory_order_acquire));
    return state == shmfx::SegmentState::Dead;
}

} // namespace

LogCenter::AttachedRing::AttachedRing(std::string ring_name,
                                      shmfx::ShmHandle ring_handle,
                                      shmfx::MpscRing bound_ring) noexcept
    : name(std::move(ring_name)),
      handle(std::move(ring_handle)),
      ring(std::move(bound_ring)) {}

LogCenter::LogCenter() : LogCenter(LogCenterOptions{}) {}

LogCenter::LogCenter(LogCenterOptions options)
    : options_(std::move(options)), sink_([](const LogRecord&) {}) {}

LogCenter::LogCenter(LogCenter&& other) noexcept = default;

LogCenter& LogCenter::operator=(LogCenter&& other) noexcept = default;

LogCenter::~LogCenter() = default;

void LogCenter::set_sink(Sink sink) {
    sink_ = std::move(sink);
    if (!sink_) {
        sink_ = [](const LogRecord&) {};
    }
}

shmfx::Result<void> LogCenter::discover_once() {
    shmfx::Registry::instance().gc();
    for (const auto& entry : shmfx::Registry::instance().list(options_.prefix)) {
        if (static_cast<shmfx::SegmentType>(entry.segment_type) !=
            shmfx::SegmentType::RecordRing) {
            continue;
        }
        if (auto attached = attach_ring(entry_name(entry)); !attached) {
            return attached.error();
        }
    }
    return {};
}

std::uint32_t LogCenter::drain_once() {
    std::uint32_t drained = 0;
    for (auto& ring : rings_) {
        drained += drain_ring(ring);
    }

    rings_.erase(std::remove_if(rings_.begin(),
                                rings_.end(),
                                [](const AttachedRing& ring) {
                                    return ring.remove_after_drain;
                                }),
                 rings_.end());
    return drained;
}

std::uint32_t LogCenter::poll_once() {
    [[maybe_unused]] auto discovered = discover_once();
    const std::uint32_t drained = drain_once();
    if (drained == 0) {
        backoff_.idle();
    } else {
        backoff_.reset();
    }
    return drained;
}

std::size_t LogCenter::attached_count() const noexcept {
    return rings_.size();
}

shmfx::Result<void> LogCenter::attach_ring(std::string_view name) {
    const bool exists = std::any_of(rings_.begin(), rings_.end(), [&](const AttachedRing& ring) {
        return ring.name == name;
    });
    if (exists) {
        return {};
    }

    auto attached = shmfx::ShmManager::attach(name, shmfx::AttachMode::ReadWrite);
    if (!attached) {
        return attached.error();
    }
    if (static_cast<shmfx::SegmentType>(attached.value().header().segment_type) !=
            shmfx::SegmentType::RecordRing ||
        attached.value().metadata().size() < sizeof(shmfx::RecordRingMeta)) {
        return shmfx::ErrorCode::CorruptedHeader;
    }

    auto ring = shmfx::MpscRing::bind(ring_meta(attached.value()), attached.value().payload());
    if (!ring) {
        return ring.error();
    }
    rings_.emplace_back(std::string(name), std::move(attached.value()), std::move(ring.value()));
    return {};
}

std::uint32_t LogCenter::drain_ring(AttachedRing& ring) {
    if (segment_dead(ring.handle.header())) {
        ring.remove_after_drain = true;
    }

    std::vector<std::byte> scratch(options_.scratch_bytes);
    std::uint32_t drained = 0;
    for (; drained < options_.drain_batch; ++drained) {
        shmfx::PoppedRecord popped{};
        if (!ring.ring.try_pop(scratch, popped)) {
            break;
        }

        LogRecord record;
        record.source = ring.name;
        record.level = decode_level(popped.type);
        record.flags = popped.flags;
        record.timestamp_ns = popped.user;
        record.payload.assign(reinterpret_cast<const char*>(scratch.data()), popped.copied_size);
        sink_(record);
    }

    return drained;
}

} // namespace shmfx_logging
