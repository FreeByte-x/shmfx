#include "shmfx_logging/log_center.h"
#include "shmfx_logging/logger.h"

#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

int main() {
    shmfx_logging::LoggerOptions options;
    options.service_name = "multidemo";
    options.segment_name = "/shmfx.log.multidemo_" + std::to_string(::getpid());
    options.record_max = 256;

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.segment_name);

    auto logger = shmfx_logging::Logger::init(options);
    if (!logger) {
        std::cerr << "logger init failed: " << shmfx::to_string(logger.error()) << "\n";
        return 1;
    }

    constexpr int thread_count = 4;
    constexpr int per_thread = 8;
    std::vector<std::thread> threads;
    for (int tid = 0; tid < thread_count; ++tid) {
        threads.emplace_back([tid, &logger]() {
            for (int i = 0; i < per_thread; ++i) {
                [[maybe_unused]] auto pushed = logger.value().info("thread=%d i=%d", tid, i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    int drained = 0;
    shmfx_logging::LogCenterOptions center_options;
    center_options.prefix = "/shmfx.log.multidemo_";
    shmfx_logging::LogCenter center(center_options);
    center.set_sink([&](const shmfx_logging::LogRecord& record) {
        ++drained;
        std::cout << record.payload << "\n";
    });
    while (drained < thread_count * per_thread) {
        [[maybe_unused]] const std::uint32_t batch = center.poll_once();
    }

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(options.segment_name);
    return 0;
}
