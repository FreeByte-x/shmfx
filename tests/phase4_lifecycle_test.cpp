#include "shmfx/shm_lifecycle.h"
#include "shmfx/shm_manager.h"
#include "shmfx/shm_registry.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

int main() {
    const std::string heartbeat_name = "/shmfx.app.phase4_hb_" + std::to_string(::getpid());
    const std::string janitor_name = "/shmfx.app.phase4_gc_" + std::to_string(::getpid());

    [[maybe_unused]] auto cleanup_hb = shmfx::ShmManager::destroy(heartbeat_name);
    [[maybe_unused]] auto cleanup_gc = shmfx::ShmManager::destroy(janitor_name);

    shmfx::CreateOptions options;
    options.type = shmfx::SegmentType::Raw;
    options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 4096;
    options.meta_size = shmfx::SHMFX_CACHE_LINE_BYTES;

    options.name = heartbeat_name;
    auto heartbeat_segment = shmfx::ShmManager::create(options);
    assert(heartbeat_segment);
    assert(heartbeat_segment.value().header().owner_start_time != 0);
    const std::uint64_t before = heartbeat_segment.value().header().heartbeat_counter;
    std::this_thread::sleep_for(std::chrono::milliseconds(shmfx::HEARTBEAT_TICK_MS * 2 + 50));
    const std::uint64_t after = heartbeat_segment.value().header().heartbeat_counter;
    assert(after > before);
    assert(!shmfx::owner_is_dead(heartbeat_segment.value().header(), shmfx::monotonic_ns()));

    options.name = janitor_name;
    auto janitor_segment = shmfx::ShmManager::create(options);
    assert(janitor_segment);
    auto& header = janitor_segment.value().header();
    std::atomic_ref<std::uint32_t>(header.owner_pid).store(0, std::memory_order_release);
    shmfx::Registry::instance().gc();
    const auto state_after_gc = static_cast<shmfx::SegmentState>(
        std::atomic_ref<std::uint32_t>(header.state).load(std::memory_order_acquire));
    assert(state_after_gc == shmfx::SegmentState::Draining);

    std::atomic_ref<std::uint32_t>(header.state).store(
        static_cast<std::uint32_t>(shmfx::SegmentState::Dead), std::memory_order_release);
    auto dead_attach = shmfx::ShmManager::attach(janitor_name, shmfx::AttachMode::ReadOnly);
    assert(!dead_attach);
    assert(dead_attach.error() == shmfx::ErrorCode::SegmentDead);

    [[maybe_unused]] auto destroy_gc = shmfx::ShmManager::destroy(janitor_name);
    [[maybe_unused]] auto destroy_hb = shmfx::ShmManager::destroy(heartbeat_name);

    std::puts("phase4_lifecycle_test: ok");
    return 0;
}
