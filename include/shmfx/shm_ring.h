#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_header.h"
#include "shmfx/shm_sync.h"
#include "shmfx/shm_types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace shmfx {

/// Producer mode value stored in RecordRingMeta::producer_mode.
inline constexpr std::uint32_t RING_PRODUCER_MPSC = 2;

/// Consumer mode value stored in RecordRingMeta::consumer_mode.
inline constexpr std::uint32_t RING_CONSUMER_SINGLE = 1;

/// Core-reserved record flag indicating payload truncation at push time.
inline constexpr std::uint16_t RECORD_FLAG_TRUNCATED = 1u << 0;

/// Fixed wire header stored at the start of every record slot.
struct RingRecordHeader {
    /// Payload byte length present in this slot, excluding this header.
    std::uint32_t len;
    /// Application-defined record type.
    std::uint16_t type;
    /// Application-defined flags plus core-reserved bits.
    std::uint16_t flags;
    /// Application-defined timestamp or user value.
    std::uint64_t user;
};

static_assert(sizeof(RingRecordHeader) == 16);
static_assert(alignof(RingRecordHeader) == 8);

/// Metadata returned after a successful pop.
struct PoppedRecord {
    /// Application-defined record type.
    std::uint16_t type;
    /// Application-defined flags plus core-reserved bits.
    std::uint16_t flags;
    /// Application-defined timestamp or user value.
    std::uint64_t user;
    /// Payload bytes stored in the ring slot.
    std::uint32_t payload_size;
    /// Payload bytes copied into the caller output buffer.
    std::uint32_t copied_size;
};

/// Non-owning view over an initialized shared-memory MPSC record ring.
class MpscRing {
public:
    /// Calculates the slot stride for a maximum payload size.
    ///
    /// @param max_payload Maximum payload bytes accepted per record before truncation.
    /// @return Cache-line-aligned slot stride including RingRecordHeader.
    [[nodiscard]] static std::uint32_t record_stride_for(std::uint32_t max_payload) noexcept;

    /// Calculates the payload bytes required for ring control and slots.
    ///
    /// @param record_max Number of slots in the bounded ring.
    /// @param record_stride Slot stride including RingRecordHeader.
    /// @return Required byte size for the ring payload area.
    [[nodiscard]] static std::uint64_t payload_size_for(std::uint32_t record_max,
                                                        std::uint32_t record_stride) noexcept;

    /// Initializes metadata and ring payload storage.
    ///
    /// @param meta RecordRingMeta located in the segment metadata area.
    /// @param payload Writable bytes located in the segment payload area.
    /// @param record_max Number of bounded slots to initialize.
    /// @param max_payload Maximum payload bytes accepted before truncation.
    /// @return Bound MpscRing view or an error code.
    [[nodiscard]] static Result<MpscRing> initialize(RecordRingMeta& meta,
                                                     std::span<std::byte> payload,
                                                     std::uint32_t record_max,
                                                     std::uint32_t max_payload) noexcept;

    /// Binds to an already initialized ring.
    ///
    /// @param meta RecordRingMeta located in the segment metadata area.
    /// @param payload Bytes located in the segment payload area.
    /// @return Bound MpscRing view or an error code.
    [[nodiscard]] static Result<MpscRing> bind(RecordRingMeta& meta,
                                               std::span<std::byte> payload) noexcept;

    /// Attempts to publish one record without blocking.
    ///
    /// @param type Application-defined record type.
    /// @param flags Application-defined flags; core may OR RECORD_FLAG_TRUNCATED.
    /// @param user Application-defined timestamp or user value.
    /// @param payload Record payload bytes.
    /// @return Success, ErrorCode::RingFull, or layout/corruption error.
    [[nodiscard]] Result<void> try_push(std::uint16_t type,
                                        std::uint16_t flags,
                                        std::uint64_t user,
                                        std::span<const std::byte> payload) noexcept;

    /// Attempts to pop one ready record for the single consumer.
    ///
    /// Empty ring is reported as false, not an error.
    ///
    /// @param out_payload Caller buffer that receives payload bytes.
    /// @param out_record Pop metadata written on success.
    /// @return true when one record was popped.
    [[nodiscard]] bool try_pop(std::span<std::byte> out_payload,
                               PoppedRecord& out_record) noexcept;

    /// Drains up to @p max_records records through a callback.
    ///
    /// The callback must be callable as bool(const PoppedRecord&, std::span<const std::byte>).
    /// Returning false stops the batch early after the current record is popped.
    ///
    /// @tparam Callback Consumer callback type.
    /// @param scratch Scratch buffer large enough for expected payloads.
    /// @param max_records Maximum records to pop in this batch.
    /// @param callback Callback invoked for each popped record.
    /// @return Number of records popped.
    template <class Callback>
    std::uint32_t drain_batch(std::span<std::byte> scratch,
                              std::uint32_t max_records,
                              Callback&& callback) noexcept {
        std::uint32_t drained = 0;
        for (; drained < max_records; ++drained) {
            PoppedRecord record{};
            if (!try_pop(scratch, record)) {
                break;
            }
            const auto view = std::span<const std::byte>(scratch.data(), record.copied_size);
            if (!callback(record, view)) {
                ++drained;
                break;
            }
        }
        return drained;
    }

    /// Returns the initialized ring metadata.
    ///
    /// @return Mutable metadata reference.
    [[nodiscard]] RecordRingMeta& meta() noexcept;

    /// Returns the initialized ring metadata.
    ///
    /// @return Immutable metadata reference.
    [[nodiscard]] const RecordRingMeta& meta() const noexcept;

    /// Returns the producer drop counter.
    ///
    /// @return Number of records dropped because the ring was full or contended.
    [[nodiscard]] std::uint64_t lost_count() const noexcept;

private:
    /// Creates a bound ring view after validation.
    ///
    /// @param meta Initialized metadata.
    /// @param payload Ring payload bytes.
    MpscRing(RecordRingMeta& meta, std::span<std::byte> payload) noexcept;

    /// Returns the slot metadata for a logical slot index.
    ///
    /// @param index Slot index in [0, record_max).
    /// @return Slot header pointer.
    [[nodiscard]] RingSlotHeader* slot_header(std::uint64_t index) noexcept;

    /// Returns the record storage for a logical slot index.
    ///
    /// @param index Slot index in [0, record_max).
    /// @return Pointer to the slot's record header bytes.
    [[nodiscard]] std::byte* record_bytes(std::uint64_t index) noexcept;

    RecordRingMeta* meta_ = nullptr;
    std::span<std::byte> payload_{};
};

} // namespace shmfx
