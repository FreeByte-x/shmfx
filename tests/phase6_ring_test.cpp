#include "shmfx/shm_ring.h"

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

    explicit AlignedBuffer(std::size_t requested_size) : size(requested_size) {
        const std::size_t rounded = shmfx::align_up(requested_size, shmfx::CACHE_LINE_SIZE);
        assert(posix_memalign(&ptr, shmfx::CACHE_LINE_SIZE, rounded) == 0);
        size = rounded;
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
    shmfx::RecordRingMeta meta{};
    const std::uint32_t record_max = 8;
    const std::uint32_t max_payload = 32;
    const std::uint32_t stride = shmfx::MpscRing::record_stride_for(max_payload);
    AlignedBuffer storage(shmfx::MpscRing::payload_size_for(record_max, stride));

    auto initialized = shmfx::MpscRing::initialize(meta, storage.bytes(), record_max, max_payload);
    assert(initialized);
    assert(meta.record_max == record_max);
    assert(meta.record_stride == stride);
    assert(meta.producer_mode == shmfx::RING_PRODUCER_MPSC);

    auto bound = shmfx::MpscRing::bind(meta, storage.bytes());
    assert(bound);

    std::array<std::byte, 4> payload{
        std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44}};
    auto pushed = bound.value().try_push(7, 0, 1234, payload);
    assert(pushed);

    std::array<std::byte, 16> out{};
    shmfx::PoppedRecord popped{};
    assert(bound.value().try_pop(out, popped));
    assert(popped.type == 7);
    assert(popped.flags == 0);
    assert(popped.user == 1234);
    assert(popped.payload_size == payload.size());
    assert(popped.copied_size == payload.size());
    assert(std::memcmp(out.data(), payload.data(), payload.size()) == 0);
    assert(!bound.value().try_pop(out, popped));

    std::array<std::byte, 80> large{};
    large.fill(std::byte{0x5a});
    const std::uint32_t slot_payload_capacity = stride - sizeof(shmfx::RingRecordHeader);
    assert(bound.value().try_push(9, 0, 5678, large));
    assert(bound.value().try_pop(out, popped));
    assert((popped.flags & shmfx::RECORD_FLAG_TRUNCATED) != 0);
    assert(popped.payload_size == slot_payload_capacity);
    assert(popped.copied_size == out.size());

    for (std::uint32_t i = 0; i < record_max; ++i) {
        auto result = bound.value().try_push(static_cast<std::uint16_t>(i), 0, i, payload);
        assert(result);
    }
    auto full = bound.value().try_push(99, 0, 99, payload);
    assert(!full);
    assert(full.error() == shmfx::ErrorCode::RingFull);
    assert(bound.value().lost_count() == 1);

    std::uint32_t drained = 0;
    drained = bound.value().drain_batch(out, record_max, [&](const shmfx::PoppedRecord&, auto) {
        return true;
    });
    assert(drained == record_max);

    constexpr std::uint32_t producers = 4;
    constexpr std::uint32_t per_producer = 1000;
    const std::uint32_t concurrent_slots = 128;
    const std::uint32_t concurrent_stride = shmfx::MpscRing::record_stride_for(8);
    shmfx::RecordRingMeta concurrent_meta{};
    AlignedBuffer concurrent_storage(
        shmfx::MpscRing::payload_size_for(concurrent_slots, concurrent_stride));
    auto concurrent_init = shmfx::MpscRing::initialize(
        concurrent_meta, concurrent_storage.bytes(), concurrent_slots, 8);
    assert(concurrent_init);
    auto concurrent_ring = shmfx::MpscRing::bind(concurrent_meta, concurrent_storage.bytes());
    assert(concurrent_ring);

    std::vector<std::thread> threads;
    for (std::uint32_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([producer, &concurrent_ring]() {
            std::array<std::byte, 4> bytes{};
            std::memcpy(bytes.data(), &producer, sizeof(producer));
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                while (!concurrent_ring.value()
                            .try_push(static_cast<std::uint16_t>(producer), 0, i, bytes)) {
                    shmfx::cpu_relax();
                }
            }
        });
    }

    std::uint32_t consumed = 0;
    while (consumed < producers * per_producer) {
        if (concurrent_ring.value().try_pop(out, popped)) {
            ++consumed;
        } else {
            shmfx::cpu_relax();
        }
    }
    for (auto& thread : threads) {
        thread.join();
    }
    assert(consumed == producers * per_producer);

    std::puts("phase6_ring_test: ok");
    return 0;
}
