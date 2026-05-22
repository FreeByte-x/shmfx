#include "shmfx/shm_registry.h"

#include "shmfx/shm_lifecycle.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shmfx {
namespace {

/// Registry segment size: framework header followed directly by RegistryPayload.
constexpr std::size_t kRegistryTotalSize = SHMFX_HEADER_SIZE + sizeof(RegistryPayload);

/// Raw mapping state used only while bootstrapping the registry singleton.
struct RawMapping {
    void* base = nullptr;
    std::size_t size = 0;
    int fd = -1;
};

/// Converts a registry bootstrap errno into a public error code.
ErrorCode registry_errno_to_error(int err) noexcept {
    switch (err) {
    case EEXIST:
        return ErrorCode::AlreadyExists;
    case EACCES:
    case EPERM:
        return ErrorCode::PermissionDenied;
    case ENOSPC:
        return ErrorCode::QuotaExceeded;
    default:
        return ErrorCode::CorruptedHeader;
    }
}

/// Validates enough of a segment header for janitor decisions.
bool janitor_header_valid(const ShmHeader& header, std::size_t mapped_size) noexcept {
    if (header.magic != SHMFX_MAGIC || header.version_major != SHMFX_VERSION_MAJOR) {
        return false;
    }
    if (header.total_size != mapped_size || header.total_size < SHMFX_HEADER_SIZE) {
        return false;
    }
    const std::uint64_t meta_end = static_cast<std::uint64_t>(header.meta_offset) + header.meta_size;
    const std::uint64_t payload_end = static_cast<std::uint64_t>(header.payload_offset) + header.payload_size;
    return header.meta_offset >= SHMFX_HEADER_SIZE && meta_end <= header.total_size &&
           header.payload_offset >= meta_end && payload_end <= header.total_size;
}

/// Writes a null-terminated name into an ABI fixed char buffer.
void copy_name(char (&dest)[SHMFX_NAME_BYTES], std::string_view name) noexcept {
    std::memset(dest, 0, SHMFX_NAME_BYTES);
    std::memcpy(dest, name.data(), std::min(name.size(), SHMFX_NAME_BYTES - 1));
}

/// Initializes the registry control mutex.
ErrorCode init_mutex(pthread_mutex_t& mutex) noexcept {
    pthread_mutexattr_t attr{};
    if (pthread_mutexattr_init(&attr) != 0) {
        return ErrorCode::CorruptedHeader;
    }
    const int shared_rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    const int robust_rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    const int init_rc = (shared_rc == 0 && robust_rc == 0) ? pthread_mutex_init(&mutex, &attr) : EINVAL;
    pthread_mutexattr_destroy(&attr);
    return init_rc == 0 ? ErrorCode::Ok : ErrorCode::CorruptedHeader;
}

/// Returns whether a registry bitmap slot is occupied.
bool bit_is_set(const RegistryPayload& payload, std::uint32_t index) noexcept {
    return (payload.bitmap[index / 8] & (1u << (index % 8))) != 0;
}

/// Sets a registry bitmap slot.
void set_bit(RegistryPayload& payload, std::uint32_t index) noexcept {
    payload.bitmap[index / 8] = static_cast<std::uint8_t>(payload.bitmap[index / 8] | (1u << (index % 8)));
}

/// Clears a registry bitmap slot.
void clear_bit(RegistryPayload& payload, std::uint32_t index) noexcept {
    payload.bitmap[index / 8] = static_cast<std::uint8_t>(payload.bitmap[index / 8] & ~(1u << (index % 8)));
}

/// Returns true when an entry's name matches a string_view.
bool entry_name_equals(const RegistryEntry& entry, std::string_view name) noexcept {
    return std::string_view(entry.name, strnlen(entry.name, SHMFX_NAME_BYTES)) == name;
}

/// Returns the stable string_view name for a registry entry.
std::string_view entry_name(const RegistryEntry& entry) noexcept {
    return {entry.name, strnlen(entry.name, SHMFX_NAME_BYTES)};
}

/// Publishes a registry slot with seqlock odd/even sequencing.
void publish_slot(RegistryEntry& slot, const RegistryEntry& source) noexcept {
    std::atomic_ref<std::uint32_t> seq(slot.seq_storage);
    const std::uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_release);

