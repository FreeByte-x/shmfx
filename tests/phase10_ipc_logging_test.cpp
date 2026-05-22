#include "shmfx/shm_manager.h"
#include "shmfx_logging/log_center.h"
#include "shmfx_logging/logger.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kRecords = 128;

bool child_exited_ok(pid_t pid) {
    int status = 0;
    assert(::waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main() {
    const std::string token = std::to_string(::getpid());
    const std::string segment_name = "/shmfx.log.phase10ipc_" + token;
    const std::string prefix = "/shmfx.log.phase10ipc_";

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(segment_name);

    const pid_t producer = ::fork();
    assert(producer >= 0);
    if (producer == 0) {
        shmfx_logging::LoggerOptions options;
        options.service_name = "phase10ipc";
        options.segment_name = segment_name;
        options.record_max = 256;
        options.max_payload = 128;

        auto logger = shmfx_logging::Logger::init(options);
        if (!logger) {
            return 2;
        }
        for (int i = 0; i < kRecords; ++i) {
            auto pushed = logger.value().info("ipc-record-%d", i);
            if (!pushed) {
                return 3;
            }
        }
        return 0;
    }

    shmfx_logging::LogCenterOptions center_options;
    center_options.prefix = prefix;
    center_options.drain_batch = 64;
    center_options.scratch_bytes = 256;

    std::vector<std::string> payloads;
    shmfx_logging::LogCenter center(center_options);
    center.set_sink([&](const shmfx_logging::LogRecord& record) {
        if (record.source == segment_name) {
            payloads.push_back(record.payload);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (payloads.size() < static_cast<std::size_t>(kRecords) &&
           std::chrono::steady_clock::now() < deadline) {
        [[maybe_unused]] const std::uint32_t drained = center.poll_once();
    }

    assert(child_exited_ok(producer));
    while (payloads.size() < static_cast<std::size_t>(kRecords) &&
           std::chrono::steady_clock::now() < deadline) {
        [[maybe_unused]] const std::uint32_t drained = center.poll_once();
    }

    assert(payloads.size() == static_cast<std::size_t>(kRecords));
    assert(payloads.front() == "ipc-record-0");
    assert(payloads.back() == "ipc-record-127");

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(segment_name);
    std::puts("phase10_ipc_logging_test: ok");
    return 0;
}
