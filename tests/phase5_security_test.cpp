#include "shmfx/shm_manager.h"
#include "shmfx/shm_security.h"

#include <algorithm>
#include <cassert>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string dev_shm_path(const std::string& name) {
    return "/dev/shm/" + name.substr(1);
}

} // namespace

int main() {
    const std::string base = std::to_string(::getpid());
    const std::string secure_name = "/shmfx.app.phase5_" + base;
    const std::string denied_name = "/shmfx.bad.phase5_" + base;

    [[maybe_unused]] auto cleanup_secure = shmfx::ShmManager::destroy(secure_name);
    [[maybe_unused]] auto cleanup_denied = shmfx::ShmManager::destroy(denied_name);

    auto invalid = shmfx::parse_and_validate_name("/bad.name");
    assert(!invalid);
    assert(invalid.error() == shmfx::ErrorCode::InvalidName);

    auto denied = shmfx::parse_and_validate_name(denied_name);
    assert(!denied);
    assert(denied.error() == shmfx::ErrorCode::PermissionDenied);

    shmfx::CreateOptions denied_options;
    denied_options.name = denied_name;
    denied_options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 1024;
    auto denied_create = shmfx::ShmManager::create(denied_options);
    assert(!denied_create);
    assert(denied_create.error() == shmfx::ErrorCode::PermissionDenied);

    assert(setenv("SHMFX_HMAC_KEY",
                  "000102030405060708090a0b0c0d0e0f"
                  "101112131415161718191a1b1c1d1e1f",
                  1) == 0);

    shmfx::CreateOptions options;
    options.name = secure_name;
    options.type = shmfx::SegmentType::Raw;
    options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 2048;
    options.meta_size = shmfx::SHMFX_CACHE_LINE_BYTES;
    options.perm = 0600;
    options.flags = shmfx::HmacEnabled;

    auto created = shmfx::ShmManager::create(options);
    assert(created);
    bool hmac_nonzero = false;
    for (const std::uint8_t byte : created.value().header().hmac) {
        hmac_nonzero = hmac_nonzero || byte != 0;
    }
    assert(hmac_nonzero);

    struct stat st {};
    assert(stat(dev_shm_path(secure_name).c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    auto attached = shmfx::ShmManager::attach(secure_name, shmfx::AttachMode::ReadOnly);
    assert(attached);

    created.value().header().version_minor ^= 1u;
    auto tampered = shmfx::ShmManager::attach(secure_name, shmfx::AttachMode::ReadOnly);
    assert(!tampered);
    assert(tampered.error() == shmfx::ErrorCode::CorruptedHeader);
    created.value().header().version_minor ^= 1u;

    std::array<std::uint8_t, shmfx::SHMFX_HMAC_BYTES> original_hmac{};
    std::copy(std::begin(created.value().header().hmac), std::end(created.value().header().hmac),
              original_hmac.begin());
    created.value().header().flags &= ~shmfx::HmacEnabled;
    std::fill(std::begin(created.value().header().hmac), std::end(created.value().header().hmac), 0);
    auto disabled_hmac = shmfx::ShmManager::attach(secure_name, shmfx::AttachMode::ReadOnly);
    assert(!disabled_hmac);
    assert(disabled_hmac.error() == shmfx::ErrorCode::CorruptedHeader);
    created.value().header().flags |= shmfx::HmacEnabled;
    std::copy(original_hmac.begin(), original_hmac.end(), std::begin(created.value().header().hmac));

    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(secure_name);
    unsetenv("SHMFX_HMAC_KEY");

    std::puts("phase5_security_test: ok");
    return 0;
}
