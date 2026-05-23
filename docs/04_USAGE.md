# 04 — Hướng dẫn sử dụng `shmfx`

Tài liệu này tập trung vào cách dùng API hiện tại của `libshmfx`. Thiết kế chi tiết nằm ở [`01_DESIGN.md`](01_DESIGN.md); logging app tham chiếu nằm ở [`03_LOGGING_APP.md`](03_LOGGING_APP.md).

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Bật đầy đủ tests, examples và logging package:

```bash
cmake -S . -B build -DSHMFX_BUILD_TESTS=ON -DSHMFX_BUILD_EXAMPLES=ON -DSHMFX_BUILD_LOGGING=ON
cmake --build build
```

Các binary hữu ích sau build:

- `./build/raw_segment_example`
- `./build/ring_mpsc_example`
- `./build/logging_demo_producer`
- `./build/logging_demo_multi_producer`
- `./build/ring_benchmark 4 100000`

## 2. Tạo raw shared-memory segment

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

Ghi nhớ:

- Tên segment phải theo dạng `/shmfx.<namespace>.<name>`.
- Namespace mặc định gồm `log`, `tlm`, `kv`, `app`, `sys`.
- `ShmHandle` là RAII move-only; khi handle hủy, mapping được detach và `ref_count` giảm nếu attach RW.
- Dùng `AttachMode::ReadOnly` cho inspect/reader không cần sửa state; dùng `ReadWrite` nếu consumer cần tham gia lifecycle/co-owner.

## 3. Dùng MPSC record ring

Record ring phù hợp cho N producer thread và 1 consumer. Core không hiểu semantic app; `type`, `flags`, `user` là app-defined.

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
        // scratch[0..popped.copied_size) chứa payload.
    }

    [[maybe_unused]] auto cleanup = shmfx::ShmManager::destroy(options.name);
    return 0;
}
```

Khi ring đầy, `try_push()` trả lỗi và tăng `lost_count`; producer không block. Nếu payload dài hơn `max_payload`, core cắt payload và set bit `RECORD_FLAG_TRUNCATED`.

## 4. Attach vào ring đã có

Producer thường `create()` và `MpscRing::initialize()`. Consumer process khác `attach()` rồi `MpscRing::bind()`:

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

Với consumer dài hạn, ưu tiên `ReadWrite` để `ref_count` phản ánh mapping đang sống và để consumer có thể xử lý lifecycle state khi owner chết. Tool chỉ đọc header/registry có thể dùng `ReadOnly`.

## 5. Registry và janitor

`ShmManager::create()` tự publish segment vào registry. Có thể list theo prefix:

```cpp
#include "shmfx/shm_registry.h"

auto entries = shmfx::Registry::instance().list("/shmfx.app.");
for (const auto& entry : entries) {
    // entry.name chứa tên POSIX shm object.
}

shmfx::Registry::instance().gc();
```

`gc()` chạy một lượt janitor nhẹ: xóa registry entry stale, đánh dấu segment owner chết sang draining/dead, và unlink zombie khi không còn process map object.

## 6. Robust mutex

`ShmMutexGuard` dùng cho control plane, không dùng cho hot path ring. Khi owner trước chết trong critical section, caller phải recover state trước khi đánh dấu consistent:

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

Nếu không thể recover state, không gọi `mark_consistent()`; mutex sẽ báo unrecoverable cho waiter sau.

## 7. Bảo mật và vận hành

- Permission mặc định là `0600`; có thể set `CreateOptions::perm` nếu cần group access.
- HMAC optional qua flag `shmfx::HmacEnabled`; key đọc từ `SHMFX_HMAC_KEY` hoặc `/etc/shmfx/hmac.key`.
- HMAC chỉ cover immutable header, không cover mutable runtime state như `ref_count`, `state`, heartbeat.
- Target hiện tại là Linux/glibc; không coi v0.1 là portable sang musl/Alpine, 32-bit Linux hoặc Windows.

## 8. Logging reference app

Logging nằm ngoài core:

```cpp
#include "shmfx_logging/logger.h"

auto logger = shmfx_logging::Logger::init("appauth");
if (!logger) {
    return 1;
}

logger.value().info("user=%s login", "alice");
```

`LogCenter` discover các ring `/shmfx.log.*`, attach RW, drain batch, và detach khi segment dead. Xem thêm [`apps/logging/README.md`](../apps/logging/README.md).
