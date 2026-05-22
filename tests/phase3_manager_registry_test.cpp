#include "shmfx/shm_manager.h"
#include "shmfx/shm_registry.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unistd.h>

int main() {
    const std::string name = "/shmfx.app.phase3_" + std::to_string(::getpid());

    // Clean up any stale object from an interrupted local test run.
    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(name);

    shmfx::CreateOptions options;
    options.name = name;
    options.type = shmfx::SegmentType::Raw;
    options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 4096;
    options.meta_size = shmfx::SHMFX_CACHE_LINE_BYTES;

    auto invalid = shmfx::ShmManager::attach("/bad.name", shmfx::AttachMode::ReadOnly);
    assert(!invalid);
    assert(invalid.error() == shmfx::ErrorCode::InvalidName);

    auto created = shmfx::ShmManager::create(options);
    assert(created);
    assert(created.value().valid());
    assert(created.value().name() == name);
    assert(created.value().metadata().size() == shmfx::SHMFX_CACHE_LINE_BYTES);
    assert(created.value().payload().size() == 4096);
    assert(created.value().header().ref_count == 1);

    {
        auto rw = shmfx::ShmManager::attach(name, shmfx::AttachMode::ReadWrite);
        assert(rw);
        assert(created.value().header().ref_count == 2);
    }
    assert(created.value().header().ref_count == 1);

    {
        auto ro = shmfx::ShmManager::attach(name, shmfx::AttachMode::ReadOnly);
        assert(ro);
        assert(ro.value().mode() == shmfx::AttachMode::ReadOnly);
        assert(created.value().header().ref_count == 1);
    }

    const auto entries = shmfx::Registry::instance().list("/shmfx.app.phase3_");
    bool found = false;
    for (const auto& entry : entries) {
        if (std::string(entry.name) == name) {
            found = true;
            assert(entry.total_size == options.total_size);
            assert(entry.segment_type == static_cast<std::uint32_t>(shmfx::SegmentType::Raw));
        }
    }
    assert(found);

    auto destroyed = shmfx::ShmManager::destroy(name);
    assert(destroyed);
    assert(shmfx::Registry::instance().list(name).empty());

    std::puts("phase3_manager_registry_test: ok");
    return 0;
}
