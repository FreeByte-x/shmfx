#pragma once

#include "shmfx/shm_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <type_traits>

namespace shmfx {

/// Fixed 256-byte segment header shared by all shmfx segment types.
///
/// The storage is intentionally POD. Mutable fields are accessed with
/// std::atomic_ref<T> by control-plane code instead of std::atomic<T> members
/// so the header remains trivially copyable for snapshots and validation.
struct ShmHeader {
    /// Constant SHMFX_MAGIC at offset 0.
    std::uint32_t magic;
    /// Breaking ABI/layout version.
    std::uint16_t version_major;
    /// Additive ABI/API version.
    std::uint16_t version_minor;
    /// SegmentType stored as uint32_t for ABI stability.
    std::uint32_t segment_type;
    /// Immutable SegmentFlags bitset.
    std::uint32_t flags;
    /// Total mapped segment size in bytes.
    std::uint64_t total_size;
    /// Payload region size in bytes.
    std::uint64_t payload_size;
    /// Offset of user metadata from segment base.
    std::uint32_t meta_offset;
    /// User metadata region size in bytes.
    std::uint32_t meta_size;
    /// Offset of payload from segment base.
    std::uint32_t payload_offset;
    /// PID of the process that created the segment.
    std::uint32_t creator_pid;
    /// CLOCK_REALTIME timestamp for segment creation.
    std::uint64_t created_at_ns;
    /// Creator /proc/<pid>/stat start time, field 22.
    std::uint64_t creator_start_time;
    /// Null-terminated POSIX shm object name.
    char name[SHMFX_NAME_BYTES];

    /// Optional HMAC-SHA256 over immutable bytes [0, 128).
    std::uint8_t hmac[SHMFX_HMAC_BYTES];

    /// Current owner PID; accessed through std::atomic_ref<uint32_t>.
    std::uint32_t owner_pid;
    /// Explicit padding to align owner_start_time.
    std::uint32_t _pad0;
    /// Current owner start time paired with owner_pid.
    std::uint64_t owner_start_time;
    /// RW attach count; accessed through std::atomic_ref<uint32_t>.
    std::uint32_t ref_count;
    /// SegmentState stored as uint32_t; accessed through std::atomic_ref.
    std::uint32_t state;
    /// Owner heartbeat counter; accessed through std::atomic_ref<uint64_t>.
    std::uint64_t heartbeat_counter;
    /// CLOCK_MONOTONIC timestamp of last owner heartbeat.
    std::uint64_t heartbeat_last_ns;

    /// Robust process-shared mutex for serialized control-plane transitions.
    pthread_mutex_t control_mutex;
    /// Reserved bytes to keep the header exactly 256 bytes.
    std::uint8_t _reserved[16];
};

/// Fixed 64-byte metadata block for SegmentType::RecordRing payloads.
struct RecordRingMeta {
    /// Maximum number of records in the ring.
    std::uint32_t record_max;
    /// Byte stride of each record slot, aligned by ring setup.
    std::uint32_t record_stride;
    /// Producer mode enum storage; v0.1 default is MPSC.
    std::uint32_t producer_mode;
    /// Consumer mode enum storage; v0.1 supports single consumer.
    std::uint32_t consumer_mode;
    /// Dropped-record count; accessed through std::atomic_ref<uint64_t>.
    std::uint64_t lost_count;
    /// Reserved bytes to keep metadata exactly one cache line.
    std::uint8_t _reserved[40];
};

/// Fixed 128-byte registry slot updated with a per-slot seqlock.
struct RegistryEntry {
    /// Seqlock counter; odd means writer in progress.
    std::uint32_t seq_storage;
    /// Explicit padding before the name field.
    std::uint32_t _pad0;
    /// Null-terminated POSIX shm object name.
    char name[SHMFX_NAME_BYTES];
    /// Total segment size in bytes.
    std::uint64_t total_size;
    /// SegmentType stored as uint32_t.
    std::uint32_t segment_type;
    /// Last known owner PID.
    std::uint32_t owner_pid;
    /// Last known owner start time.
    std::uint64_t owner_start_time;
    /// Immutable SegmentFlags bitset copied from the segment header.
    std::uint32_t flags;
    /// Explicit padding before timestamps.
    std::uint32_t _pad1;
    /// CLOCK_REALTIME timestamp for registry entry creation.
    std::uint64_t created_at_ns;
    /// Janitor-updated last-seen timestamp.
    std::uint64_t last_seen_ns;
    /// Reserved bytes to keep each slot exactly 128 bytes.
    std::uint8_t _reserved[8];
};

/// Registry payload containing an occupancy bitmap and fixed slot array.
struct RegistryPayload {
    /// Occupied-slot count hint; accessed through std::atomic_ref<uint32_t>.
    std::uint32_t count_storage;
    /// Explicit padding before the bitmap.
    std::uint32_t _pad0;
    /// Occupancy bitmap, one bit per RegistryEntry slot.
    std::uint8_t bitmap[REGISTRY_BITMAP_BYTES];
    /// Padding to place slots at offset 256.
    std::uint8_t _align[120];
    /// Fixed registry slots protected by header control mutex for writes.
    RegistryEntry slots[MAX_REGISTRY_ENTRIES];
};

static_assert(sizeof(ShmHeader) == SHMFX_HEADER_SIZE);
static_assert(alignof(ShmHeader) >= 8);
static_assert(offsetof(ShmHeader, hmac) == SHMFX_IMMUTABLE_BYTES);
static_assert(offsetof(ShmHeader, owner_pid) == 160);
static_assert(offsetof(ShmHeader, owner_start_time) == 168);
static_assert(offsetof(ShmHeader, control_mutex) == 200);
static_assert(sizeof(pthread_mutex_t) <= 40,
              "Bump version_major if glibc changes pthread_mutex_t size");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<ShmHeader>);
static_assert(std::is_trivially_copyable_v<ShmHeader>);

static_assert(sizeof(RecordRingMeta) == 64);
static_assert(std::is_standard_layout_v<RecordRingMeta>);
static_assert(std::is_trivially_copyable_v<RecordRingMeta>);

static_assert(sizeof(RegistryEntry) == 128);
static_assert(std::is_standard_layout_v<RegistryEntry>);
static_assert(std::is_trivially_copyable_v<RegistryEntry>);

static_assert(sizeof(RegistryPayload) == 256 + 128 * MAX_REGISTRY_ENTRIES);
static_assert(std::is_standard_layout_v<RegistryPayload>);
static_assert(std::is_trivially_copyable_v<RegistryPayload>);

} // namespace shmfx
