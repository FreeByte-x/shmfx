# 05 — Reference App: Distributed Logging

> App/package tham chiếu dùng `libshmfx`. Tài liệu này không định nghĩa API core của framework.

## 1. Boundary

- Code nằm dưới `apps/logging/`.
- Public API dùng namespace riêng `shmfx_logging` hoặc `shmfx::logging`, không đặt `Logger` / `LogCenter` vào core `include/shmfx/`.
- App chỉ phụ thuộc một chiều vào `libshmfx`: `ShmManager`, `Registry`, `MpscRing`, lifecycle/security primitives.
- `libshmfx` không include header logging, không biết file rotation, formatter, log level, hoặc log center.

## 2. Architecture

```
  app_auth          app_api           app_worker
   │ Logger          │ Logger          │ Logger
   ▼                 ▼                 ▼
  /shmfx.log.appauth_1234  /shmfx.log.appapi_2234  /shmfx.log.worker_3234
   │                  │                  │
   └────────┬─────────┴─────────┬────────┘
            ▼                   ▼
                  log_center (1 process)
                  ├── adaptive polling over attached rings
                  ├── drain → format → write
                  └── rotate (size 256 MB | time 1h)
                          │
                          ▼
                  /var/log/app/<service>.log[.YYYYMMDD-HH]
```

- Mỗi producer process tạo 1 generic `RECORD_RING` riêng, tên mặc định `<service>_<pid>` để tránh collision khi cùng service chạy nhiều process.
- Nhiều thread trong process cùng push vào ring MPSC đó.
- `log_center` watch prefix `/shmfx.log.` → auto-discover ring mới qua `Registry`.

## 3. Record Semantics

Logging dùng record ring generic của framework:

```
[u32 len][u16 type][u16 flags][u64 ts_or_user][payload]
```

Mapping của logging app:

- `type`: log level `0..15` (`TRACE..FATAL`), upper bits reserved.
- `flags`: bit 0 = `TRUNCATED`, bit 1 = `BINARY`.
- `ts_or_user`: `CLOCK_MONOTONIC` timestamp ns.
- `payload`: formatted text hoặc binary structured log do `Logger::Formatter` tạo.

## 4. Producer Flow

1. `Logger::init("appauth")` tạo/attach `/shmfx.log.appauth_<pid>` qua `ShmManager`.
2. `LOG_INFO("user=%s login", user)` hot path:
   - format vào TLS buffer 1KB (`vsnprintf` mặc định, hoặc `Logger::Formatter` callback),
   - build 16B record header,
   - `MpscRing::try_push(header, payload)`.
3. Nếu ring full hoặc CAS contention vượt retry budget: increment `lost_count`, return, không block.
4. Không wake consumer trong v0.1; `log_center` dùng adaptive polling.

## 5. Consumer Flow

```c++
LogCenter c;
c.watch("/shmfx.log.");
c.run();
```

- Discovery thread: mỗi `DISCOVERY_PERIOD_MS` gọi `Registry::list("/shmfx.log.")`; entry mới thì attach ring.
- Main loop: drain từng ring theo batch, format text, ghi vào `FileWriter`.
- Ring owner dead: drain phần còn lại, detach theo lifecycle policy của framework.
- File writer: append-only, `fsync` mỗi `LOG_FSYNC_BATCH` record hoặc khi rotate.

## 6. Log Rotation

Trigger:

- Size: file ≥ `rotate_size_bytes` (default 256 MB).
- Time: cross sang giờ mới và file ≥ 1 MB.
- Signal: SIGHUP → rotate ngay.

Tên file rotate: `<service>.log.YYYYMMDD-HHMMSS`.

## 7. App Tunables

| Const | Default | Mô tả |
|-------|---------|------|
| `DISCOVERY_PERIOD_MS` | 1000 | Chu kỳ scan registry prefix `/shmfx.log.`. |
| `LOGCENTER_IDLE_SLEEP_US` | 200 | Adaptive polling sleep khi không drain được record nào. |
| `LOG_ROTATE_SIZE` | 256 MiB | File size threshold. |
| `LOG_FSYNC_BATCH` | 4096 | fsync mỗi N record. |

