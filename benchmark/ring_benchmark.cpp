#include "shmfx/shm_ring.h"
#include "shmfx/shm_sync.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <thread>
#include <vector>

namespace {

struct AlignedBuffer {
    void* ptr = nullptr;
    std::size_t size = 0;

    explicit AlignedBuffer(std::size_t requested_size) {
        size = shmfx::align_up(requested_size, shmfx::CACHE_LINE_SIZE);
        if (posix_memalign(&ptr, shmfx::CACHE_LINE_SIZE, size) != 0) {
            std::abort();
        }
    }

    ~AlignedBuffer() {
        std::free(ptr);
    }

    std::span<std::byte> bytes() {
        return {static_cast<std::byte*>(ptr), size};
    }
};

} // namespace

int main(int argc, char** argv) {
    const std::uint32_t producers = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 4;
    const std::uint32_t per_producer = argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 100000;
    constexpr std::uint32_t record_max = 65536;
    constexpr std::uint32_t max_payload = 64;

    const std::uint32_t stride = shmfx::MpscRing::record_stride_for(max_payload);
    shmfx::RecordRingMeta meta{};
    AlignedBuffer storage(shmfx::MpscRing::payload_size_for(record_max, stride));
    auto initialized = shmfx::MpscRing::initialize(meta, storage.bytes(), record_max, max_payload);
    if (!initialized) {
        std::cerr << "ring initialize failed\n";
        return 1;
    }
    auto ring = shmfx::MpscRing::bind(meta, storage.bytes());
    if (!ring) {
        std::cerr << "ring bind failed\n";
        return 1;
    }

    const std::uint64_t total = static_cast<std::uint64_t>(producers) * per_producer;
    std::array<std::byte, max_payload> payload{};
    payload.fill(std::byte{0x42});

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (std::uint32_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([producer, per_producer, &payload, &ring]() {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                while (!ring.value()
                            .try_push(static_cast<std::uint16_t>(producer), 0, i, payload)) {
                    shmfx::cpu_relax();
                }
            }
        });
    }

    std::array<std::byte, max_payload> scratch{};
    std::uint64_t consumed = 0;
    while (consumed < total) {
        shmfx::PoppedRecord record{};
        if (ring.value().try_pop(scratch, record)) {
            ++consumed;
        } else {
            shmfx::cpu_relax();
        }
    }

    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double seconds = static_cast<double>(ns) / 1'000'000'000.0;
    const double throughput = static_cast<double>(total) / seconds;
    const double ns_per_record = static_cast<double>(ns) / static_cast<double>(total);

    std::cout << "producers=" << producers << "\n";
    std::cout << "records=" << total << "\n";
    std::cout << "seconds=" << seconds << "\n";
    std::cout << "throughput_records_per_sec=" << throughput << "\n";
    std::cout << "avg_ns_per_record=" << ns_per_record << "\n";
    std::cout << "lost_count=" << ring.value().lost_count() << "\n";
    return 0;
}
