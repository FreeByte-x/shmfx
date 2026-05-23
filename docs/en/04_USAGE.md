# 04 - `shmfx` Usage Guide

This guide focuses on the current `libshmfx` API. Core design details are in [`01_DESIGN.md`](01_DESIGN.md); the logging reference app is in [`03_LOGGING_APP.md`](03_LOGGING_APP.md).

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Build tests, examples, and the logging package:

```bash
cmake -S . -B build -DSHMFX_BUILD_TESTS=ON -DSHMFX_BUILD_EXAMPLES=ON -DSHMFX_BUILD_LOGGING=ON
cmake --build build
```

Useful binaries:

- `./build/raw_segment_example`
- `./build/ring_mpsc_example`
- `./build/logging_demo_producer`
- `./build/logging_demo_multi_producer`
- `./build/ring_benchmark 4 100000`

## 2. Create a Raw Shared-Memory Segment

```cpp
#include "shmfx/shm_manager.h"

#include <cstring>
#include <string>
#include <unistd.h>

int main() {
    const std::string name = "/shmfx.app.demo_" + std::to_string(::getpid());

    shmfx::CreateOptions options;
    options.name = name;
    options.type = shmfx::SegmentType::Raw;
    options.meta_size = shmfx::SHMFX_CACHE_LINE_BYTES;
    options.total_size = shmfx::SHMFX_HEADER_SIZE + options.meta_size + 4096;

    auto created = shmfx::ShmManager::create(options);
    if (!created) {
        return 1;
    }

    const char msg[] = "hello shmfx";
    std::memcpy(created.value().payload().data(), msg, sizeof(msg));

    auto reader = shmfx::ShmManager::attach(name, shmfx::AttachMode::ReadOnly);
    if (!reader) {
        return 1;
    }

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(name);
    return 0;
}
```

Notes:

- Segment names must use `/shmfx.<namespace>.<name>`.
- Default namespaces are `log`, `tlm`, `kv`, `app`, and `sys`.
- `ShmHandle` is move-only RAII.
- Use `ReadOnly` for inspection; use `ReadWrite` when a consumer participates in lifecycle/co-owner behavior.

## 3. Use an MPSC Record Ring

The ring supports many producer threads and one consumer. The core does not interpret app fields.

```cpp
#include "shmfx/shm_manager.h"
#include "shmfx/shm_ring.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>

int main() {
    constexpr std::uint32_t record_max = 1024;
    constexpr std::uint32_t max_payload = 256;
    const auto stride = shmfx::MpscRing::record_stride_for(max_payload);
    const auto payload_size = shmfx::MpscRing::payload_size_for(record_max, stride);

    shmfx::CreateOptions options;
    options.name = "/shmfx.app.ring_demo";
    options.type = shmfx::SegmentType::RecordRing;
    options.meta_size = sizeof(shmfx::RecordRingMeta);
    options.total_size = shmfx::align_up(shmfx::SHMFX_HEADER_SIZE + options.meta_size,
                                         shmfx::SHMFX_CACHE_LINE_BYTES) +
                         payload_size;
    options.flags = shmfx::LockfreeRing;

    auto segment = shmfx::ShmManager::create(options);
    if (!segment) {
        return 1;
    }

    auto& meta = *reinterpret_cast<shmfx::RecordRingMeta*>(segment.value().metadata().data());
    auto ring = shmfx::MpscRing::initialize(meta, segment.value().payload(), record_max, max_payload);
    if (!ring) {
        return 1;
    }

    const std::string text = "event payload";
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());

    auto pushed = ring.value().try_push(/*type=*/1, /*flags=*/0, /*user=*/42, bytes);
    if (!pushed) {
        return 1;
    }

    std::array<std::byte, max_payload> scratch{};
    shmfx::PoppedRecord popped{};
    if (ring.value().try_pop(scratch, popped)) {
        // scratch[0..popped.copied_size) contains the payload.
    }

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.name);
    return 0;
}
```

When the ring is full, `try_push()` returns an error and increments `lost_count`. If the payload exceeds `max_payload`, the core truncates it and sets `RECORD_FLAG_TRUNCATED`.

## 4. Attach to an Existing Ring

Producer processes normally create and initialize a ring. Consumers attach and bind:

```cpp
auto segment = shmfx::ShmManager::attach("/shmfx.app.ring_demo", shmfx::AttachMode::ReadWrite);
if (!segment) {
    return 1;
}

auto& meta = *reinterpret_cast<shmfx::RecordRingMeta*>(segment.value().metadata().data());
auto ring = shmfx::MpscRing::bind(meta, segment.value().payload());
if (!ring) {
    return 1;
}
```

Long-lived consumers should usually attach RW so `ref_count` reflects live mappings and the consumer can participate in lifecycle handling. Header-only inspection tools can attach RO.

## 5. Registry and Janitor

`ShmManager::create()` publishes segments to the registry. List by prefix:

```cpp
#include "shmfx/shm_registry.h"

auto entries = shmfx::Registry::instance().list("/shmfx.app.");
for (const auto& entry : entries) {
    // entry.name contains the POSIX shm object name.
}

shmfx::Registry::instance().gc();
```

`gc()` runs a cheap janitor pass: remove stale registry entries, mark dead-owner segments draining/dead, and unlink zombies only when no process maps them.

## 6. Robust Mutex

`ShmMutexGuard` is for the control plane, not the ring hot path. If a prior owner died while holding the mutex, recover protected state before marking the mutex consistent:

```cpp
shmfx::ShmMutexGuard guard(header.control_mutex);
if (!guard.locked()) {
    return guard.error();
}

if (guard.prior_owner_dead()) {
    recover_state_protected_by_mutex();
    guard.mark_consistent();
}
```

If state cannot be recovered, do not call `mark_consistent()`.

## 7. Security and Operations

- Default permission is `0600`; set `CreateOptions::perm` for group access.
- Optional HMAC uses `shmfx::HmacEnabled`.
- HMAC keys come from `SHMFX_HMAC_KEY` or `/etc/shmfx/hmac.key`.
- HMAC covers only immutable header bytes, not mutable runtime state.
- v0.1 targets Linux/glibc.

## 8. Logging Reference App

```cpp
#include "shmfx_logging/logger.h"

auto logger = shmfx_logging::Logger::init("appauth");
if (!logger) {
    return 1;
}

logger.value().info("user=%s login", "alice");
```

`LogCenter` discovers `/shmfx.log.*` rings, attaches RW, drains batches, and detaches when segments become dead. See [`apps/logging/README.md`](../../apps/logging/README.md).