    const std::uint32_t next_seq = s + 2;
    slot = source;
    slot.seq_storage = s + 1;

    std::atomic_thread_fence(std::memory_order_release);
    seq.store(next_seq, std::memory_order_release);
}

/// Resets a damaged registry slot while publishing an even seqlock value.
void clear_slot_for_recovery(RegistryPayload& payload, std::uint32_t index) noexcept {
    RegistryEntry& slot = payload.slots[index];
    std::atomic_ref<std::uint32_t> seq(slot.seq_storage);
    const std::uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s | 1u, std::memory_order_release);
    slot = RegistryEntry{};
    std::atomic_thread_fence(std::memory_order_release);
    seq.store((s + 2u) & ~1u, std::memory_order_release);
    clear_bit(payload, index);
}

/// Repairs registry bitmap, seqlock, and count after prior owner death.
void recover_registry_invariant(RegistryPayload& payload) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        const bool occupied = bit_is_set(payload, i);
        RegistryEntry& slot = payload.slots[i];
        const std::uint32_t seq = std::atomic_ref<std::uint32_t>(slot.seq_storage).load(
            std::memory_order_acquire);
        const bool writer_in_progress = (seq & 1u) != 0;
        const bool has_name = slot.name[0] != '\0';

        if (writer_in_progress || (occupied && !has_name) || (!occupied && has_name)) {
            clear_slot_for_recovery(payload, i);
            continue;
        }
        if (occupied) {
            ++count;
        }
    }
    std::atomic_ref<std::uint32_t>(payload.count_storage).store(count, std::memory_order_release);
}

/// Locks registry control mutex and recovers the registry invariant if needed.
Result<ShmMutexGuard> lock_registry_control(ShmHeader& header, RegistryPayload& payload) noexcept {
    ShmMutexGuard guard(header.control_mutex);
    if (!guard.locked()) {
        return guard.error();
    }
    if (guard.prior_owner_dead()) {
        recover_registry_invariant(payload);
        if (auto marked = guard.mark_consistent(); !marked) {
            return marked.error();
        }
    }
    return guard;
}

/// Updates the registry last_seen_ns field for an existing entry.
void update_registry_last_seen(Registry& registry, RegistryEntry entry, std::uint64_t now_mono_ns) {
    entry.last_seen_ns = now_mono_ns;
    [[maybe_unused]] auto updated = registry.register_segment(entry);
}

/// Takes a consistent slot snapshot or returns false after bounded retries.
bool snapshot_slot(const RegistryEntry& slot, RegistryEntry& out) noexcept {
    auto& seq_storage = const_cast<std::uint32_t&>(slot.seq_storage);
    std::atomic_ref<std::uint32_t> seq(seq_storage);
    for (int retry = 0; retry < 100; ++retry) {
        const std::uint32_t s1 = seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0) {
            continue;
        }
        out = slot;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t s2 = seq.load(std::memory_order_acquire);
        if (s1 == s2) {
            return true;
        }
    }
    return false;
}

