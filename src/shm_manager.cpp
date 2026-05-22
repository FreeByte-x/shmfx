#include "shmfx/shm_manager.h"

#include "shmfx/shm_lifecycle.h"
#include "shmfx/shm_registry.h"
#include "shmfx/shm_security.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shmfx {
namespace {

constexpr std::uint32_t kDefaultCreateFlags = RobustMutex;

/// Returns CLOCK_REALTIME in nanoseconds for immutable creation metadata.
std::uint64_t realtime_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

/// Converts errno from shm_open-style calls into the public error surface.
ErrorCode errno_to_error(int err) noexcept {
    switch (err) {
    case EEXIST:
        return ErrorCode::AlreadyExists;
    case ENOENT:
        return ErrorCode::NotFound;
    case EACCES:
    case EPERM:
        return ErrorCode::PermissionDenied;
    case ENOSPC:
    case EFBIG:
        return ErrorCode::QuotaExceeded;
    default:
        return ErrorCode::CorruptedHeader;
    }
}

/// Initializes a robust process-shared mutex in mapped shared memory.
ErrorCode init_control_mutex(pthread_mutex_t& mutex) noexcept {
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

/// Validates immutable and bounds-sensitive header fields after mmap.
ErrorCode validate_header(const ShmHeader& header, std::size_t mapped_size) noexcept {
    if (header.magic != SHMFX_MAGIC) {
        return ErrorCode::CorruptedHeader;
    }
    if (header.version_major != SHMFX_VERSION_MAJOR) {
        return ErrorCode::VersionMismatch;
    }
    if (header.total_size != mapped_size || header.total_size < SHMFX_HEADER_SIZE) {
        return ErrorCode::CorruptedHeader;
    }
    if (header.meta_offset < SHMFX_HEADER_SIZE || header.meta_offset > header.total_size) {
        return ErrorCode::CorruptedHeader;
    }
    const std::uint64_t meta_end = static_cast<std::uint64_t>(header.meta_offset) + header.meta_size;
    const std::uint64_t payload_end = static_cast<std::uint64_t>(header.payload_offset) + header.payload_size;
    if (meta_end > header.total_size || header.payload_offset < meta_end || payload_end > header.total_size) {
        return ErrorCode::CorruptedHeader;
    }
    return ErrorCode::Ok;
}

/// Writes a null-terminated name into a fixed ABI char buffer.
void copy_name(char (&dest)[SHMFX_NAME_BYTES], std::string_view name) noexcept {
    std::memset(dest, 0, SHMFX_NAME_BYTES);
    std::memcpy(dest, name.data(), std::min(name.size(), SHMFX_NAME_BYTES - 1));
}

/// Returns whether the registry records this segment as HMAC protected.
bool registry_expects_hmac(std::string_view name) {
    if (name == REGISTRY_NAME) {
        return false;
    }
    for (const RegistryEntry& entry : Registry::instance().list(name)) {
        const std::string_view entry_name(entry.name, strnlen(entry.name, SHMFX_NAME_BYTES));
        if (entry_name == name) {
            return (entry.flags & HmacEnabled) != 0;
        }
    }
    return false;
}

} // namespace

ShmHandle::ShmHandle(void* base, std::size_t size, int fd, AttachMode mode, bool owner) noexcept
    : base_(base), size_(size), fd_(fd), mode_(mode), owner_(owner) {}

ShmHandle::ShmHandle(ShmHandle&& other) noexcept
    : base_(other.base_),
      size_(other.size_),
      fd_(other.fd_),
      mode_(other.mode_),
      owner_(other.owner_) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.mode_ = AttachMode::ReadOnly;
    other.owner_ = false;
}

ShmHandle& ShmHandle::operator=(ShmHandle&& other) noexcept {
    if (this != &other) {
        reset();
        base_ = other.base_;
        size_ = other.size_;
        fd_ = other.fd_;
        mode_ = other.mode_;
        owner_ = other.owner_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.mode_ = AttachMode::ReadOnly;
        other.owner_ = false;
    }
    return *this;
}

ShmHandle::~ShmHandle() {
    reset();
}

bool ShmHandle::valid() const noexcept {
    return base_ != nullptr;
}

ShmHeader& ShmHandle::header() noexcept {
    return *static_cast<ShmHeader*>(base_);
}

const ShmHeader& ShmHandle::header() const noexcept {
    return *static_cast<const ShmHeader*>(base_);
}

std::span<std::byte> ShmHandle::metadata() noexcept {
    auto* bytes = static_cast<std::byte*>(base_);
    return {bytes + header().meta_offset, header().meta_size};
}

