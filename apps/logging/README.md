# shmfx Logging Reference App

The logging package demonstrates how an app can use core `shmfx` without adding app-specific behavior to `libshmfx`.

## Producer

```cpp
#include "shmfx_logging/logger.h"

auto logger = shmfx_logging::Logger::init("appauth");
if (logger) {
    logger.value().info("user=%s login", "alice");
}
```

Each producer process creates or attaches one ring named like `/shmfx.log.<service>_<pid>`.
Multiple producer threads push into the same MPSC ring.

## Consumer

```cpp
#include "shmfx_logging/log_center.h"

shmfx_logging::LogCenter center;
center.set_sink([](const shmfx_logging::LogRecord& record) {
    // write record.payload to a file, stdout, or a structured sink
});
center.poll_once();
```

`LogCenter` discovers `/shmfx.log.` registry entries, attaches rings with `AttachMode::ReadWrite`, drains batches, and detaches rings that reach `SegmentState::Dead`.

## Build

From the repository root:

```bash
cmake -S . -B build -DSHMFX_BUILD_LOGGING=ON
cmake --build build
```

Example targets:

- `logging_demo_producer`
- `logging_demo_multi_producer`

## v0.1 Limits

- There is no long-running file writer daemon yet. The current `LogCenter` exposes a sink callback; production file rotation belongs to the next app hardening pass.
- Wakeups use adaptive polling, not eventfd.
- Log payload formatting uses `vsnprintf` into a 1 KiB TLS buffer before pushing to the ring.
