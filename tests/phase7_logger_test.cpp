#include "shmfx/shm_manager.h"
#include "shmfx/shm_ring.h"
#include "shmfx_logging/logger.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

int main() {
    shmfx_logging::LoggerOptions options;
    options.service_name = "phase7";
    options.segment_name = "/shmfx.log.phase7_" + std::to_string(::getpid());
    options.record_max = 8;
    options.max_payload = 64;

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.segment_name);

    auto logger = shmfx_logging::Logger::init(options);
    assert(logger);
    assert(logger.value().segment_name() == options.segment_name);

    auto info = logger.value().info("user=%s id=%d", "alice", 42);
    assert(info);
    const std::string direct = "direct text";
    assert(logger.value().log_text(shmfx_logging::LogLevel::Warn, direct));

    auto consumer = shmfx::ShmManager::attach(options.segment_name, shmfx::AttachMode::ReadWrite);
    assert(consumer);
    auto& meta = *reinterpret_cast<shmfx::RecordRingMeta*>(consumer.value().metadata().data());
    auto ring = shmfx::MpscRing::bind(meta, consumer.value().payload());
    assert(ring);

    std::byte payload[128]{};
    shmfx::PoppedRecord record{};
    assert(ring.value().try_pop(payload, record));
    assert(record.type == static_cast<std::uint16_t>(shmfx_logging::LogLevel::Info));
    assert(record.user != 0);
    assert(std::string(reinterpret_cast<char*>(payload), record.copied_size) == "user=alice id=42");

    assert(ring.value().try_pop(payload, record));
    assert(record.type == static_cast<std::uint16_t>(shmfx_logging::LogLevel::Warn));
    assert(std::string(reinterpret_cast<char*>(payload), record.copied_size) == direct);

    const std::string large(256, 'x');
    assert(logger.value().log_text(shmfx_logging::LogLevel::Error, large));
    assert(ring.value().try_pop(payload, record));
    assert(record.type == static_cast<std::uint16_t>(shmfx_logging::LogLevel::Error));
    assert((record.flags & shmfx_logging::Truncated) != 0);

    auto bad = shmfx_logging::Logger::init("BadName");
    assert(!bad);
    assert(bad.error() == shmfx::ErrorCode::InvalidName);

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(options.segment_name);

    std::puts("phase7_logger_test: ok");
    return 0;
}