std::span<const std::byte> ShmHandle::metadata() const noexcept {
    auto* bytes = static_cast<const std::byte*>(base_);
    return {bytes + header().meta_offset, header().meta_size};
}

std::span<std::byte> ShmHandle::payload() noexcept {
    auto* bytes = static_cast<std::byte*>(base_);
    return {bytes + header().payload_offset, header().payload_size};
}

std::span<const std::byte> ShmHandle::payload() const noexcept {
    auto* bytes = static_cast<const std::byte*>(base_);
    return {bytes + header().payload_offset, header().payload_size};
}

std::string_view ShmHandle::name() const noexcept {
    return {header().name, strnlen(header().name, SHMFX_NAME_BYTES)};
}

AttachMode ShmHandle::mode() const noexcept {
    return mode_;
}

void ShmHandle::reset() noexcept {
    if (base_ != nullptr) {
        const std::string segment_name(name());
        const bool is_registry = segment_name == REGISTRY_NAME;
        if (owner_) {
            unregister_heartbeat_owner(header());
        }
        if (mode_ == AttachMode::ReadWrite) {
            const std::uint32_t previous = std::atomic_ref<std::uint32_t>(header().ref_count).fetch_sub(
                1, std::memory_order_acq_rel);
            const auto state = static_cast<SegmentState>(
                std::atomic_ref<std::uint32_t>(header().state).load(std::memory_order_acquire));
            if (owner_ && !is_registry && previous == 1 && state == SegmentState::Draining) {
                std::atomic_ref<std::uint32_t>(header().state).store(
                    static_cast<std::uint32_t>(SegmentState::Dead), std::memory_order_release);
                ::shm_unlink(segment_name.c_str());
                [[maybe_unused]] auto unregistered = Registry::instance().unregister(segment_name);
            }
        }
        ::munmap(base_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    base_ = nullptr;
    size_ = 0;
    fd_ = -1;
    mode_ = AttachMode::ReadOnly;
    owner_ = false;
}

Result<ShmHandle> ShmManager::create(const CreateOptions& options) {
    if (auto name = parse_and_validate_name(options.name); !name) {
        return name.error();
    }
    if (auto security = validate_create_security(options, Registry::instance().list()); !security) {
        return security.error();
    }

    const std::uint64_t payload_offset = align_up(SHMFX_HEADER_SIZE + options.meta_size,
                                                 SHMFX_CACHE_LINE_BYTES);
    if (payload_offset > options.total_size) {
        return ErrorCode::QuotaExceeded;
    }

    const int fd = ::shm_open(options.name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return errno_to_error(errno);
    }
    if (auto perms = apply_permissions(fd, options.perm); !perms) {
        const ErrorCode err = perms.error();
        ::close(fd);
        ::shm_unlink(options.name.c_str());
        return err;
    }

    if (::ftruncate(fd, static_cast<off_t>(options.total_size)) != 0) {
        const int saved = errno;
        ::close(fd);
        ::shm_unlink(options.name.c_str());
        return errno_to_error(saved);
    }

    void* base = ::mmap(nullptr, options.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        const int saved = errno;
        ::close(fd);
        ::shm_unlink(options.name.c_str());
        return errno_to_error(saved);
    }

    auto* header = static_cast<ShmHeader*>(base);
    std::memset(header, 0, SHMFX_HEADER_SIZE);
    header->magic = SHMFX_MAGIC;
    header->version_major = SHMFX_VERSION_MAJOR;
    header->version_minor = SHMFX_VERSION_MINOR;
    header->segment_type = static_cast<std::uint32_t>(options.type);
    header->flags = options.flags | kDefaultCreateFlags;
    header->total_size = options.total_size;
    header->meta_offset = SHMFX_HEADER_SIZE;
    header->meta_size = static_cast<std::uint32_t>(options.meta_size);
    header->payload_offset = static_cast<std::uint32_t>(payload_offset);
    header->payload_size = options.total_size - payload_offset;
    header->creator_pid = static_cast<std::uint32_t>(::getpid());
    header->created_at_ns = realtime_ns();
    std::uint64_t creator_start_time = 0;
    [[maybe_unused]] const bool start_time_read = read_proc_start_time(
        header->creator_pid, creator_start_time);
    header->creator_start_time = creator_start_time;
    copy_name(header->name, options.name);
    header->owner_pid = header->creator_pid;
    header->owner_start_time = header->creator_start_time;
    header->ref_count = 1;
    header->state = static_cast<std::uint32_t>(SegmentState::Active);
    header->heartbeat_last_ns = monotonic_ns();

    if (auto signed_header = sign_header_if_enabled(*header); !signed_header) {
        const ErrorCode err = signed_header.error();
        ::munmap(base, options.total_size);
        ::close(fd);
        ::shm_unlink(options.name.c_str());
        return err;
    }

    if (const ErrorCode mutex_error = init_control_mutex(header->control_mutex); mutex_error != ErrorCode::Ok) {
        ::munmap(base, options.total_size);
        ::close(fd);
        ::shm_unlink(options.name.c_str());
        return mutex_error;
    }

    ShmHandle handle(base, options.total_size, fd, AttachMode::ReadWrite, true);
    register_heartbeat_owner(*header);
    if (options.name != REGISTRY_NAME) {
        RegistryEntry entry{};
        copy_name(entry.name, options.name);
        entry.total_size = options.total_size;
        entry.segment_type = static_cast<std::uint32_t>(options.type);
        entry.owner_pid = header->owner_pid;
        entry.owner_start_time = header->owner_start_time;
        entry.flags = header->flags;
        entry.created_at_ns = header->created_at_ns;
        entry.last_seen_ns = monotonic_ns();
        if (auto registered = Registry::instance().register_segment(entry); !registered) {
            ::shm_unlink(options.name.c_str());
            return registered.error();
        }
    }

    return handle;
}

Result<ShmHandle> ShmManager::attach(std::string_view name, AttachMode mode) {
    if (auto parsed = parse_and_validate_name(name); !parsed) {
        return parsed.error();
    }
    if (name != REGISTRY_NAME) {
        Registry::instance().gc();
    }

    const std::string owned_name(name);
    const int flags = mode == AttachMode::ReadOnly ? O_RDONLY : O_RDWR;
    const int fd = ::shm_open(owned_name.c_str(), flags, 0);
    if (fd < 0) {
        return errno_to_error(errno);
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(SHMFX_HEADER_SIZE)) {
        const int saved = errno;
        ::close(fd);
        return errno_to_error(saved);
    }

    const int prot = mode == AttachMode::ReadOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* base = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), prot, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        const int saved = errno;
        ::close(fd);
        return errno_to_error(saved);
    }

    auto* header = static_cast<ShmHeader*>(base);
    if (const ErrorCode valid = validate_header(*header, static_cast<std::size_t>(st.st_size));
        valid != ErrorCode::Ok) {
        ::munmap(base, static_cast<std::size_t>(st.st_size));
        ::close(fd);
        return valid;
    }
    if (std::string_view(header->name, strnlen(header->name, SHMFX_NAME_BYTES)) != name) {
        ::munmap(base, static_cast<std::size_t>(st.st_size));
        ::close(fd);
        return ErrorCode::CorruptedHeader;
    }
    if (registry_expects_hmac(name) && (header->flags & HmacEnabled) == 0) {
        ::munmap(base, static_cast<std::size_t>(st.st_size));
        ::close(fd);
        return ErrorCode::CorruptedHeader;
    }
    if (auto hmac = verify_header_hmac(*header); !hmac) {
        ::munmap(base, static_cast<std::size_t>(st.st_size));
        ::close(fd);
        return hmac.error();
    }

    const auto state = static_cast<SegmentState>(
        std::atomic_ref<std::uint32_t>(header->state).load(std::memory_order_acquire));
    if (state == SegmentState::Dead) {
        ::munmap(base, static_cast<std::size_t>(st.st_size));
        ::close(fd);
        return ErrorCode::SegmentDead;
    }
    if (mode == AttachMode::ReadWrite) {
        std::atomic_ref<std::uint32_t>(header->ref_count).fetch_add(1, std::memory_order_acq_rel);
        const auto state_after_bump = static_cast<SegmentState>(
            std::atomic_ref<std::uint32_t>(header->state).load(std::memory_order_acquire));
        if (state_after_bump == SegmentState::Dead) {
            std::atomic_ref<std::uint32_t>(header->ref_count).fetch_sub(1, std::memory_order_acq_rel);
            ::munmap(base, static_cast<std::size_t>(st.st_size));
            ::close(fd);
            return ErrorCode::SegmentDead;
        }
    }

    return ShmHandle(base, static_cast<std::size_t>(st.st_size), fd, mode, false);
}

Result<void> ShmManager::destroy(std::string_view name) {
    if (auto parsed = parse_and_validate_name(name); !parsed) {
        return parsed.error();
    }
    const std::string owned_name(name);
    if (::shm_unlink(owned_name.c_str()) != 0) {
        return errno_to_error(errno);
    }
    if (name != REGISTRY_NAME) {
        return Registry::instance().unregister(name);
    }
    return {};
}

} // namespace shmfx
