#include "shmfx/shm_lifecycle.h"

#include "shmfx/shm_types.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shmfx {
namespace {

std::mutex g_heartbeat_mutex;
std::vector<ShmHeader*> g_heartbeat_headers;
std::thread g_heartbeat_thread;
bool g_heartbeat_stop = false;

/// Runs one heartbeat update for every header owned by this process.
void heartbeat_tick_locked(std::uint64_t now) {
    for (ShmHeader* header : g_heartbeat_headers) {
        if (header == nullptr) {
            continue;
        }
        std::atomic_ref<std::uint64_t>(header->heartbeat_counter).fetch_add(
            1, std::memory_order_release);
        std::atomic_ref<std::uint64_t>(header->heartbeat_last_ns).store(
            now, std::memory_order_release);
    }
}

/// Background worker shared by all owner handles in this process.
void heartbeat_worker() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
            if (g_heartbeat_stop) {
                return;
            }
            heartbeat_tick_locked(monotonic_ns());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_TICK_MS));
    }
}

/// Converts a POSIX shm name into its /dev/shm path suffix.
std::string shm_maps_token(std::string_view name) {
    std::string token("/dev/shm");
    if (!name.empty() && name.front() != '/') {
        token.push_back('/');
    }
    token.append(name.data(), name.size());
    return token;
}

/// Returns whether a directory name is a decimal pid.
bool is_pid_dir_name(const char* name) noexcept {
    if (name == nullptr || *name == '\0') {
        return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
    }
    return true;
}

} // namespace

ShmMutexGuard::ShmMutexGuard(pthread_mutex_t& mutex) noexcept : mutex_(&mutex) {
    const int rc = pthread_mutex_lock(mutex_);
    if (rc == 0) {
        locked_ = true;
        error_ = ErrorCode::Ok;
        return;
    }
    if (rc == EOWNERDEAD) {
        locked_ = true;
        prior_owner_dead_ = true;
        error_ = ErrorCode::PriorOwnerDead;
        return;
    }
    mutex_ = nullptr;
    error_ = ErrorCode::PriorOwnerDead;
}

ShmMutexGuard::~ShmMutexGuard() {
    reset();
}

ShmMutexGuard::ShmMutexGuard(ShmMutexGuard&& other) noexcept
    : mutex_(other.mutex_),
      locked_(other.locked_),
      prior_owner_dead_(other.prior_owner_dead_),
      error_(other.error_) {
    other.mutex_ = nullptr;
    other.locked_ = false;
    other.prior_owner_dead_ = false;
    other.error_ = ErrorCode::Ok;
}

ShmMutexGuard& ShmMutexGuard::operator=(ShmMutexGuard&& other) noexcept {
    if (this != &other) {
        reset();
        mutex_ = other.mutex_;
        locked_ = other.locked_;
        prior_owner_dead_ = other.prior_owner_dead_;
        error_ = other.error_;
        other.mutex_ = nullptr;
        other.locked_ = false;
        other.prior_owner_dead_ = false;
        other.error_ = ErrorCode::Ok;
    }
    return *this;
}

bool ShmMutexGuard::locked() const noexcept {
    return locked_;
}

bool ShmMutexGuard::prior_owner_dead() const noexcept {
    return prior_owner_dead_;
}

ErrorCode ShmMutexGuard::error() const noexcept {
    return error_;
}

Result<void> ShmMutexGuard::mark_consistent() noexcept {
    if (!locked_ || mutex_ == nullptr) {
        return error_;
    }
    if (!prior_owner_dead_) {
        return {};
    }
    if (pthread_mutex_consistent(mutex_) != 0) {
        return ErrorCode::PriorOwnerDead;
    }
    prior_owner_dead_ = false;
    error_ = ErrorCode::Ok;
    return {};
}