/// Creates the registry segment from raw POSIX calls to avoid Registry recursion.
Result<RawMapping> create_registry_raw() {
    const int fd = ::shm_open(REGISTRY_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return registry_errno_to_error(errno);
    }
    if (::ftruncate(fd, static_cast<off_t>(kRegistryTotalSize)) != 0) {
        const int saved = errno;
        ::close(fd);
        ::shm_unlink(REGISTRY_NAME);
        return registry_errno_to_error(saved);
    }
    void* base = ::mmap(nullptr, kRegistryTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        const int saved = errno;
        ::close(fd);
        ::shm_unlink(REGISTRY_NAME);
        return registry_errno_to_error(saved);
    }

    std::memset(base, 0, kRegistryTotalSize);
    auto* header = static_cast<ShmHeader*>(base);
    header->magic = SHMFX_MAGIC;
    header->version_major = SHMFX_VERSION_MAJOR;
    header->version_minor = SHMFX_VERSION_MINOR;
    header->segment_type = static_cast<std::uint32_t>(SegmentType::Registry);
    header->flags = RobustMutex;
    header->total_size = kRegistryTotalSize;
    header->payload_size = sizeof(RegistryPayload);
    header->meta_offset = SHMFX_HEADER_SIZE;
    header->meta_size = 0;
    header->payload_offset = SHMFX_HEADER_SIZE;
    header->creator_pid = static_cast<std::uint32_t>(::getpid());
    copy_name(header->name, REGISTRY_NAME);
    header->owner_pid = header->creator_pid;
    header->state = static_cast<std::uint32_t>(SegmentState::Active);
    header->ref_count = 1;
    if (const ErrorCode err = init_mutex(header->control_mutex); err != ErrorCode::Ok) {
        ::munmap(base, kRegistryTotalSize);
        ::close(fd);
        ::shm_unlink(REGISTRY_NAME);
        return err;
    }
    return RawMapping{base, kRegistryTotalSize, fd};
}

} // namespace

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

Registry::Registry() {
    if (auto created = create_registry_raw()) {
        const RawMapping raw = created.value();
        handle_ = ShmHandle(raw.base, raw.size, raw.fd, AttachMode::ReadWrite, true);
    } else if (created.error() == ErrorCode::AlreadyExists) {
        auto opened = ShmManager::attach(REGISTRY_NAME, AttachMode::ReadWrite);
        if (opened) {
            handle_ = std::move(opened.value());
        }
    }
}

RegistryPayload& Registry::payload() noexcept {
    auto* bytes = reinterpret_cast<std::byte*>(&handle_.header());
    return *reinterpret_cast<RegistryPayload*>(bytes + handle_.header().payload_offset);
}

Result<void> Registry::register_segment(const RegistryEntry& entry) {
    if (!handle_.valid()) {
        return ErrorCode::NotFound;
    }
    RegistryPayload& p = payload();
    auto guard = lock_registry_control(handle_.header(), p);
    if (!guard) {
        return guard.error();
    }

    std::uint32_t free_slot = MAX_REGISTRY_ENTRIES;
    for (std::uint32_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        if (!bit_is_set(p, i)) {
            if (free_slot == MAX_REGISTRY_ENTRIES) {
                free_slot = i;
            }
            continue;
        }
        RegistryEntry snap{};
        if (snapshot_slot(p.slots[i], snap) && entry_name_equals(snap, entry.name)) {
            publish_slot(p.slots[i], entry);
            return {};
        }
    }

    if (free_slot == MAX_REGISTRY_ENTRIES) {
        return ErrorCode::QuotaExceeded;
    }

    set_bit(p, free_slot);
    std::atomic_ref<std::uint32_t>(p.count_storage).fetch_add(1, std::memory_order_acq_rel);
    publish_slot(p.slots[free_slot], entry);
    return {};
}

Result<void> Registry::unregister(std::string_view name) {
    if (!handle_.valid()) {
        return ErrorCode::NotFound;
    }
    RegistryPayload& p = payload();
    auto guard = lock_registry_control(handle_.header(), p);
    if (!guard) {
        return guard.error();
    }

    for (std::uint32_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        if (!bit_is_set(p, i)) {
            continue;
        }
        RegistryEntry snap{};
        if (snapshot_slot(p.slots[i], snap) && entry_name_equals(snap, name)) {
            RegistryEntry empty{};
            publish_slot(p.slots[i], empty);
            clear_bit(p, i);
            std::atomic_ref<std::uint32_t>(p.count_storage).fetch_sub(1, std::memory_order_acq_rel);
            return {};
        }
    }

    return ErrorCode::NotFound;
}

