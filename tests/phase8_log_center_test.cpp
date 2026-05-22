#include "shmfx/shm_manager.h"
#include "shmfx_logging/log_center.h"
#include "shmfx_logging/logger.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <unistd.h>

int main() {
    shmfx_logging::LoggerOptions options;
    options.service_name = "phase8";
    options.segment_name = "/shmfx.log.phase8_" + std::to_string(::getpid());
    options.record_max = 16;
    options.max_payload = 128;

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.segment_name);

    auto logger = shmfx_logging::Logger::init(options);
    assert(logger);
    assert(logger.value().info("first=%d", 1));
    assert(logger.value().warn("second"));

    shmfx_logging::LogCenterOptions center_options;
    center_options.prefix = "/shmfx.log.phase8_";
    center_options.drain_batch = 8;
    center_options.scratch_bytes = 256;

    std::vector<shmfx_logging::LogRecord> records;
    shmfx_logging::LogCenter center(center_options);
    center.set_sink([&](const shmfx_logging::LogRecord& record) {
        records.push_back(record);
    });

    auto discovered = center.discover_once();
    assert(discovered);
    assert(center.attached_count() == 1);
    assert(center.drain_once() == 2);
    assert(records.size() == 2);
    assert(records[0].source == options.segment_name);
    assert(records[0].level == shmfx_logging::LogLevel::Info);
    assert(records[0].payload == "first=1");
    assert(records[1].level == shmfx_logging::LogLevel::Warn);
    assert(records[1].payload == "second");

    assert(logger.value().error("third"));
    assert(center.poll_once() == 1);
    assert(records.size() == 3);
    assert(records[2].level == shmfx_logging::LogLevel::Error);

    auto observer = shmfx::ShmManager::attach(options.segment_name, shmfx::AttachMode::ReadWrite);
    assert(observer);
    std::atomic_ref<std::uint32_t>(observer.value().header().state)
        .store(static_cast<std::uint32_t>(shmfx::SegmentState::Dead), std::memory_order_release);
    assert(center.drain_once() == 0);
    assert(center.attached_count() == 0);

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(options.segment_name);

    std::puts("phase8_log_center_test: ok");
    return 0;
}
