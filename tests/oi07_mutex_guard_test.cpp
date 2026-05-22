#include "shmfx/shm_lifecycle.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct SharedMutexFixture {
    pthread_mutex_t mutex;
};

void init_robust_mutex(pthread_mutex_t& mutex) {
    pthread_mutexattr_t attr{};
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0);
    assert(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0);
    assert(pthread_mutex_init(&mutex, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);
}

} // namespace

int main() {
    auto* fixture = static_cast<SharedMutexFixture*>(
        mmap(nullptr, sizeof(SharedMutexFixture), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    assert(fixture != MAP_FAILED);
    init_robust_mutex(fixture->mutex);

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(pthread_mutex_lock(&fixture->mutex) == 0);
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));

    {
        shmfx::ShmMutexGuard guard(fixture->mutex);
        assert(guard.locked());
        assert(guard.prior_owner_dead());
        assert(guard.error() == shmfx::ErrorCode::PriorOwnerDead);

        // Test recovery contract: caller repairs protected state first, then marks consistent.
        auto marked = guard.mark_consistent();
        assert(marked);
        assert(!guard.prior_owner_dead());
        assert(guard.error() == shmfx::ErrorCode::Ok);
    }

    {
        shmfx::ShmMutexGuard guard(fixture->mutex);
        assert(guard.locked());
        assert(!guard.prior_owner_dead());
        assert(guard.error() == shmfx::ErrorCode::Ok);
    }

    assert(pthread_mutex_destroy(&fixture->mutex) == 0);
    assert(munmap(fixture, sizeof(SharedMutexFixture)) == 0);

    std::puts("oi07_mutex_guard_test: ok");
    return 0;
}