std::vector<RegistryEntry> Registry::list(std::string_view prefix) {
    std::vector<RegistryEntry> out;
    if (!handle_.valid()) {
        return out;
    }

    RegistryPayload& p = payload();
    for (std::uint32_t i = 0; i < MAX_REGISTRY_ENTRIES; ++i) {
        if (!bit_is_set(p, i)) {
            continue;
        }

        RegistryEntry snap{};
        if (!snapshot_slot(p.slots[i], snap)) {
            auto guard = lock_registry_control(handle_.header(), p);
            if (!guard) {
                continue;
            }
            snap = p.slots[i];
        }

        const std::string_view name(snap.name, strnlen(snap.name, SHMFX_NAME_BYTES));
        if (prefix.empty() || name.substr(0, prefix.size()) == prefix) {
            out.push_back(snap);
        }
    }
    return out;
}

void Registry::gc() {
    const std::vector<RegistryEntry> entries = list();
    const std::uint64_t now = monotonic_ns();

    for (const RegistryEntry& entry : entries) {
        const std::string name(entry_name(entry));
        if (name.empty() || name == REGISTRY_NAME) {
            continue;
        }

        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) {
            if (errno == ENOENT) {
                [[maybe_unused]] auto removed = unregister(name);
            }
            continue;
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(SHMFX_HEADER_SIZE)) {
            ::close(fd);
            [[maybe_unused]] auto removed = unregister(name);
            continue;
        }

        const std::size_t size = static_cast<std::size_t>(st.st_size);
        void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            ::close(fd);
            continue;
        }

        auto* header = static_cast<ShmHeader*>(base);
        if (!janitor_header_valid(*header, size)) {
            ::munmap(base, size);
            ::close(fd);
            [[maybe_unused]] auto removed = unregister(name);
            continue;
        }

        const bool dead = owner_is_dead(*header, now);
        const std::uint32_t rc = std::atomic_ref<std::uint32_t>(header->ref_count).load(
            std::memory_order_acquire);
        const auto state = static_cast<SegmentState>(
            std::atomic_ref<std::uint32_t>(header->state).load(std::memory_order_acquire));

        if (dead && rc == 0) {
            ShmMutexGuard guard(header->control_mutex);
            if (guard.locked()) {
                if (guard.prior_owner_dead()) {
                    [[maybe_unused]] auto marked = guard.mark_consistent();
                }
                const std::uint32_t rc2 = std::atomic_ref<std::uint32_t>(header->ref_count).load(
                    std::memory_order_acquire);
                if (rc2 == 0 && owner_is_dead(*header, now)) {
                    std::atomic_ref<std::uint32_t>(header->state).store(
                        static_cast<std::uint32_t>(SegmentState::Dead), std::memory_order_release);
                    ::shm_unlink(name.c_str());
                    [[maybe_unused]] auto removed = unregister(name);
                }
            }
        } else if (dead && rc > 0 && state == SegmentState::Active) {
            std::uint32_t expected = static_cast<std::uint32_t>(SegmentState::Active);
            std::atomic_ref<std::uint32_t>(header->state).compare_exchange_strong(
                expected, static_cast<std::uint32_t>(SegmentState::Draining),
                std::memory_order_acq_rel);
            update_registry_last_seen(*this, entry, now);
        } else if (state == SegmentState::Draining && entry.last_seen_ns != 0 &&
                   now > entry.last_seen_ns && now - entry.last_seen_ns > SEGMENT_ZOMBIE_NS) {
            ::munmap(base, size);
            ::close(fd);
            if (!any_process_maps_segment(name)) {
                ::shm_unlink(name.c_str());
                [[maybe_unused]] auto removed = unregister(name);
            }
            continue;
        }

        ::munmap(base, size);
        ::close(fd);
    }
}

} // namespace shmfx
