#include "shmfx_logging/log_center.h"
#include "shmfx_logging/logger.h"

#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    shmfx_logging::LoggerOptions options;
    options.service_name = "demo";
    options.segment_name = "/shmfx.log.demo_" + std::to_string(::getpid());

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.segment_name);

    auto logger = shmfx_logging::Logger::init(options);
    if (!logger) {
        std::cerr << "logger init failed: " << shmfx::to_string(logger.error()) << "\n";
        return 1;
    }

    [[maybe_unused]] auto first = logger.value().info(
        "demo producer pid=%d", static_cast<int>(::getpid()));
    [[maybe_unused]] auto second = logger.value().warn(
        "this record is drained by an in-process LogCenter demo");

    shmfx_logging::LogCenterOptions center_options;
    center_options.prefix = "/shmfx.log.demo_";
    shmfx_logging::LogCenter center(center_options);
    center.set_sink([](const shmfx_logging::LogRecord& record) {
        std::cout << record.source << " level=" << static_cast<int>(record.level)
                  << " payload=" << record.payload << "\n";
    });
    [[maybe_unused]] const std::uint32_t drained = center.poll_once();

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(options.segment_name);
    return 0;
}
