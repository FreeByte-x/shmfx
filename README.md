# shmfx

`shmfx` is a small Linux shared-memory framework built on POSIX `shm_open` and `mmap`.
It provides a fixed segment header, discovery registry, lifecycle recovery, optional immutable-header HMAC, robust control-plane mutexes, and a generic MPSC record ring.

The distributed logging package under `apps/logging/` is a reference app. Core `libshmfx` does not depend on logging.

## License

This project is licensed under the MIT License. See [LICENSE](/apps/source/sharemem/LICENSE).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Usage guide: [docs/04_USAGE.md](docs/04_USAGE.md).

Useful options:

```bash
cmake -S . -B build -DSHMFX_BUILD_TESTS=ON -DSHMFX_BUILD_EXAMPLES=ON -DSHMFX_BUILD_LOGGING=ON
```

## Core Example

```cpp
#include "shmfx/shm_manager.h"

shmfx::CreateOptions options;
options.name = "/shmfx.app.demo";
options.type = shmfx::SegmentType::Raw;
options.total_size = shmfx::SHMFX_HEADER_SIZE + shmfx::SHMFX_CACHE_LINE_BYTES + 4096;

auto handle = shmfx::ShmManager::create(options);
if (!handle) {
    return 1;
}
```

Runnable examples:

- `raw_segment_example`
- `ring_mpsc_example`
- `logging_demo_producer`
- `logging_demo_multi_producer`

## Stress and Benchmark

Phase 10 adds integration/stress tests to CTest:

- `phase10_ipc_logging_test`: forked producer and parent `LogCenter` consumer over real POSIX shared memory.
- `phase10_mpsc_stress_test`: multi-threaded MPSC ordering and drain stress.

The benchmark target is built but not registered as a test because timing is machine-dependent:

```bash
cmake --build build --target ring_benchmark
./build/ring_benchmark 4 100000
```

## Design Notes

- Names must use `/shmfx.<namespace>.<name>`.
- Default user namespaces are `log`, `tlm`, `kv`, `app`, and `sys`.
- `SegmentType::RecordRing` stores generic records as `[u32 len][u16 type][u16 flags][u64 user][payload]`.
- Producers never block on a slow consumer; full rings return `ErrorCode::RingFull` and increment `lost_count`.
- `LogCenter` uses adaptive polling in v0.1: short spin, yield, then a 200 us sleep when idle.

## Limitations

- Target platform is Linux/glibc. The ABI stores `pthread_mutex_t` in shared memory and uses compile-time static asserts for the supported layout. Do not treat v0.1 as portable to musl/Alpine, 32-bit Linux, or Windows.
- Optional HMAC-SHA256 covers only immutable header bytes `[0, 128)`. Mutable runtime state such as owner PID, refcount, lifecycle state, heartbeat, and robust mutex bytes is not MACed. This detects tamper/corruption of layout and identity fields, but it is not a full defense against a same-UID process that can write the shared-memory object.
- Read-only consumers can keep a valid mapping after `shm_unlink`. Logging `LogCenter` avoids the common stale-read path by attaching read/write, checking `state == DEAD`, draining once more, and detaching. Other consumers should implement the same self-validation policy.
- Adaptive polling keeps v0.1 daemonless, but idle CPU and tail latency are bounded by the polling strategy. Eventfd or broker-based wakeups are deferred until benchmarks justify the extra control channel.

## Repository Layout

- `include/shmfx/`: public core headers.
- `src/`: core implementation.
- `apps/logging/`: reference logging producer and consumer.
- `examples/`: core examples.
- `tests/`: unit tests wired by CMake.
- `docs/`: design and issue-tracking documents.
