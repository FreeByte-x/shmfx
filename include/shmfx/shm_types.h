#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <sys/types.h>

namespace shmfx {

/// Magic number stored at byte 0 of every shmfx segment header ("SHMF").
inline constexpr std::uint32_t SHMFX_MAGIC = 0x53484D46u;

/// Breaking ABI version. Mismatch means attach must fail.
inline constexpr std::uint16_t SHMFX_VERSION_MAJOR = 1;

/// Additive ABI/API version. Mismatch may warn but should not break attach.
inline constexpr std::uint16_t SHMFX_VERSION_MINOR = 0;

/// Fixed size of ShmHeader in bytes.
inline constexpr std::size_t SHMFX_HEADER_SIZE = 256;

/// Number of immutable header bytes covered by optional HMAC.
inline constexpr std::size_t SHMFX_IMMUTABLE_BYTES = 128;

/// HMAC-SHA256 digest size in bytes.
inline constexpr std::size_t SHMFX_HMAC_BYTES = 32;

/// Fixed byte capacity for null-terminated POSIX shm names in headers.
inline constexpr std::size_t SHMFX_NAME_BYTES = 64;

/// Cache-line alignment used for metadata and hot shared-memory fields.
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t SHMFX_CACHE_LINE_BYTES =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t SHMFX_CACHE_LINE_BYTES = 64;
#endif

static_assert(SHMFX_CACHE_LINE_BYTES >= sizeof(std::uint64_t));
static_assert((SHMFX_CACHE_LINE_BYTES & (SHMFX_CACHE_LINE_BYTES - 1u)) == 0);

/// Hard cap for a single segment in v0.1.
inline constexpr std::size_t MAX_SEGMENT_SIZE = 1ull << 30;       // 1 GiB

/// Planned aggregate framework quota across namespaces.
inline constexpr std::uint64_t TOTAL_QUOTA_BYTES = 4ull << 30;    // 4 GiB

/// Compile-time registry slot count for the v1 registry payload.
inline constexpr std::uint32_t MAX_REGISTRY_ENTRIES = 1024;

/// Bytes needed to track MAX_REGISTRY_ENTRIES occupancy bits.
inline constexpr std::uint32_t REGISTRY_BITMAP_BYTES = MAX_REGISTRY_ENTRIES / 8;

/// Owner heartbeat interval in milliseconds.
inline constexpr std::uint64_t HEARTBEAT_TICK_MS = 100;

/// Producer CAS retry budget for MPSC ring slot claim contention.
inline constexpr std::uint32_t MPSC_CAS_RETRY_BUDGET = 64;

/// Owner heartbeat staleness threshold in nanoseconds.
inline constexpr std::uint64_t HEARTBEAT_DEAD_NS = 3ull * 1000 * 1000 * 1000;

/// DRAINING zombie age threshold before janitor may force unlink.
inline constexpr std::uint64_t SEGMENT_ZOMBIE_NS = 60ull * 60 * 1000 * 1000 * 1000;

/// Bootstrap POSIX shm name for the singleton registry segment.
inline constexpr char REGISTRY_NAME[] = "/shmfx.registry";

/// Reserved bootstrap name for future registry leader-election locking.
inline constexpr char REGISTRY_LOCK_NAME[] = "/shmfx.registry.lock";

/// Segment payload layout category stored in ShmHeader::segment_type.
enum class SegmentType : std::uint32_t {
    /// Free-form bytes owned by the application.
    Raw = 1,
    /// Generic record-based MPSC ring payload.
    RecordRing = 2,
    /// Reserved key/value payload category.
    Kv = 3,
    /// Framework registry payload.
    Registry = 4,
};

/// Lifecycle state stored in ShmHeader::state.
enum class SegmentState : std::uint32_t {
    /// Header exists but segment has not been published.
    Init = 0,
    /// Segment is published and attachable.
    Active = 1,
    /// Owner is shutting down; consumers may drain remaining data.
    Draining = 2,
    /// Terminal state; segment may be unlinked.
    Dead = 3,
};

/// Mapping mode requested by attach().
enum class AttachMode : std::uint8_t {
    /// Map without bumping mutable RW reference count.
    ReadOnly = 0,
    /// Map read/write and participate in lifecycle reference counting.
    ReadWrite = 1,
};

/// Immutable feature flags stored in ShmHeader::flags.
enum SegmentFlags : std::uint32_t {
    /// Header contains HMAC-SHA256 over immutable bytes [0, 128).
    HmacEnabled = 1u << 0,
    /// Control mutex was initialized robust and process-shared.
    RobustMutex = 1u << 1,
    /// Payload uses lock-free ring operations.
    LockfreeRing = 1u << 2,
    /// Payload should be treated as read-only after creation.
    ReadonlyPayload = 1u << 3,
};

/// Options used by ShmManager::create() to initialize a new segment.
struct CreateOptions {
    /// POSIX shm name, normally /shmfx.<namespace>.<name>.
    std::string name;
    /// Payload layout category.
    SegmentType type = SegmentType::Raw;
    /// Total segment size including header, metadata, and payload.
    std::size_t total_size = 0;
    /// Application metadata size immediately after ShmHeader.
    std::size_t meta_size = SHMFX_CACHE_LINE_BYTES;
    /// POSIX mode passed to shm_open/fchmod.
    mode_t perm = 0600;
    /// Bitwise OR of SegmentFlags values.
    std::uint32_t flags = 0;
};

/// Rounds a value up to the next alignment boundary.
///
/// @param value Value to align.
/// @param alignment Power-of-two alignment boundary.
/// @return @p value rounded up to a multiple of @p alignment.
[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t value,
                                               std::uint64_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

} // namespace shmfx
