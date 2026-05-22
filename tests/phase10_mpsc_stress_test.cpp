#include "shmfx/shm_ring.h"
#include "shmfx/shm_sync.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace {

struct AlignedBuffer {
    void* ptr = nullptr;
    std::size_t size = 0;

    explicit AlignedBuffer(std::size_t requested_size) {
        size = shmfx::align_up(requested_size, shmfx::CACHE_LINE_SIZE);
        assert(posix_memalign(&ptr, shmfx::CACHE_LINE_SIZE, size) == 0);
    }

    ~AlignedBuffer() {
        std::free(ptr);
    }

    std::span<std::byte> bytes() {
        return {static_cast<std::byte*>(ptr), size};
    }
};

} // namespace

int main() {
    constexpr std::uint32_t producers = 8;
    constexpr std::uint32_t per_producer = 5000;
    constexpr std::uint32_t record_max = 2048;
    constexpr std::uint32_t max_payload = 16;

    const std::uint32_t stride = shmfx::MpscRing::record_stride_for(max_payload);
    shmfx::RecordRingMeta meta{};
    AlignedBuffer storage(shmfx::MpscRing::payload_size_for(record_max, stride));
    auto initialized = shmfx::MpscRing::initialize(meta, storage.bytes(), record_max, max_payload);
    assert(initialized);

    auto ring = shmfx::MpscRing::bind(meta, storage.bytes());
    assert(ring);

    std::vector<std::thread> threads;
    for (std::uint32_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([producer, &ring]() {
            std::array<std::byte, sizeof(std::uint64_t)> payload{};
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                const std::uint64_t value =
                    (static_cast<std::uint64_t>(producer) << 32u) | i;
                std::memcpy(payload.data(), &value, sizeof(value));
                while (!ring.value()
                            .try_push(static_cast<std::uint16_t>(producer), 0, value, payload)) {
                    shmfx::cpu_relax();
                }
            }
        });
    }

    std::array<std::byte, 32> scratch{};
    std::vector<std::uint32_t> seen(producers, 0);
    std::uint32_t consumed = 0;
    while (consumed < producers * per_producer) {
        shmfx::PoppedRecord record{};
        if (!ring.value().try_pop(scratch, record)) {
            shmfx::cpu_relax();
            continue;
        }

        std::uint64_t value = 0;
        assert(record.copied_size == sizeof(value));
        std::memcpy(&value, scratch.data(), sizeof(value));
        const std::uint32_t producer = static_cast<std::uint32_t>(value >> 32u);
        const std::uint32_t sequence = static_cast<std::uint32_t>(value);
        assert(producer < producers);
        assert(sequence == seen[producer]);
        ++seen[producer];
        ++consumed;
    }

    for (auto& thread : threads) {
        thread.join();
    }
    for (std::uint32_t count : seen) {
        assert(count == per_producer);
    }

    std::puts("phase10_mpsc_stress_test: ok");
    return 0;
}
