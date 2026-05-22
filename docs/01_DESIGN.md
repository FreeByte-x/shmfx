# 01 — Design Document: SharedMemory Framework Core (`shmfx`)

> Phiên bản: 0.5 (Phase 1, tách core framework khỏi reference logging app)
> Tuân thủ các quyết định lock-in tại `PLAN.md §4`. Mọi thay đổi với §4 phải cập nhật ở đó trước, không sửa trực tiếp tài liệu này.

**Changelog 0.4 → 0.5**:

- **Tách boundary core/app**: `libshmfx` là framework shared-memory chung; Distributed Logging chuyển sang tài liệu riêng [`05_LOGGING_APP.md`](05_LOGGING_APP.md) và repo path `apps/logging/`.
- **Ring trong core là generic `RECORD_RING` / `MpscRing`**, không phải primitive chỉ dành cho logging. Logging chỉ map `type/flags/timestamp/payload` theo semantic của app.
- **Core API không expose `Logger` / `LogCenter`**; các class này thuộc package logging và không được tạo dependency ngược vào `include/shmfx/`.

**Changelog 0.3 → 0.4** (OI-04):

- **Record ring v0.1 chuyển từ SPSC-only sang MPSC bắt buộc**: nhiều thread trong cùng producer process push vào một ring chung; consumer vẫn single-consumer.
- **Bỏ TLS ring per-thread khỏi path chính**: TLS buffer vẫn dùng cho format log, nhưng không tạo một shm segment/ring cho mỗi thread. Điều này giữ registry segment count scale theo số process thay vì số thread.
- **Phase 6 phải implement `MpscRing` production-ready**; SPSC chỉ còn là optional specialization/test helper. SPMC broadcast telemetry vẫn future.

**Changelog 0.2 → 0.3** (theo `docs/03_DESIGN_REVIEW.md`):

- **`std::atomic<T>` member → `std::atomic_ref<T>` trên storage POD** (review §1.1 + §2.10): mọi field đồng bộ inter-process giờ lưu là `uint32_t` / `uint64_t` *plain* trong struct; truy cập đồng bộ qua `std::atomic_ref<T>(field)` (C++20). Hệ quả:
  - Struct trivially copyable → `RegistryEntry snap = slot;` hợp lệ.
  - Loại bỏ UB của `std::atomic<T>` member trên shared memory (C++ standard không guarantee inter-process semantics cho `std::atomic`).
  - Áp dụng cho: `ShmHeader` mutable fields, `RegistryEntry::seq_storage`, `RegistryPayload::count`, `RecordRingMeta::lost_count`, `RingCursor::pos`.
- **`creator_start_time` + `owner_start_time`: u32 → u64** (review §1.2): chống overflow ở uptime > ~497 ngày (CLK_TCK=100). Kéo theo re-layout header:
  - **Immutable block**: drop field `header_size` (luôn = 256; layout contract qua `version_major`). Tiết kiệm 4 byte cho u64 `creator_start_time`. `name[64]` giữ offset 64.
  - **Mutable block** mở rộng 32 → 40 byte (thêm 4 byte pad cho u64 alignment + 4 byte u64 extra). `control_mutex` shift offset 192 → 200.
  - **Reserved padding**: 24 → 16 byte.
- **§2.3 perf targets viết lại thành bucket rõ ràng** (review §3.5): raw ring push / framework wrapper / consumer drain; app end-to-end metric đặt ở app design riêng.
- **§5.1 regex thắt nhẹ** để max name guarantee fit `name[64]`: namespace `{1,14}` (was 15), name `{0,38}` (was 39). Max length: 7 + 15 + 1 + 39 = 62 chars + null = 63 byte ≤ 64.
- **Phần trade-off còn open** (review §2.1 HMAC mutable, §2.6 polling, §2.8 mutex API, §3.x risks): **chưa apply hết** — chờ user xác nhận cho từng case. Review §2.5 đã resolve ở v0.4: dùng MPSC ngay.

**Changelog 0.1 → 0.2** (theo `docs/02_DESIGN_REVIEW.md`):

- **C++20** thay vì C++17 (đã sync vào `PLAN.md §4`). Cho phép `std::span`, `<concepts>`, `<bit>`.
- **Header tách 3 vùng vật lý**: immutable (covered bởi HMAC) | mutable runtime state (atomic, ngoài HMAC) | control mutex. Bảng §4.1 viết lại; HMAC không invalid mỗi heartbeat nữa.
- **`creator_start_time` + `owner_start_time`** (đọc từ `/proc/<pid>/stat:starttime`) để chống PID reuse trong owner-liveness check.
- **Registry seqlock per-slot** + capacity compile-time `MAX_REGISTRY_ENTRIES = 1024` (sửa lỗi `slots[capacity]` runtime-array).
- **`ref_count` chỉ bump khi attach RW**; janitor dùng RO attach (`O_RDONLY` + `PROT_READ`) để inspect mà không đụng refcount.
- **Record format reorder**: `[u32 len][u16 type][u16 flags][u64 ts_or_user][payload]` — header 16B, u64 field 8-byte aligned. Logging app maps `type` thành level và `ts_or_user` thành `ts_ns`.
- **Ring scope v0.1 = generic MPSC record ring**; SPMC deferred. Logging app dùng một ring chung mỗi producer process/service, nhiều thread push đồng thời qua CAS.
- **Wake-up v0.1 = adaptive polling** ở consumer tham chiếu; eventfd + UDS broker là future optimization (xem §12.4 Q5).
- **Security**: bỏ thao tác `umask` (process-global, không thread-safe). Dùng `shm_open(..., 0)` rồi `fchmod(fd, perm)`.
- **`pthread_mutex_consistent()`** chỉ gọi *sau khi* caller validate/recover state, không gọi ngay.
- **Naming regex** cho phép `_` trong cả namespace và name. `/shmfx.registry` được khai báo là special-case bootstrap không qua regex.
- **Performance targets** tách core buckets (raw ring push / framework wrapper / consumer drain); app end-to-end để trong app design riêng.
- **Static asserts** liệt kê ở §4.1.1 làm contract ABI Phase 2 phải pass.
- **`lost_count` trong `RecordRingMeta`** là `uint64_t` POD, truy cập qua `std::atomic_ref<uint64_t>`.

---

## 1. Mục tiêu & Phạm vi

### 1.1 Mục tiêu chính

`shmfx` là một framework C++20 cung cấp:

1. **Lớp abstraction trên POSIX shared memory** với header chuẩn 256B cho mọi segment → tooling và process khác có thể "đọc hiểu" cấu trúc mà không cần biết schema.
2. **Lifecycle quản trị tập trung**: tạo / attach / detach / huỷ segment an toàn cả khi process owner crash.
3. **Discovery**: registry segment cố định cho phép liệt kê và tra cứu các segment đang sống.
4. **Concurrency primitives** dùng được trên vùng shm: robust mutex (control plane) + generic lock-free MPSC record ring (data plane); SPMC broadcast để future.
5. **Bảo mật cơ bản**: POSIX permission + HMAC integrity + namespace isolation + DoS guards.
6. **App boundary rõ ràng**: các ứng dụng như Distributed Logging, telemetry, KV cache dùng framework qua API public, nhưng không nằm trong core `libshmfx`.

### 1.2 Không trong phạm vi (Non-goals)

