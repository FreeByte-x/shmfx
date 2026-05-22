#include "shmfx/shm_manager.h"

#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    const std::string name = "/shmfx.app.raw_demo_" + std::to_string(::getpid());
    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(name);

    shmfx::CreateOptions options;
    options.name = name;
    options.type = shmfx::SegmentType::Raw;
    options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 4096;
    options.meta_size = shmfx::SHMFX_CACHE_LINE_BYTES;

    auto created = shmfx::ShmManager::create(options);
    if (!created) {
        std::cerr << "create failed: " << shmfx::to_string(created.error()) << "\n";
        return 1;
    }

    constexpr char message[] = "hello from shmfx raw segment";
    std::memcpy(created.value().payload().data(), message, sizeof(message));

    auto attached = shmfx::ShmManager::attach(name, shmfx::AttachMode::ReadOnly);
    if (!attached) {
        std::cerr << "attach failed: " << shmfx::to_string(attached.error()) << "\n";
        return 1;
    }

    std::cout << reinterpret_cast<const char*>(attached.value().payload().data()) << "\n";
    [[maybe_unused]] auto destroyed = shmfx::ShmManager::destroy(name);
    return 0;
}
