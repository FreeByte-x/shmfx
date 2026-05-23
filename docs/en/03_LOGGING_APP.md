# 03 - Reference App: Distributed Logging

> This package demonstrates how an app can use `libshmfx`. It does not define core framework APIs.

## 1. Boundary

- Code lives under `apps/logging/`.
- Public API uses `shmfx_logging` or `shmfx::logging`, not core `include/shmfx/`.
- The app depends one-way on `libshmfx`: `ShmManager`, `Registry`, `MpscRing`, lifecycle, and security primitives.
- `libshmfx` must not include logging headers or know about log levels, formatting, file rotation, or log centers.

## 2. Architecture

```text
app_auth          app_api           app_worker
  Logger           Logger             Logger
    |                |                  |
    v                v                  v
/shmfx.log.appauth_1234  /shmfx.log.appapi_2234  /shmfx.log.worker_3234
    \___________________________  ___________________________/
                                \/
                         log_center process
                         - discover rings
                         - adaptive poll
                         - drain, format, write
                         - rotate files
```

Each producer process creates one generic `RECORD_RING` named by service and PID. Multiple producer threads in the same process push to that MPSC ring. `log_center` watches `/shmfx.log.` through the registry and attaches new rings.

## 3. Record Semantics

The logging app uses the generic record format:

```text
[u32 len][u16 type][u16 flags][u64 ts_or_user][payload]
```

Logging mapping:

- `type`: log level `TRACE..FATAL`.
- `flags`: `TRUNCATED` and `BINARY`.
- `ts_or_user`: monotonic timestamp in nanoseconds.
- `payload`: formatted text or binary structured data.

## 4. Producer Flow

1. `Logger::init("appauth")` creates/attaches `/shmfx.log.appauth_<pid>`.
2. Hot-path logging formats into a TLS buffer.
3. The logger builds the record header and calls `MpscRing::try_push`.
4. If the ring is full or contended, `lost_count` increments and the call returns without blocking.
5. v0.1 does not wake the consumer; `LogCenter` uses adaptive polling.

## 5. Consumer Flow

```cpp
shmfx_logging::LogCenter center;
center.watch("/shmfx.log.");
center.run();
```

- Discovery periodically calls `Registry::list("/shmfx.log.")`.
- The main loop drains each attached ring in batches.
- Dead-owner rings are drained one last time, then detached.
- File writing is append-only with configurable fsync and rotation.

## 6. Rotation

Rotation triggers:

- file size reaches `rotate_size_bytes` (default 256 MiB),
- time crosses an hourly boundary and the file is at least 1 MiB,
- SIGHUP requests immediate rotation.

Rotated file name:

```text
<service>.log.YYYYMMDD-HHMMSS
```

## 7. Tunables

| Name | Default | Description |
|------|---------|-------------|
| `DISCOVERY_PERIOD_MS` | 1000 | Registry scan period for `/shmfx.log.`. |
| `LOGCENTER_IDLE_SLEEP_US` | 200 | Sleep after idle adaptive polling. |
| `LOG_ROTATE_SIZE` | 256 MiB | File size rotation threshold. |
| `LOG_FSYNC_BATCH` | 4096 | fsync every N records. |