- Cross-host transport (multicast, RDMA): để dành cho lớp trên.
- Cross-platform GA: code chính trên **Linux (POSIX `shm_open` + `mmap`)**. Windows port chỉ được phác thảo (§7.3) không implement.
- Serialization general-purpose (kiểu Cap'n Proto/FlatBuffers): chỉ định nghĩa layout per-segment-type, không cung cấp schema language.
- Capability-based security / mTLS: HMAC chỉ là integrity guard chống corruption + tamper sơ cấp; không thay thế kernel ACL.

### 1.3 Người dùng mục tiêu

- Đội backend / realtime cần IPC throughput cao trên cùng máy (logging, telemetry, market data, sensor fusion).
- Tooling / SRE cần inspect shm state khi debug.

---

## 2. Use cases & Yêu cầu

### 2.1 Use cases tiêu biểu

| UC | Mô tả | Tải tiêu biểu |
|----|------|----------------|
| UC1 | N process producer → 1 process consumer qua MPSC record ring | 8–64 producer, mỗi producer 10k–100k msg/s, msg ~256B |
| UC2 | 1 producer → N consumer (broadcast telemetry, future SPMC) | 1 prod, 4–16 consumer, tốc độ ms scale |
| UC3 | Shared KV cache giữa các process worker | đọc nhiều, ghi ít, control plane qua robust mutex |
| UC4 | Tooling `shmfxctl ls/inspect` đọc registry + header | low frequency |
| UC5 | Reference app Distributed Logging | Dùng UC1; thiết kế riêng ở `docs/05_LOGGING_APP.md` |

### 2.2 Functional requirements

- **F1**: API tạo segment với `(name, size, type, perm)` và nhận về handle map sẵn.
- **F2**: API attach segment theo tên, validate magic/version/HMAC trước khi map.
- **F3**: Reference counting tự động (RAII): refcount tăng khi attach, giảm khi destruct.
- **F4**: Heartbeat: owner cập nhật `heartbeat_last_ns` định kỳ; consumer / janitor có thể phát hiện owner chết.
- **F5**: Crash recovery: khi attach, scan registry và segment header để dọn segment "chết" theo policy.
- **F6**: Generic MPSC record ring với API `try_push(span<byte>)` / `try_pop(span<byte>&)`, không block.
- **F7**: Sync primitive API: robust process-shared mutex wrapper + recovery contract.
- **F8**: App packages có thể định nghĩa semantic riêng trên metadata/payload mà không sửa core ABI.

### 2.3 Non-functional requirements

**Core latency buckets** — định nghĩa **rõ ràng** để tránh ambiguity producer-side vs app end-to-end (review §3.5):

| # | Bucket | Định nghĩa chính xác | Target |
|---|--------|----------------------|--------|
| 1 | **Raw ring push** | `MpscRing::try_push(payload 64B)` enter → return, ring nóng trong L1, uncontended | p50 < 150 ns, p99 < 1 µs |
| 2 | **Framework push wrapper** | `RecordRingWriter::try_push(payload 64B)` gồm validate length + push | p50 < 250 ns, p99 < 2 µs |
| 3 | **Consumer drain** | generic consumer đọc 1 record từ ring → record visible trong user-buffer | p50 < 100 ns/record |

> App end-to-end metrics, ví dụ logging visible-in-file latency, nằm trong app design riêng và không được dùng để kéo API app vào core.

| Non-latency | Mục tiêu |
|----------|----------|
| Throughput per ring (MPSC, payload 64B, ring 16 MiB, 16 producer threads) | ≥ 5M msg/s aggregate |
| Crash safety | Không leak shm sau crash + restart loop ≤ 10s |
| Footprint | Framework lib < 200 KB stripped |
| Dependency | Chỉ stdlib + libpthread + libc; OpenSSL/`mbedtls` là **optional** (HMAC) |
| C++ standard | **C++20** |
| Build | CMake ≥ 3.16 |

---

## 3. Kiến trúc tổng thể

```
            ┌──────────────────────────────────────────────────────┐
            │                  Process A (owner)                   │
            │  ┌────────────┐    ┌──────────────────────────────┐  │
            │  │ App code   │───▶│  ShmManager (RAII handle)    │  │
            │  └────────────┘    └──────────────┬───────────────┘  │
            └─────────────────────────────────┬─┼──────────────────┘
                                              │ │ mmap
                                              ▼ ▼
            ┌──────────────────────────────────────────────────────┐
            │  /dev/shm/shmfx.<ns>.<name>    (POSIX shm object)    │
            │  ┌────────────────────────────────────────────────┐  │
            │  │ ShmHeader 256B (magic, ver, refcnt, hb, hmac)  │  │
            │  ├────────────────────────────────────────────────┤  │
            │  │ User metadata (segment-type specific)          │  │
            │  ├────────────────────────────────────────────────┤  │
            │  │ Payload: RAW / RECORD_RING / KV / app-defined  │  │
            │  └────────────────────────────────────────────────┘  │
            └────────────────────────────────────▲─────────────────┘
                                                 │ mmap
            ┌────────────────────────────────────┴─────────────────┐
            │             Process B (reader/tool/consumer)         │
            │  discover via Registry → attach RO/RW theo use case  │
            │  inspect header/metadata or consume record payload   │
            └──────────────────────────────────────────────────────┘

            ┌──────────────────────────────────────────────────────┐
            │  /dev/shm/shmfx.registry   (singleton discovery)     │
            │  ShmHeader 256B + array<RegistryEntry> + free list   │
            └──────────────────────────────────────────────────────┘
```

Ba lớp module (mỗi lớp ↔ một cặp file header/source, xem `PLAN.md §3`):

- **Layout layer**: `shm_header.h`, `shm_types.h` — struct + invariant.
- **Control layer**: `shm_manager`, `shm_registry`, `shm_lifecycle`, `shm_security`.
- **Data/sync layer**: `shm_ring`, `shm_sync`.

Reference apps nằm ngoài ba lớp này, ví dụ Distributed Logging ở `apps/logging/` và [`05_LOGGING_APP.md`](05_LOGGING_APP.md).

---

## 4. Memory Layout

### 4.1 Header chung — `struct ShmHeader` (cố định 256 bytes)

Header chia **3 vùng vật lý**:

1. **Immutable block** `[0, 128)` — set một lần ở `create()`, không bao giờ ghi lại. Là input của HMAC.
2. **HMAC field** `[128, 160)` — chữ ký HMAC-SHA256 trên bytes `[0, 128)`. 32 byte, zero nếu `HMAC_ENABLED` off.
3. **Mutable runtime state + mutex** `[160, 256)` — counter inter-process + robust mutex + padding. **Không** nằm trong HMAC để heartbeat / refcount đổi không invalid chữ ký.

Tất cả integer little-endian. **Storage là POD** (`uint32_t` / `uint64_t` plain); đồng bộ inter-process qua `std::atomic_ref<T>(field)` (C++20). Lý do: `std::atomic<T>` member không có ngữ nghĩa inter-process trong C++ standard, và còn làm struct non-copyable (delete copy ctor) → bug nghiêm trọng khi snapshot. Storage POD + `atomic_ref` giải quyết cả hai.

| Offset | Size | Field | Storage | Mô tả |
|--------|------|-------|---------|------|
| **— Immutable block `[0, 128)`, là input HMAC —** | | | | |
| 0   | 4  | `magic`               | `uint32_t`              | `0x53484D46` (= "SHMF" little-endian). |
| 4   | 2  | `version_major`       | `uint16_t`              | Breaking. Mismatch ⇒ refuse attach. Cũng là contract định nghĩa cả layout (kể cả header size = 256). |
| 6   | 2  | `version_minor`       | `uint16_t`              | Additive. Mismatch warn. |
| 8   | 4  | `segment_type`        | `uint32_t`              | `RAW=1`, `RECORD_RING=2`, `KV=3`, `REGISTRY=4`. |
| 12  | 4  | `flags`               | `uint32_t`              | `HMAC_ENABLED=1`, `ROBUST_MUTEX=2`, `LOCKFREE_RING=4`, `READONLY_PAYLOAD=8`. Immutable sau create. |
| 16  | 8  | `total_size`          | `uint64_t`              | Tổng size segment. |
| 24  | 8  | `payload_size`        | `uint64_t`              | Bytes payload. |
| 32  | 4  | `meta_offset`         | `uint32_t`              | Offset user-metadata (thường 256). |
| 36  | 4  | `meta_size`           | `uint32_t`              | Bytes user-metadata. |
| 40  | 4  | `payload_offset`      | `uint32_t`              | Offset payload (= `align64(meta_offset + meta_size)`). |
| 44  | 4  | `creator_pid`         | `uint32_t`              | PID process tạo (không đổi). |
| 48  | 8  | `created_at_ns`       | `uint64_t`              | `CLOCK_REALTIME` ns lúc create. |
| 56  | 8  | `creator_start_time`  | `uint64_t`              | `/proc/<pid>/stat:starttime` (clock ticks since boot, field 22). **u64** để không overflow ở uptime dài. |
| 64  | 64 | `name[64]`            | `char[64]`              | Bản sao tên `/shmfx.<ns>.<name>` (null-terminated). Max 63 chars + null. |
| **— HMAC field `[128, 160)` —** | | | | |
| 128 | 32 | `hmac[32]`            | `uint8_t[32]`           | HMAC-SHA256 over bytes `[0, 128)` khi `HMAC_ENABLED`. Zero nếu disabled. |
| **— Mutable runtime state `[160, 200)`, KHÔNG nằm trong HMAC —** | | | | |
| 160 | 4  | `owner_pid`           | `uint32_t` *(atomic_ref)* | PID owner hiện tại. Synchronization point cho ownership transfer (xem §6.3). |
| 164 | 4  | `_pad0`               | `uint32_t`              | Align `owner_start_time` đến 8-byte. |
| 168 | 8  | `owner_start_time`    | `uint64_t`              | start_time owner. Đọc *sau* khi `owner_pid` đã load với acquire (happens-before, không cần atomic_ref riêng). |
| 176 | 4  | `ref_count`           | `uint32_t` *(atomic_ref)* | Số handle đang attach **RW** (RO attach không bump). 0 + state≥DRAINING ⇒ candidate unlink. |
| 180 | 4  | `state`               | `uint32_t` *(atomic_ref)* | `INIT=0`, `ACTIVE=1`, `DRAINING=2`, `DEAD=3`. |
| 184 | 8  | `heartbeat_counter`   | `uint64_t` *(atomic_ref)* | Owner ++ mỗi tick. |
| 192 | 8  | `heartbeat_last_ns`   | `uint64_t` *(atomic_ref)* | `CLOCK_MONOTONIC` ns lần tick gần nhất. |
| **— Control mutex `[200, 240)` —** | | | | |
| 200 | ≤40| `control_mutex`       | `pthread_mutex_t`       | Robust + process-shared (xem §7.1). Trên glibc x86_64/aarch64 = 40 byte. |
| **— Padding `[240, 256)` —** | | | | |
| 240 | 16 | `_reserved[16]`       | `uint8_t[16]`           | Padding đến 256B, future use. |

> Lưu ý ABI: HMAC chỉ phụ thuộc 128 byte đầu tiên. Mutable state đổi mỗi 100ms không invalid chữ ký, nên verify HMAC chỉ thực hiện *một lần* lúc attach, không re-verify trong steady state.

**Ownership transfer protocol** (`owner_pid` + `owner_start_time`): writer hold `control_mutex`, ghi `owner_start_time` trước (relaxed) rồi ghi `owner_pid` với release. Reader load `owner_pid` với acquire — happens-before đảm bảo `owner_start_time` đã visible. Pseudocode:

```c++
// Writer (under control_mutex):
header.owner_start_time = new_start_time;                              // plain store
std::atomic_ref<uint32_t>(header.owner_pid).store(new_pid,
    std::memory_order_release);

// Reader (no lock):
uint32_t pid = std::atomic_ref<uint32_t>(header.owner_pid).load(
    std::memory_order_acquire);
uint64_t st  = header.owner_start_time;                                // plain load OK
```

#### 4.1.1 ABI lock-in & static assertions

Target ABI **Linux + glibc ≥ 2.31 + x86_64 hoặc aarch64**. Port sang ABI khác (musl, BSD, Windows) phải bump `version_major`.

Phase 2 phải pass các `static_assert` sau (đặt trong `shm_header.h`):

```c++
static_assert(sizeof(ShmHeader)                     == 256);
static_assert(alignof(ShmHeader)                    >= 8);
static_assert(offsetof(ShmHeader, hmac)             == 128);
static_assert(offsetof(ShmHeader, owner_pid)        == 160);
static_assert(offsetof(ShmHeader, owner_start_time) == 168);
static_assert(offsetof(ShmHeader, control_mutex)    == 200);
static_assert(sizeof(pthread_mutex_t)               <= 40,
              "Bump version_major nếu glibc thay đổi size pthread_mutex_t");
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<ShmHeader>);
static_assert(std::is_trivially_copyable_v<ShmHeader>);   // storage POD, không có atomic<T> member
```

**Ngữ nghĩa C++20**: framework dùng `std::atomic_ref<T>(storage)` thay vì `std::atomic<T>` member. Hệ quả:

- Struct trivially copyable → snapshot bằng plain copy hợp lệ (không UB).
- `is_always_lock_free` của `atomic_ref` thay thế `is_always_lock_free` của `atomic` — trên Linux x86_64/aarch64 cả hai true cho T ∈ {u32, u64}.
- Standard C++20 không bảo đảm tường minh inter-process semantics cho `atomic_ref` (cũng như `atomic`). Trên Linux+glibc với lock-free atomics: gcc/clang emit hardware instructions (LOCK CMPXCHG, LDXR/STXR) trực tiếp trên storage, không dùng table lock per-process. Behavior well-defined về mặt thực dụng. Nếu cần stricter, fallback sang `__atomic_*` builtins trên storage — semantic identical.

#### 4.1.2 Invariant kiểm sau `mmap`

- `magic == 0x53484D46`
- `version_major == SHMFX_VERSION_MAJOR` (v0.3 = 1) — version_major chứa contract layout & header size.
- `meta_offset >= 256 && meta_offset <= total_size`
- `payload_offset + payload_size <= total_size`
- `payload_offset >= meta_offset + meta_size`
- Nếu `flags & HMAC_ENABLED`: HMAC verify pass over `[0, 128)`.

Vi phạm bất kỳ ⇒ `attach()` fail với `ErrCorruptedHeader`, `munmap` ngay, không trả handle.

### 4.2 User metadata region

- Bắt đầu tại `meta_offset`, dài `meta_size` byte.
- Format do segment_type quy định. Ví dụ với `RECORD_RING`:

  ```c++
  struct RecordRingMeta {                    // tổng 64B, POD, trivially copyable
      uint32_t record_max;                   // số record tối đa
      uint32_t record_stride;                // size 1 slot (align 64)
      uint32_t producer_mode;                // SPSC=1 optional, MPSC=2 (v0.1 default)
      uint32_t consumer_mode;                // SC=1 (single consumer). SPMC=3 reserved/future.
      uint64_t lost_count;                   // truy cập qua std::atomic_ref<uint64_t>
      uint8_t  _rsv[40];
  };
  static_assert(sizeof(RecordRingMeta) == 64);
  static_assert(std::is_trivially_copyable_v<RecordRingMeta>);

  // Producer drop:
  // std::atomic_ref<uint64_t>(meta.lost_count).fetch_add(1, std::memory_order_relaxed);
  ```

- Metadata stride **align 64B** để tránh false-sharing với header.

### 4.3 Payload region

- Bắt đầu tại `payload_offset` (align 64).
- Nội dung do segment_type quy định:
  - `RAW`: free-form, user tự quản.
  - `RECORD_RING`: 2 cursor cache-line, `record_max` sequence number, rồi `record_max` slot record; mỗi record slot `record_stride` bytes (xem §7.2).
  - `KV`: hash table inline (không trong scope v0.1, để placeholder).
  - `REGISTRY`: array `RegistryEntry` + bitmap free slot (§5.2).

---

## 5. Naming & Discovery

### 5.1 Naming rule

Quy ước đặt tên POSIX shm object:

```
/shmfx.<namespace>.<name>
```

- `<namespace>`: 2–15 ký tự, `[a-z][a-z0-9_-]{1,14}`. Whitelist trong `shm_security.cpp`: `log`, `tlm`, `kv`, `app`, `sys` (mở rộng qua config).
- `<name>`: 1–39 ký tự, `[a-z][a-z0-9_-]{0,38}`.
- Tổng độ dài tối đa: `/shmfx.` (7) + ns (15) + `.` (1) + name (39) = **62 chars** + `\0` = 63 byte, fit vừa trong `ShmHeader::name[64]`.

Regex chính tắc (cho phép cả `_` và `-` ở cả hai phần):

```
^/shmfx\.[a-z][a-z0-9_-]{1,14}\.[a-z][a-z0-9_-]{0,38}$
```

`ShmManager::create()` reject ngay nếu tên không match.

**Special-case bootstrap names** (không qua regex, do framework tự quản):

| Name | Mục đích |
|------|----------|
| `/shmfx.registry` | Registry segment (xem §5.2). Chỉ được tạo bởi `Registry::open_or_create()`. |
| `/shmfx.registry.lock` | Reserved cho future leader-election. Phase 1 chưa dùng. |

Việc cho phép `/shmfx.registry` đi ngoài regex được code hoá tường minh trong `shm_security.cpp::is_valid_name()` (early-return true cho whitelist này).

### 5.2 Registry segment

- Tên cố định: `/shmfx.registry`.
- `segment_type = REGISTRY`.
- Bootstrap: process đầu tiên gọi `Registry::open_or_create()` sẽ `shm_open(O_CREAT|O_EXCL)`, ghi header + payload zeroed. Race lose (`EEXIST`) ⇒ retry attach.
- Capacity **compile-time fixed**: `constexpr uint32_t MAX_REGISTRY_ENTRIES = 1024`. Lý do: struct C++ không cho phép runtime-sized array làm member; muốn flex thì phải tính offset bằng tay. 1024 đủ cho mọi UC mục tiêu (mỗi entry 128B ⇒ 128 KB).

```c++
constexpr uint32_t MAX_REGISTRY_ENTRIES = 1024;
constexpr uint32_t REGISTRY_BITMAP_BYTES = MAX_REGISTRY_ENTRIES / 8;   // 128

struct RegistryEntry {                          // 128 byte, POD, trivially copyable
    uint32_t seq_storage;                       // seqlock counter; access qua atomic_ref<u32>
    uint32_t _pad0;
    char     name[64];
    uint64_t total_size;
    uint32_t segment_type;
    uint32_t owner_pid;
    uint64_t owner_start_time;                  // /proc/<pid>/stat field 22 (u64, không overflow)
    uint32_t flags;
    uint32_t _pad1;                             // align 8 cho created_at_ns
    uint64_t created_at_ns;
    uint64_t last_seen_ns;                      // janitor cập nhật (atomic_ref nếu cần)
    uint8_t  _reserved[8];
};
static_assert(sizeof(RegistryEntry) == 128);
static_assert(std::is_trivially_copyable_v<RegistryEntry>);

struct RegistryPayload {                        // 256 byte head + 128 KiB slots
    uint32_t count_storage;                     // số slot occupied (hint); atomic_ref<u32>
    uint32_t _pad0;
    uint8_t  bitmap[REGISTRY_BITMAP_BYTES];     // bit i = slot i occupied
    uint8_t  _align[120];                       // pad đến 256B
    RegistryEntry slots[MAX_REGISTRY_ENTRIES];
};
static_assert(sizeof(RegistryPayload) == 256 + 128 * MAX_REGISTRY_ENTRIES);
```

**Concurrency** — seqlock per-slot, dùng `std::atomic_ref<uint32_t>` lên `seq_storage` (storage POD, không có `atomic<T>` member nên struct copyable):

- **Writer** (`register`/`unregister`): hold `ShmHeader::control_mutex` (robust), tìm bit free trong bitmap, set bit, rồi update slot:

  ```c++
  RegistryEntry& slot = payload.slots[i];
  std::atomic_ref<uint32_t> seq(slot.seq_storage);

  uint32_t s = seq.load(std::memory_order_relaxed);
  seq.store(s + 1, std::memory_order_release);          // odd: writing
  // ... ghi tất cả field non-seq của slot ...
  std::atomic_thread_fence(std::memory_order_release);
  seq.store(s + 2, std::memory_order_release);          // even: stable
  ```

- **Reader** (`list`/janitor) — **không** lock; retry trên seqlock. Vì `RegistryEntry` trivially copyable, `RegistryEntry snap = slot;` *hợp lệ*:

  ```c++
  std::atomic_ref<uint32_t> seq(slot.seq_storage);
  for (int retry = 0; retry < 100; ++retry) {
      uint32_t s1 = seq.load(std::memory_order_acquire);
      if (s1 & 1u) { cpu_relax(); continue; }            // writer đang vào
      RegistryEntry snap = slot;                          // OK: POD copy
      std::atomic_thread_fence(std::memory_order_acquire);
      uint32_t s2 = seq.load(std::memory_order_acquire);
      if (s1 == s2) return snap;                          // consistent snapshot
      // writer đè giữa chừng → retry
  }
  // Bound retry: fallback acquire control_mutex để đảm bảo progress (anti-starvation).
  ```

- Operations: `register(entry)`, `unregister(name)`, `list(prefix)`, `gc()` (janitor sweep).

---

## 6. Lifecycle Management

### 6.1 State machine

```
              create()          attach()
   ┌────┐    ───────▶  ┌──────┐ ───────▶  ┌────────┐
   │ -  │              │ INIT │            │ ACTIVE │
   └────┘              └──┬───┘            └───┬────┘
                          │ ready                │ owner.shutdown()
                          ▼                       ▼
                       ACTIVE ◀───────────  ┌──────────┐
                                            │ DRAINING │
                                            └────┬─────┘
                                                 │ refcount == 0
                                                 ▼
                                              ┌──────┐
                                              │ DEAD │ → shm_unlink
                                              └──────┘
```

- `INIT`: header được ghi (magic, layout) nhưng chưa published vào registry.
- `ACTIVE`: registry đã có entry, consumer được phép attach + đọc/ghi.
- `DRAINING`: owner báo shutdown, không nhận record mới; consumer drain phần còn lại.
- `DEAD`: refcount = 0 và state = DRAINING ⇒ unlink (`shm_unlink`) + xoá entry registry.

Mọi transition đi qua `control_mutex` để serialize.

### 6.2 Reference counting

`ref_count` đếm **số handle RW** (read-write). Lý do tách RW/RO: janitor cần inspect segment mà không tự bump số đếm nó đang kiểm tra (review §3).

- `ShmManager::attach(name, AttachMode::ReadWrite)`:
  - `shm_open(name, O_RDWR)`, `mmap(PROT_READ|PROT_WRITE)`.
  - Sau khi validate header: `std::atomic_ref<uint32_t>(h.ref_count).fetch_add(1, memory_order_acq_rel)`.
- `ShmManager::attach(name, AttachMode::ReadOnly)`:
  - `shm_open(name, O_RDONLY)`, `mmap(PROT_READ)`.
  - **Không** đụng `ref_count`. Janitor + `shmfxctl inspect` dùng mode này.
- `~ShmHandle` (RW): `std::atomic_ref<uint32_t>(h.ref_count).fetch_sub(1, memory_order_acq_rel)` rồi `munmap`.
- Nếu sau sub == 0 **và** `state == DRAINING` **và** caller là owner ⇒ unlink + registry.unregister.

**Crash protection**: nếu RW handle crash trước khi destruct, ref_count rò 1 đơn vị. Janitor (§6.4) phát hiện qua heartbeat + `owner_start_time`, đặt `state = DRAINING`; khi consumer cuối cùng detach, nó là người thực hiện unlink. Không có "force reset refcount" — chỉ tin vào sự tiến triển của state machine.

Hạn chế đã biết: nếu nhiều RW handle cùng process crash đồng thời, ref_count rò nhiều đơn vị; segment có thể không bao giờ về 0. Mitigation: janitor có timeout `SEGMENT_ZOMBIE_NS` (default 1h) — sau ngần ấy thời gian `DRAINING` mà ref_count vẫn > 0 *và* không process nào hold handle (kiểm bằng `fuser /dev/shm/...` hoặc scan `/proc/*/maps`), force unlink. Triển khai chi tiết để Phase 4.

### 6.3 Heartbeat & owner liveness

- Owner spawn 1 thread `heartbeat_thread`:
  - Mỗi `HEARTBEAT_TICK_MS` (default 100ms):
    ```c++
    std::atomic_ref<uint64_t>(h.heartbeat_counter).fetch_add(1, std::memory_order_release);
    std::atomic_ref<uint64_t>(h.heartbeat_last_ns).store(monotonic_ns(), std::memory_order_release);
    ```
- Owner liveness check (gọi từ consumer/janitor):

  ```c++
  bool owner_is_dead(const ShmHeader& h, uint64_t now_mono_ns) {
      uint32_t pid = std::atomic_ref<uint32_t>(
          const_cast<uint32_t&>(h.owner_pid)).load(std::memory_order_acquire);
      uint64_t st  = h.owner_start_time;        // happens-after acquire load của pid

      if (pid == 0) return true;
      // (1) PID không còn tồn tại?
      if (::kill(pid, 0) == -1 && errno == ESRCH) return true;
      // (2) PID còn nhưng đã bị reuse → start_time khác. Fallback nếu /proc fail.
      uint64_t cur_st = 0;
      if (!read_proc_start_time(pid, &cur_st)) {
          // /proc không đọc được (sandboxed container?). KHÔNG mark dead chỉ vì đọc fail
          // → bỏ qua bước (2), nhảy thẳng (3).
      } else if (cur_st != st) {
          return true;
      }
      // (3) Process tồn tại + (đúng instance hoặc proc-unavailable) → check heartbeat staleness.
      uint64_t last = std::atomic_ref<uint64_t>(
          const_cast<uint64_t&>(h.heartbeat_last_ns)).load(std::memory_order_acquire);
      return (now_mono_ns - last) > HEARTBEAT_DEAD_NS;     // default 3s
  }
  ```

  Bước (2) là cốt lõi chống PID reuse. `read_proc_start_time()` đọc **field 22** của `/proc/<pid>/stat` (clock ticks since boot, kernel emit `unsigned long long` → fit `uint64_t`, không overflow). Trả `false` nếu `/proc` không khả dụng — đây là contract để fallback an toàn trong container sandbox. Future: `pidfd_open()` + `pidfd_send_signal()` khi minimum kernel ≥ 5.3 (§12.4 Q8).

- **Ownership transfer**: consumer hold `control_mutex` rồi store `owner_start_time` (plain) trước, store `owner_pid` (release) sau — pattern đã document ở cuối §4.1. RO consumer phải re-attach RW (bump ref_count) để có quyền ghi mutable block; điều này đồng nghĩa "co-owner" semantic, không phải pure consumer. Xem trade-off §3.2 của `03_DESIGN_REVIEW.md`.

### 6.4 Crash recovery — Janitor

Janitor có 2 hình thái:

1. **Inline janitor (default)**: mỗi lần `ShmManager::attach()` hoặc `Registry::list()` được gọi → chạy 1 lượt quét cheap (lazy).
2. **Daemon janitor (optional)**: binary `shmfxd` chạy timer mỗi 5s, full sweep.

Thuật toán sweep (lưu ý: dùng RO attach để inspect, *không* bump ref_count; mọi access mutable field qua `std::atomic_ref`):

```
for entry in registry.snapshot():                  # seqlock-read, §5.2
    rh = shm_open(entry.name, O_RDONLY)
    if rh fails with ENOENT:
        registry.unregister(entry.name); continue
    hdr = mmap(rh, PROT_READ, header-only)         # chỉ map 256B đầu
    if not validate(hdr):
        registry.unregister(entry.name); munmap; continue

    dead = owner_is_dead(hdr, now_mono_ns())       # §6.3
    rc   = atomic_ref<u32>(hdr.ref_count).load(acquire)

    if dead and rc == 0:
        # Có race: process khác đang attach RW giữa lúc ta đọc.
        # Serialize qua registry control_mutex.
        with registry.header.control_mutex:        # robust mutex
            rc2 = atomic_ref<u32>(hdr.ref_count).load(acquire)
            if rc2 == 0 and not still_alive(hdr.owner_pid, hdr.owner_start_time):
                # State machine: ACTIVE → DRAINING → DEAD
                atomic_ref<u32>(hdr.state).compare_exchange_strong(ACTIVE,   DRAINING)
                atomic_ref<u32>(hdr.state).compare_exchange_strong(DRAINING, DEAD)
                shm_unlink(entry.name)
                registry.unregister(entry.name)

    elif dead and rc > 0:
        atomic_ref<u32>(hdr.state).compare_exchange_strong(ACTIVE, DRAINING)
        registry.update_last_seen(entry.name, now_mono_ns)  # DRAINING start marker
        # KHÔNG gọi pthread_mutex_consistent() ở đây — chỉ caller thực sự cần
        # control_mutex và bắt EOWNERDEAD mới được consistent (xem §7.1).

    elif state == DRAINING and now_mono_ns - entry.last_seen_ns > SEGMENT_ZOMBIE_NS:
        munmap(hdr)  # tránh self-map làm /proc scan luôn thấy live mapping
        if not proc_maps_contains(entry.name):
            shm_unlink(entry.name)
            registry.unregister(entry.name)
```

Race cố tránh:

- Hai janitor cùng unlink → registry `control_mutex` quanh khối `unlink + unregister`.
- Janitor unlink trong khi process khác đang `attach()` giữa `shm_open` và `ref_count++`: race vẫn tồn tại trong khoảng ns. Mitigation: process attach phải re-validate `state != DEAD` *sau khi* bump ref_count; nếu DEAD, rollback (decrement + bail). Đây là contract của `ShmManager::attach`.

---

## 7. Concurrency Model

### 7.1 Robust pthread mutex (control plane)

- Init:
  - `pthread_mutexattr_setpshared(..., PTHREAD_PROCESS_SHARED)`
  - `pthread_mutexattr_setrobust(..., PTHREAD_MUTEX_ROBUST)`
  - `pthread_mutexattr_settype(..., PTHREAD_MUTEX_ERRORCHECK)` (debug) / `_NORMAL` (release).
- Khi owner chết khi đang hold, lock tiếp theo trả `EOWNERDEAD`. Caller phải **(1)** validate/recover state mà mutex bảo vệ, **(2)** rồi mới gọi `pthread_mutex_consistent()`, **(3)** sau đó mutex coi như được hold hợp lệ — caller `unlock` bình thường.

  Order quan trọng: nếu gọi `pthread_mutex_consistent()` *trước* khi recover, các waiter khác sẽ thấy mutex đã consistent và có thể grab → race với recovery đang dở.

  ```c++
  ShmMutexGuard guard(header.control_mutex);
  if (!guard.locked()) return guard.error();

  if (guard.prior_owner_dead()) {
      // 1. Kiểm tra & sửa state mutex bảo vệ (registry slot, lifecycle state...).
      recover_protected_state();
      // 2. Mark consistent — chỉ sau khi state đã ổn.
      guard.mark_consistent();
  }
  // 3. ... dùng critical section bình thường. Destructor unlock().
  ```

  Nếu caller không thể recover (state không xác định) thì *không* gọi `consistent()` — `unlock()` trên mutex EOWNERDEAD unrecovered sẽ propagate `ENOTRECOVERABLE` cho mọi waiter sau, đó là tín hiệu segment hỏng cần dispose.

- Dùng cho: registry slot updates, lifecycle transitions, ownership transfer. **Không** dùng cho hot path log producer.

### 7.2 Lock-free ring (data plane)

**Scope v0.1**: implement **generic MPSC record ring** (multi-producer / single-consumer). SPSC chỉ là optional specialization/test helper; **không** là path chính. SPMC broadcast telemetry defer sang phase sau.

Lý do chốt MPSC ngay:

- UC target gồm worker pool / async runtime 16+ thread, nhiều thread cùng publish record vào một channel shared-memory.
- Ring per-thread làm segment count scale theo thread: 16 thread × 100 process = 1600 ring, vượt `MAX_REGISTRY_ENTRIES = 1024` và làm consumer/tooling phải poll quá nhiều ring.
- MPSC giữ model "một channel/process/service = một record ring"; registry và polling scale theo channel thay vì thread.

Triển khai MPSC dùng bounded ring kiểu sequence-per-slot (Vyukov bounded queue rút gọn còn single consumer):

- `head`: cursor consumer duy nhất.
- `tail`: cursor claim slot của nhiều producer, update bằng CAS.
- Mỗi slot có `seq_storage` riêng để báo trạng thái free/ready, truy cập qua `std::atomic_ref<uint64_t>`.
- Slot `i` free cho cursor `pos` khi `seq == pos`; producer claim thành công bằng `CAS(tail, pos, pos+1)`, ghi record, rồi publish `seq = pos + 1` với release.
- Consumer đọc slot khi `seq == head + 1` với acquire; sau khi drain, mark free cho vòng sau bằng `seq = head + record_max`.

```c++
constexpr std::size_t CACHE_LINE_SIZE =
    std::hardware_destructive_interference_size;       // fallback 64 nếu toolchain không expose

struct alignas(CACHE_LINE_SIZE) RingCursor {           // POD, trivially copyable
    uint64_t pos_storage;                              // access qua atomic_ref<u64>
    uint8_t  _pad[CACHE_LINE_SIZE - sizeof(uint64_t)];
};
static_assert(sizeof(RingCursor) == CACHE_LINE_SIZE);
static_assert(std::is_trivially_copyable_v<RingCursor>);

struct RingSlotHeader {
    uint64_t seq_storage;                              // access qua atomic_ref<u64>
};
static_assert(sizeof(RingSlotHeader) == 8);

// Ring layout:
//   RingCursor head; RingCursor tail; RingSlotHeader slot_meta[record_max];
//   record bytes[record_max][record_stride];
```

Memory order:

- Producer claim: `tail.compare_exchange_weak(pos, pos + 1, acq_rel, relaxed)`.
- Producer publish: ghi payload hoàn tất → `slot.seq.store(pos + 1, release)`.
- Consumer ready check: `slot.seq.load(acquire) == head + 1` → đọc payload.
- Consumer free slot: `slot.seq.store(head + record_max, release)` rồi `head.store(head + 1, release)`.

Full/drop policy: nếu producer thấy `seq < tail_snapshot` sau khi load acquire, ring đang full hoặc consumer chưa free slot kịp; `try_push()` trả `ErrRingFull`, increment `lost_count`, không block. ABA: monotonic 64-bit cursor, không wrap trong vòng đời thực tế (5M msg/s × 2⁶⁴ ≈ hơn 100k năm).

**Record format** trong slot (header 16B, `ts_ns` align 8):

```
offset  0   4   6   8         16
        ┌───┬───┬───┬─────────┬─────────────────────┐
        │len│typ│flg│ ts/usr  │ payload[len]        │
        └───┴───┴───┴─────────┴─────────────────────┘
         u32 u16 u16   u64       ≤ record_stride-16
```

- `len`: payload size (bytes), không bao gồm 16B header.
- `type`: app-defined record type. Logging app map field này thành level `TRACE..FATAL`.
- `flags`: app-defined flags. Core chỉ reserve bit 0 = `TRUNCATED`.
- `record_stride = align64(16 + max_payload)`. Lock-in tại `PLAN.md §4`.
- Record có `len + 16 > record_stride` ⇒ producer cắt, set `TRUNCATED`. Không hỗ trợ split-across-slot trong v0.1.

**Wake-up v0.1 — adaptive polling** (không dùng eventfd):

Consumer tham chiếu chạy main loop:

```c++
for (auto& ring : attached_rings) {
    size_t drained = drain_batch(ring, /*max=*/256);
    total += drained;
}
if (total == 0) {
    spin_count++;
    if      (spin_count <  64)    cpu_relax();
    else if (spin_count <  512)   std::this_thread::yield();
    else                          std::this_thread::sleep_for(std::chrono::microseconds(200));
} else {
    spin_count = 0;
}
```

Lý do chọn adaptive polling thay vì eventfd trong v0.1:

- Eventfd cần protocol chia sẻ fd giữa producer process và consumer process → cần UDS broker hoặc `pidfd_getfd`. Cả hai làm phình scope core + thêm failure mode (broker chết, fd revoked, container không cho pidfd).
- Adaptive polling không trace state lên shm. Trade-off: consumer hơi tốn CPU khi idle (~1% với sleep 200µs). Chấp nhận được cho v0.1.
- Khi cần latency thấp hơn: upgrade sang eventfd + UDS broker (Q5 §12.4).

### 7.3 Note Windows port (out of scope, để tham chiếu)

- Thay `shm_open` bằng `CreateFileMapping(INVALID_HANDLE_VALUE, ..., name)`.
- Robust mutex: không có tương đương trực tiếp; phải dùng named mutex + check `WAIT_ABANDONED`.
- Eventfd: thay bằng `CreateEventA` + `WaitForMultipleObjects`.

---

## 8. Security

### 8.1 POSIX permission

- `shm_open(name, O_CREAT|O_EXCL|O_RDWR, 0600)` rồi `fchmod(fd, perm)` ngay sau đó. Default `perm = 0600`, opt-in `0660` cho group.
- **Không** dùng `umask` (process-global, không thread-safe — review §13). Sequence "tạo restrictive + fchmod" đảm bảo:
  - Không có cửa sổ thời gian mà permission rộng hơn ý định.
  - Không can thiệp `umask` của thread khác cùng process.
- Owner UID lưu ngầm trong inode `/dev/shm/<name>` (kernel quản); framework không lưu trùng.

### 8.2 HMAC integrity header

- Khi `flags & HMAC_ENABLED`: HMAC-SHA256 trên **immutable block** `header[0..128)` (xem §4.1). Mutable runtime state (`owner_pid`, `ref_count`, `state`, heartbeat...) ở `[160..192)` *không* nằm trong scope HMAC, nên heartbeat tick mỗi 100ms không invalid chữ ký.
- Verify HMAC **một lần** lúc `attach()`. Không re-verify trong steady state. Nếu registry đã ghi segment là HMAC-enabled thì attach reject header bị sửa để tắt flag HMAC hoặc zero `hmac[]`.
- Compute HMAC trong `create()`: ghi tất cả immutable field, zero `hmac[32]`, compute HMAC over `[0..128)`, ghi vào `hmac[]`.
- Key lấy từ:
  1. ENV `SHMFX_HMAC_KEY` (hex 64 char, 32 byte raw) — dev / pilot.
  2. File `/etc/shmfx/hmac.key` (mode 0400 root) — prod khuyến nghị.
  3. Optional Linux keyring (`keyctl`) — future.
- Mục đích: phát hiện tamper / corruption (file `/dev/shm` có thể bị process khác ghi đè nếu permission rộng hoặc kernel bug). **Không** chống insider cùng UID (họ có thể đọc key file rồi forge).

### 8.3 Namespace isolation

- Whitelist namespace ở `shm_security.cpp::ALLOWED_NAMESPACES` (`log`, `tlm`, `kv`, `app`, `sys` trong v0.1).
- Per-namespace có config: max segment size, max segment count, perm default.
- Quá quota ⇒ `create()` trả `ErrQuotaExceeded`.

### 8.4 DoS protections

| Vector | Defense |
|--------|---------|
| Tạo nhiều shm fill `/dev/shm` (tmpfs) | Quota per-namespace + total quota toàn framework (default 1 GB). Track ở registry. |
| Segment "zombie" giữ space | Janitor §6.4. |
| Producer spam ring | Bounded record ring + `lost_count`; producer overrun = drop, không block. |
| Consumer chậm gây backpressure | MPSC record ring full ⇒ producer drop record + increment `lost_count`, không block hot path. SPMC telemetry future sẽ dùng per-consumer lag/drop policy riêng. |
| HMAC key brute force | Key 256-bit + file mode 0400; không có path online verify (HMAC chỉ verify khi attach). |

---

## 9. API Surface — C++ class diagram

```c++
namespace shmfx {

  enum class SegmentType : uint32_t { Raw = 1, RecordRing = 2, Kv = 3, Registry = 4 };
  enum class AttachMode  : uint8_t  { ReadOnly = 0, ReadWrite = 1 };

  struct CreateOptions {
      std::string  name;             // /shmfx.<ns>.<name>
      SegmentType  type;
      std::size_t  total_size;
      std::size_t  meta_size = 64;
      mode_t       perm      = 0600;
      uint32_t     flags     = 0;    // HMAC_ENABLED | ROBUST_MUTEX | LOCKFREE_RING | READONLY_PAYLOAD
  };

  class ShmHandle {                  // RAII, move-only
   public:
      ShmHeader&            header();
      std::span<std::byte>  metadata();
      std::span<std::byte>  payload();
      std::string_view      name() const;
      AttachMode            mode() const;
      ~ShmHandle();                  // RW: ref_count--, then munmap (maybe unlink)
                                     // RO: chỉ munmap
   private:
      void* base_; std::size_t size_; int fd_; AttachMode mode_; bool owner_;
  };

  // Result<T> = std::expected<T, ErrorCode> trong C++23, hoặc tl::expected polyfill C++20.
  // Không throw trong hot path.

  class ShmManager {                 // factory, không có instance state
   public:
      static Result<ShmHandle> create(const CreateOptions&);
      static Result<ShmHandle> attach(std::string_view name,
                                      AttachMode mode = AttachMode::ReadWrite);
      static Result<void>      destroy(std::string_view name);  // force, admin only
  };

  class Registry {
   public:
      static Registry& instance();
      Result<void>                register_segment(const RegistryEntry&);
      Result<void>                unregister(std::string_view name);
      std::vector<RegistryEntry>  list(std::string_view prefix = {});
      void                        gc();                 // janitor sweep, RO-attach internally
  };

  template <class Record>
  class MpscRing {                   // header-only, view lên RECORD_RING payload (C++20 std::span)
   public:
      bool        try_push(const Record&);   // thread-safe multi-producer, non-blocking
      bool        try_pop(Record&);          // single consumer only
      std::size_t lost() const;
  };
}
```

Ghi chú API:

- `Result<T>` ưu tiên `std::expected<T, ErrorCode>` (C++23, đã có trên gcc-12+, clang-16+). Fallback `tl::expected` cho compiler cũ hơn — vendor sẵn trong `third_party/`. Không dùng exception trong hot path producer.
- `std::span<std::byte>` đã chuẩn C++20, không cần polyfill.
- Logging API (`Logger`, `LogCenter`, formatter, file rotation) nằm trong `apps/logging/`, không thuộc core API ở trên.

---

## 10. So sánh với các giải pháp hiện có

| Giải pháp | License | Mô hình | Performance | Crash safety | Tooling | Lý do không pick / pick |
|-----------|---------|---------|-------------|--------------|---------|--------------------------|
| **POSIX shm raw** (`shm_open` + `mmap`) | OS | Bytes thô | Cao nhất | ❌ Không có header chuẩn | ❌ | Quá thấp, không reusable. **Ta bọc lên trên cùng.** |
| **Boost.Interprocess** | Boost | C++ STL allocator trên shm | Trung bình (offset_ptr, anonymous mutex) | Mutex thường không robust | Hạn chế | Heavy, kéo header Boost; mutex không robust mặc định; không có discovery / lifecycle policy. |
| **Cap'n Proto / FlatBuffers** | MIT/Apache | Serialization zero-copy | Cao cho read | ❌ Không quản shm | — | Khác chiều: schema chứ không phải transport. Có thể kết hợp về sau (payload là FlatBuffer table). |
| **iceoryx (Eclipse)** | Apache-2.0 | Pub/Sub zero-copy IPC, mempool | Rất cao | ✅ RouDi daemon | ✅ | Mạnh nhưng *phải* chạy RouDi daemon, footprint lớn, model pub/sub fixed-size; overkill cho framework nhỏ cần embeddable/no-daemon. |
| **LCM** | LGPL | UDP multicast + shm fallback | Trung bình | — | ✅ `lcm-spy` | Thiên về cross-host multicast; on-host kém hơn shm thuần. |
| **ROS 2 DDS (Cyclone/Fast)** | Apache | DDS, có shm transport | Trung bình–cao | ✅ | ✅ Rất tốt | Quá khổng lồ cho non-ROS app; phụ thuộc ROS lifecycle. |
| **DPDK `rte_memzone` / `rte_ring`** | BSD | Hugepage shm + lock-free ring | Cao nhất | ❌ (assume primary process sống) | ❌ | Đòi hugepage, root, EAL init; chỉ hợp lý cho NIC/userspace networking. |
| **shmfx (this doc)** | (TBD MIT) | Header + lifecycle + ring | Cao (target ≥5M msg/s) | ✅ Robust mutex + janitor + heartbeat | Có (registry + CLI) | Nhỏ gọn, không daemon bắt buộc, có discovery & integrity. **Pick.** |

Kết luận: nếu đã dùng iceoryx hoặc DDS thì không cần shmfx. Nếu cần một tầng "lighter than iceoryx, heavier than `shm_open`" thì shmfx vào đúng khe đó.

---

## 11. Reference Apps

Core `libshmfx` chỉ cung cấp shared-memory lifecycle, registry, security, robust sync, và generic record ring. Các workflow cụ thể nằm ở package riêng:

| App | Repo path | Design | Ghi chú |
|-----|-----------|--------|---------|
| Distributed Logging | `apps/logging/` | [`05_LOGGING_APP.md`](05_LOGGING_APP.md) | Dùng `SegmentType::RecordRing` + `MpscRing`; optional build target. |

Quy tắc boundary:

- App có thể phụ thuộc `include/shmfx/*`.
- `include/shmfx/*` và `src/*` không include header từ `apps/*`.
- App-defined metadata/payload semantic phải nằm trong app doc/header riêng.
- Demo app không được làm thay đổi ABI core nếu không có use case framework-level.

---

## 12. Phụ lục

### 12.1 Error codes (chính)

| Code | Tên | Ý nghĩa |
|------|-----|---------|
| 0   | `Ok` | success |
| -1  | `ErrInvalidName` | tên không match regex §5.1 |
| -2  | `ErrCorruptedHeader` | magic/version/HMAC fail |
| -3  | `ErrNotFound` | shm object không tồn tại khi attach |
| -4  | `ErrAlreadyExists` | create với O_EXCL nhưng đã có |
| -5  | `ErrPermissionDenied` | EACCES / namespace whitelist fail |
| -6  | `ErrQuotaExceeded` | per-namespace hoặc tổng quota |
| -7  | `ErrPriorOwnerDead` | mutex `EOWNERDEAD`, caller phải recover |
| -8  | `ErrRingFull` | ring đầy, drop record |
| -9  | `ErrVersionMismatch` | version_major khác |
| -10 | `ErrSegmentDead` | state = DEAD |

### 12.2 Kích thước khuyến nghị

| Use case | total_size | record_max | record_stride |
|----------|------------|------------|---------------|
| Record ring nhỏ | 1 MiB | 3840 | 256 |
| Record ring dày | 16 MiB | 61440 | 256 |
| Telemetry (msg lớn) | 64 MiB | 16000 | 4096 |
| KV cache nhỏ | 4 MiB | — | — |

Với `RECORD_RING`, `total_size` phải cover `ShmHeader` + `RecordRingMeta` + 2 cursor cache-line + `record_max * sizeof(RingSlotHeader)` + `record_max * record_stride`.

### 12.3 Tunables

| Const | Default | Mô tả |
|-------|---------|------|
| `HEARTBEAT_TICK_MS` | 100 | Owner tick chu kỳ. |
| `HEARTBEAT_DEAD_NS` | 3 × 10⁹ | Threshold coi owner đã chết. |
| `JANITOR_PERIOD_MS` | 5000 | Daemon janitor sweep interval. |
| `MPSC_CAS_RETRY_BUDGET` | 64 | Số lần producer retry CAS claim slot trước khi coi như transient push failure. |
| `MAX_SEGMENT_SIZE` | 1 GiB | Hard cap per segment. |
| `TOTAL_QUOTA_BYTES` | 4 GiB | Tổng cap framework. |

### 12.4 Open questions (để Phase tiếp theo giải)

- [Q1] Có nên cho phép resize segment in-place (mremap) hay luôn buộc tạo segment mới? — Phase 3/4.
- [Q2] Format file log: plain text vs binary length-prefixed? — reference logging app Phase 8.
- [Q3] Hugepages support (`MAP_HUGETLB`) — opt-in trong `CreateOptions` hay tự detect? — Phase 3.
- [Q4] Có cần Rust binding sớm không? — sau v0.1.
- [Q5] Upgrade wake-up từ adaptive polling sang **eventfd + UDS broker**: khi nào cần (latency target nào)? Có thay bằng `io_uring` poll luôn không? — sau v0.1, drive bởi benchmark thực.
- [Q6] **Resolved v0.4 / OI-04**: cần MPSC ngay v0.1; TLS ring per-thread không dùng làm path chính.
- [Q7] **Resolved Phase 4 / OI-11**: force unlink zombie chỉ khi `state == DRAINING` quá `SEGMENT_ZOMBIE_NS` và scan `/proc/*/maps` không thấy process nào còn map object.
- [Q8] `pidfd_open()` thay `kill(pid,0) + /proc/<pid>/stat` cho liveness check khi kernel ≥ 5.3 — future opt-in; Phase 4 giữ fallback `/proc` vì ABI hiện không lưu pidfd.

---

*Hết Phase 1 core deliverable (v0.5). Phase 2 sẽ hiện thực `shm_header.h` + `shm_types.h` theo §4.1 và phải pass toàn bộ `static_assert` ở §4.1.1 (gồm `is_trivially_copyable_v<ShmHeader>` và offset asserts).*