void ShmMutexGuard::reset() noexcept {
    if (locked_ && mutex_ != nullptr) {
        pthread_mutex_unlock(mutex_);
    }
    mutex_ = nullptr;
    locked_ = false;
    prior_owner_dead_ = false;
    error_ = ErrorCode::Ok;
}

std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull * 1000 * 1000 +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool read_proc_start_time(std::uint32_t pid, std::uint64_t& out_start_time) noexcept {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) {
        return false;
    }

    std::string line;
    std::getline(stat, line);
    const std::size_t close_paren = line.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= line.size()) {
        return false;
    }

    std::string tail = line.substr(close_paren + 2);
    std::size_t start = 0;
    int field = 3;
    while (field < 22) {
        start = tail.find(' ', start);
        if (start == std::string::npos) {
            return false;
        }
        while (start < tail.size() && tail[start] == ' ') {
            ++start;
        }
        ++field;
    }

    try {
        out_start_time = std::stoull(tail.substr(start));
        return true;
    } catch (...) {
        return false;
    }
}

bool owner_is_dead(const ShmHeader& header, std::uint64_t now_mono_ns) noexcept {
    auto& owner_pid_storage = const_cast<std::uint32_t&>(header.owner_pid);
    const std::uint32_t pid = std::atomic_ref<std::uint32_t>(owner_pid_storage).load(
        std::memory_order_acquire);
    const std::uint64_t expected_start_time = header.owner_start_time;

    if (pid == 0) {
        return true;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) {
        return true;
    }

    std::uint64_t current_start_time = 0;
    if (expected_start_time != 0 && read_proc_start_time(pid, current_start_time) &&
        current_start_time != expected_start_time) {
        return true;
    }

    auto& heartbeat_storage = const_cast<std::uint64_t&>(header.heartbeat_last_ns);
    const std::uint64_t last = std::atomic_ref<std::uint64_t>(heartbeat_storage).load(
        std::memory_order_acquire);
    if (last == 0 || now_mono_ns < last) {
        return false;
    }
    return (now_mono_ns - last) > HEARTBEAT_DEAD_NS;
}

void register_heartbeat_owner(ShmHeader& header) {
    const std::uint64_t now = monotonic_ns();
    std::atomic_ref<std::uint64_t>(header.heartbeat_counter).fetch_add(
        1, std::memory_order_release);
    std::atomic_ref<std::uint64_t>(header.heartbeat_last_ns).store(now, std::memory_order_release);

    std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
    if (std::find(g_heartbeat_headers.begin(), g_heartbeat_headers.end(), &header) ==
        g_heartbeat_headers.end()) {
        g_heartbeat_headers.push_back(&header);
    }
    if (!g_heartbeat_thread.joinable()) {
        g_heartbeat_stop = false;
        g_heartbeat_thread = std::thread(heartbeat_worker);
    }
}

void unregister_heartbeat_owner(ShmHeader& header) noexcept {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
        g_heartbeat_headers.erase(
            std::remove(g_heartbeat_headers.begin(), g_heartbeat_headers.end(), &header),
            g_heartbeat_headers.end());
        if (g_heartbeat_headers.empty() && g_heartbeat_thread.joinable()) {
            g_heartbeat_stop = true;
            to_join = std::move(g_heartbeat_thread);
        }
    }
    if (to_join.joinable()) {
        to_join.join();
    }
}

bool any_process_maps_segment(std::string_view name) {
    const std::string token = shm_maps_token(name);
    DIR* proc = opendir("/proc");
    if (proc == nullptr) {
        return true;
    }

    bool found = false;
    while (!found) {
        dirent* ent = readdir(proc);
        if (ent == nullptr) {
            break;
        }
        if (!is_pid_dir_name(ent->d_name)) {
            continue;
        }

        std::ifstream maps(std::string("/proc/") + ent->d_name + "/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find(token) != std::string::npos) {
                found = true;
                break;
            }
        }
    }
    closedir(proc);
    return found;
}

} // namespace shmfx
