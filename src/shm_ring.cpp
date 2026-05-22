#include "shmfx/shm_ring.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace shmfx {
namespace {

constexpr std::uint64_t kRecordAreaAlign = alignof(RingRecordHeader);

[[nodiscard]] bool payload_aligned(std::span<std::byte> payload) noexcept {
    return reinterpret_cast<std::uintptr_t>(payload.data()) % alignof(RingCursor) == 0;
}

[[nodiscard]] std::uint64_t slot_meta_offset() noexcept {
    return sizeof(RingCursor) * 2u;
}

[[nodiscard]] std::uint64_t record_area_offset(std::uint32_t record_max) noexcept {
    return align_up(slot_meta_offset() + sizeof(RingSlotHeader) * record_max, kRecordAreaAlign);
}

[[nodiscard]] RingCursor* head_cursor(std::span<std::byte> payload) noexcept {
    return reinterpret_cast<RingCursor*>(payload.data());
}

[[nodiscard]] RingCursor* tail_cursor(std::span<std::byte> payload) noexcept {
    return reinterpret_cast<RingCursor*>(payload.data() + sizeof(RingCursor));
}

[[nodiscard]] bool valid_meta(const RecordRingMeta& meta) noexcept {
    return meta.record_max != 0 && meta.record_stride >= sizeof(RingRecordHeader) &&
           meta.producer_mode == RING_PRODUCER_MPSC &&
           meta.consumer_mode == RING_CONSUMER_SINGLE;
}

} // namespace

std::uint32_t MpscRing::record_stride_for(std::uint32_t max_payload) noexcept {
    return static_cast<std::uint32_t>(
        align_up(sizeof(RingRecordHeader) + static_cast<std::uint64_t>(max_payload),
                 CACHE_LINE_SIZE));
}

std::uint64_t MpscRing::payload_size_for(std::uint32_t record_max,
                                         std::uint32_t record_stride) noexcept {
    if (record_max == 0 || record_stride < sizeof(RingRecordHeader)) {
        return 0;
    }
    return record_area_offset(record_max) +
           static_cast<std::uint64_t>(record_max) * record_stride;
}

Result<MpscRing> MpscRing::initialize(RecordRingMeta& meta,
                                      std::span<std::byte> payload,
                                      std::uint32_t record_max,
                                      std::uint32_t max_payload) noexcept {
    const std::uint32_t stride = record_stride_for(max_payload);
    const std::uint64_t required = payload_size_for(record_max, stride);
    if (required == 0 || payload.size() < required || !payload_aligned(payload)) {
        return ErrorCode::CorruptedHeader;
    }

    std::memset(&meta, 0, sizeof(meta));
    meta.record_max = record_max;
    meta.record_stride = stride;
    meta.producer_mode = RING_PRODUCER_MPSC;
    meta.consumer_mode = RING_CONSUMER_SINGLE;
    atomic_store_u64(meta.lost_count, 0, std::memory_order_relaxed);

    std::memset(payload.data(), 0, required);
    atomic_store_u64(head_cursor(payload)->pos_storage, 0, std::memory_order_relaxed);
    atomic_store_u64(tail_cursor(payload)->pos_storage, 0, std::memory_order_relaxed);

    auto* slots = reinterpret_cast<RingSlotHeader*>(payload.data() + slot_meta_offset());
    for (std::uint32_t i = 0; i < record_max; ++i) {
        atomic_store_u64(slots[i].seq_storage, i, std::memory_order_relaxed);
    }

    return MpscRing(meta, payload.first(static_cast<std::size_t>(required)));
}

Result<MpscRing> MpscRing::bind(RecordRingMeta& meta,
                                std::span<std::byte> payload) noexcept {
    if (!valid_meta(meta) || !payload_aligned(payload)) {
        return ErrorCode::CorruptedHeader;
    }
    const std::uint64_t required = payload_size_for(meta.record_max, meta.record_stride);
    if (required == 0 || payload.size() < required) {
        return ErrorCode::CorruptedHeader;
    }
    return MpscRing(meta, payload.first(static_cast<std::size_t>(required)));
}

