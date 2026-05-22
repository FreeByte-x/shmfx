#include "shmfx/shm_manager.h"
#include "shmfx/shm_ring.h"
#include "shmfx/shm_sync.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

int main() {
    const std::string name = "/shmfx.app.ring_demo_" + std::to_string(::getpid());
    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(name);

    constexpr std::uint32_t record_max = 128;
    constexpr std::uint32_t max_payload = 32;
    const std::uint32_t stride = shmfx::MpscRing::record_stride_for(max_payload);
    const std::uint64_t payload_size = shmfx::MpscRing::payload_size_for(record_max, stride);

    shmfx::CreateOptions options;
    options.name = name;
    options.type = shmfx::SegmentType::RecordRing;
    options.meta_size = sizeof(shmfx::RecordRingMeta);
    options.total_size = shmfx::align_up(shmfx::SHMFX_HEADER_SIZE + options.meta_size,
                                         shmfx::SHMFX_CACHE_LINE_BYTES) +
                         payload_size;
    options.flags = shmfx::LockfreeRing;

    auto segment = shmfx::ShmManager::create(options);
    if (!segment) {
        std::cerr << "create failed: " << shmfx::to_string(segment.error()) << "\n";
        return 1;
    }

    auto& meta = *reinterpret_cast<shmfx::RecordRingMeta*>(segment.value().metadata().data());
    auto ring = shmfx::MpscRing::initialize(meta, segment.value().payload(), record_max, max_payload);
    if (!ring) {
        std::cerr << "ring init failed: " << shmfx::to_string(ring.error()) << "\n";
        return 1;
    }

    constexpr std::uint32_t producers = 4;
    constexpr std::uint32_t per_producer = 16;
    std::vector<std::thread> threads;
    for (std::uint32_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([producer, &ring]() {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                const std::string text = "p" + std::to_string(producer) + ":" + std::to_string(i);
                const auto bytes = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(text.data()), text.size());
                while (!ring.value().try_push(static_cast<std::uint16_t>(producer), 0, i, bytes)) {
                    shmfx::cpu_relax();
                }
            }
        });
    }

    std::uint32_t consumed = 0;
    std::array<std::byte, 64> scratch{};
    while (consumed < producers * per_producer) {
        shmfx::PoppedRecord record{};
        if (ring.value().try_pop(scratch, record)) {
            std::cout << "type=" << record.type << " payload="
                      << std::string(reinterpret_cast<char*>(scratch.data()), record.copied_size)
                      << "\n";
            ++consumed;
        } else {
            shmfx::cpu_relax();
        }
    }

    for (auto& thread : threads) {
        thread.join();
    }
    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(name);
    return 0;
}