Result<void> MpscRing::try_push(std::uint16_t type,
                                std::uint16_t flags,
                                std::uint64_t user,
                                std::span<const std::byte> payload) noexcept {
    if (meta_ == nullptr || !valid_meta(*meta_)) {
        return ErrorCode::CorruptedHeader;
    }

    auto* tail = tail_cursor(payload_);
    std::uint64_t pos = atomic_load_u64(tail->pos_storage, std::memory_order_relaxed);
    RingSlotHeader* slot = nullptr;
    bool claimed = false;

    for (std::uint32_t retry = 0; retry < MPSC_CAS_RETRY_BUDGET; ++retry) {
        slot = slot_header(pos % meta_->record_max);
        const std::uint64_t seq = atomic_load_u64(slot->seq_storage, std::memory_order_acquire);
        const auto diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
        if (diff == 0) {
            std::uint64_t expected = pos;
            if (atomic_compare_exchange_weak_u64(tail->pos_storage,
                                                 expected,
                                                 pos + 1u,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
                claimed = true;
                break;
            }
            pos = expected;
            continue;
        }
        if (diff < 0) {
            atomic_fetch_add_u64(meta_->lost_count, 1, std::memory_order_relaxed);
            return ErrorCode::RingFull;
        }
        pos = atomic_load_u64(tail->pos_storage, std::memory_order_relaxed);
    }

    if (!claimed || slot == nullptr) {
        atomic_fetch_add_u64(meta_->lost_count, 1, std::memory_order_relaxed);
        return ErrorCode::RingFull;
    }

    auto* record = reinterpret_cast<RingRecordHeader*>(record_bytes(pos % meta_->record_max));
    const std::uint32_t max_payload = meta_->record_stride - sizeof(RingRecordHeader);
    const std::uint32_t written = static_cast<std::uint32_t>(
        std::min<std::size_t>(payload.size(), max_payload));
    std::uint16_t stored_flags = flags;
    if (payload.size() > max_payload) {
        stored_flags |= RECORD_FLAG_TRUNCATED;
    }

    record->len = written;
    record->type = type;
    record->flags = stored_flags;
    record->user = user;
    std::memcpy(record + 1, payload.data(), written);

    atomic_store_u64(slot->seq_storage, pos + 1u, std::memory_order_release);
    return {};
}

bool MpscRing::try_pop(std::span<std::byte> out_payload,
                       PoppedRecord& out_record) noexcept {
    if (meta_ == nullptr || !valid_meta(*meta_)) {
        return false;
    }

    auto* head = head_cursor(payload_);
    const std::uint64_t pos = atomic_load_u64(head->pos_storage, std::memory_order_relaxed);
    auto* slot = slot_header(pos % meta_->record_max);
    const std::uint64_t seq = atomic_load_u64(slot->seq_storage, std::memory_order_acquire);
    if (seq != pos + 1u) {
        return false;
    }

    const auto* record = reinterpret_cast<const RingRecordHeader*>(
        record_bytes(pos % meta_->record_max));
    const std::uint32_t max_payload = meta_->record_stride - sizeof(RingRecordHeader);
    const std::uint32_t payload_size = std::min(record->len, max_payload);
    const std::uint32_t copied = static_cast<std::uint32_t>(
        std::min<std::size_t>(out_payload.size(), payload_size));
    std::memcpy(out_payload.data(), record + 1, copied);

    out_record.type = record->type;
    out_record.flags = record->flags;
    out_record.user = record->user;
    out_record.payload_size = payload_size;
    out_record.copied_size = copied;

    atomic_store_u64(slot->seq_storage, pos + meta_->record_max, std::memory_order_release);
    atomic_store_u64(head->pos_storage, pos + 1u, std::memory_order_release);
    return true;
}

RecordRingMeta& MpscRing::meta() noexcept {
    return *meta_;
}

const RecordRingMeta& MpscRing::meta() const noexcept {
    return *meta_;
}

std::uint64_t MpscRing::lost_count() const noexcept {
    return meta_ == nullptr ? 0 : atomic_load_u64(meta_->lost_count, std::memory_order_relaxed);
}

MpscRing::MpscRing(RecordRingMeta& meta, std::span<std::byte> payload) noexcept
    : meta_(&meta), payload_(payload) {}

RingSlotHeader* MpscRing::slot_header(std::uint64_t index) noexcept {
    return reinterpret_cast<RingSlotHeader*>(payload_.data() + slot_meta_offset()) + index;
}

std::byte* MpscRing::record_bytes(std::uint64_t index) noexcept {
    return payload_.data() + record_area_offset(meta_->record_max) + index * meta_->record_stride;
}

} // namespace shmfx
