# PLAN: SharedMemory Framework (`shmfx`) + Reference Apps

> Mục tiêu: xây dựng `shmfx` như framework shared-memory dùng chung cho nhiều mục đích. Distributed Logging là reference app/package riêng, không được làm lệch API core.

---

## 1. Tổng quan công việc

| # | Phase | Deliverable | Ước lượng | Có thể tách session? |
|---|-------|-------------|-----------|----------------------|
| 1 | Core Design Document | `docs/01_DESIGN.md` (framework chuẩn) + `docs/03_LOGGING_APP.md` (reference app riêng) | 1 lượt | ✅ |
| 2 | Framework Core – Header & Layout | `shm_header.h`, `shm_types.h` | 1 lượt | ✅ |
| 3 | Framework Core – Manager & Registry | `shm_manager.{h,cpp}`, `shm_registry.{h,cpp}` | 1 lượt | ✅ |
| 4 | Framework Core – Lifecycle & Crash Recovery | `shm_lifecycle.{h,cpp}` | 1 lượt | ✅ |
| 5 | Framework Core – Security | `shm_security.{h,cpp}` | 0.5 lượt | gộp với 4 |
| 6 | Framework Core – Sync primitives | `shm_ring.{h,cpp}`, `shm_sync.{h,cpp}` | 1 lượt | ✅ |
| 7 | Reference App – Logging Producer API | `apps/logging/include/shmfx_logging/logger.h`, `apps/logging/src/logger.cpp` | 0.5 lượt | gộp với 8 |
| 8 | Reference App – Logging Center | `apps/logging/src/log_center.cpp` | 1 lượt | ✅ |
| 9 | Build system + framework README + demos | `CMakeLists.txt`, `README.md`, `examples/`, `apps/logging/examples/` | 1 lượt | ✅ |
| 10 | (Tuỳ chọn) Integration / Stress / Benchmark | `tests/`, `benchmark/` — test liên tiến trình, crash recovery thực, stress MPSC, benchmark latency/throughput | 1 lượt | ✅ – cut nếu thiếu |

**Tổng**: ~7–8 lượt hội thoại. Mỗi phase độc lập, mỗi lượt chỉ làm 1 phase để tránh cắt giữa chừng.

---

## 2. Quy tắc tránh hết token

1. **Mỗi lượt chỉ làm đúng 1 phase.** Không gộp 2 phase vào 1 response trừ khi cả 2 đều nhỏ (đã đánh dấu).
2. **File-first**: code và doc đều viết ra file ở repo này (`/apps/source/sharemem/...`), không in dài trong chat.
3. **Trong chat chỉ tóm tắt**: phase vừa xong làm gì, file gì sinh ra, bước tiếp là gì.
4. **Checkpoint cuối mỗi phase**: cập nhật mục §6 trong plan này (✅ done / 🔄 in‑progress).
5. **Khi mở session mới**, user chỉ cần nói:
   > "Đọc `/apps/source/sharemem/PLAN.md`, tiếp tục Phase N."
   Claude sẽ đọc plan + các file đã sinh để khôi phục context.
6. **Ưu tiên cắt**: Phase 10 (test) → Phase 9 demo phụ → các tính năng "nice to have" trong design.

---

## 2.1 Coding convention

1. **Luôn viết kèm chú thích cho code public hoặc dễ bị hiểu sai.** Comment phải giải thích contract, ABI, concurrency, ownership, hoặc lý do thiết kế; không viết comment lặp lại cú pháp hiển nhiên.
2. **Mọi hàm khai báo/định nghĩa trong header public phải có Doxygen comment** ngay phía trên hàm (`///` hoặc `/** ... */`) để sau này generate tài liệu API.
3. **Public enum/struct/constant quan trọng nên có Doxygen comment** khi là một phần API hoặc ABI. Field trong layout shared-memory phải ghi rõ semantic nếu không tự hiển nhiên.
4. **Header public không để function trần.** Kể cả `constexpr`, `inline`, helper nhỏ cũng phải có `@param`, `@return` khi có tham số hoặc giá trị trả về.
5. **Từ Phase 3 trở đi, mỗi phase có code production phải kèm unit test tối thiểu** cho behavior chính của phase đó. Unit test đi cùng phase, không dồn sang Phase 10. Nếu chưa test được ngay vì thiếu build system/dependency, phase đó phải ghi rõ test gap và thêm test stub/case mô tả trong `tests/` khi khả thi.

---

## 3. Cấu trúc thư mục mục tiêu

```
/apps/source/sharemem/
├── PLAN.md                         # file này
├── docs/
│   ├── 01_DESIGN.md                # Core framework design
│   ├── 02_OPEN_ISSUES.md           # Cross-cutting backlog
│   ├── 03_LOGGING_APP.md           # Reference logging app design
│   └── 04_USAGE.md                 # User guide
├── include/shmfx/
│   ├── shm_error.h                 # Phase 2
│   ├── shm_header.h                # Phase 2
│   ├── shm_types.h                 # Phase 2
│   ├── shm_manager.h               # Phase 3
│   ├── shm_registry.h              # Phase 3
│   ├── shm_lifecycle.h             # Phase 4
│   ├── shm_security.h              # Phase 5
│   ├── shm_ring.h                  # Phase 6
│   └── shm_sync.h                  # Phase 6
├── src/
│   ├── shm_manager.cpp
│   ├── shm_registry.cpp
│   ├── shm_lifecycle.cpp
│   ├── shm_security.cpp
│   ├── shm_ring.cpp
│   └── shm_sync.cpp
├── apps/
│   └── logging/
│       ├── include/shmfx_logging/
│       │   └── logger.h            # Phase 7
│       ├── src/
│       │   ├── logger.cpp          # Phase 7
│       │   └── log_center.cpp      # Phase 8
│       ├── examples/
│       │   ├── demo_producer.cpp   # Phase 9, logging demo
│       │   └── demo_multi_producer.cpp
│       └── README.md               # Phase 9
├── examples/
│   ├── raw_segment.cpp             # Phase 9, core demo
│   └── ring_mpsc.cpp               # Phase 9, core demo
├── tests/                          # Phase 10 (optional)
├── CMakeLists.txt                  # Phase 9
└── README.md                       # Phase 9
```

---

## 4. Quyết định kỹ thuật cố định (lock-in)

Các quyết định này chốt ngay từ đầu để các phase sau nhất quán, không phải debate lại:

| Khía cạnh | Quyết định |
|-----------|-----------|
| Target platform | Linux (POSIX `shm_open` + `mmap`). Windows port đề cập trong design nhưng không code. |
| Ngôn ngữ | C++20; cho phép `std::span`, `<concepts>`, `<bit>` |
| Naming convention shm | `/shmfx.<namespace>.<name>`; namespace `log` chỉ là một consumer của core convention |
| Magic number | `0x53484D46` ("SHMF" ASCII) |
| Version | major.minor 16+16 bit |
| Header size | cố định 256 bytes, padding cho alignment |
| Layout | `[ShmHeader 256B][user metadata N B][payload …]` |
| Concurrency | Robust pthread mutex cho control plane + generic lock-free MPSC record ring trong core; SPMC broadcast để future |
| Crash recovery | Heartbeat counter + reference count + janitor process/lib khi attach |
| Security | UNIX permission + optional HMAC-SHA256 trên header + namespace prefix |
| Discovery | Registry shm cố định tên `/shmfx.registry` chứa danh sách segments |
| Record ring payload | Generic record-based ring: `[u32 len][u16 type][u16 flags][u64 ts_or_user][...payload]`; semantic field do app quyết định |
| Reference apps | Logging nằm dưới `apps/logging`, build optional, không được thêm dependency ngược vào `libshmfx` |

Bất kỳ thay đổi nào với bảng trên phải cập nhật ở đây trước khi code.

---

## 5. Outline chi tiết Phase 1 (Design Document)

Để Phase 1 không bị tràn token, định sẵn cấu trúc:

```
01_DESIGN.md
1. Mục tiêu & Phạm vi
2. Use cases & Yêu cầu (functional + non-functional)
3. Kiến trúc tổng thể (sơ đồ ASCII)
4. Memory Layout
   4.1 Header chung (struct ShmHeader 256B – đặc tả từng field)
   4.2 User metadata vùng
   4.3 Payload region
5. Naming & Discovery
   5.1 Naming rule
   5.2 Registry segment
6. Lifecycle Management
   6.1 Create / Attach / Detach / Destroy state machine
   6.2 Reference counting
   6.3 Heartbeat & owner liveness
   6.4 Crash recovery (janitor)
7. Concurrency Model
   7.1 Robust mutex (PTHREAD_MUTEX_ROBUST)
   7.2 Lock-free ring (memory order, ABA)
8. Security
   8.1 POSIX perm
   8.2 HMAC integrity header
   8.3 Namespace isolation
   8.4 DoS protections (size limit, rate limit)
9. API Surface (C++ class diagram)
10. So sánh với:
    - Boost.Interprocess
    - POSIX shm raw
    - Cap'n Proto / FlatBuffers (serialization)
    - iceoryx (Eclipse, zero-copy IPC)
    - LCM, ROS2 DDS
    - shmem trong DPDK
    Bảng so sánh: feature, license, perf, complexity, our pick
11. Reference apps
    11.1 Boundary: app không nằm trong core API
    11.2 Distributed Logging xem `docs/03_LOGGING_APP.md`
12. Phụ lục: error codes, kích thước khuyến nghị, tunables
```

---

## 6. Checkpoint (cập nhật sau mỗi phase)

- [x] Phase 1 – Core design (`docs/01_DESIGN.md`) + logging app design (`docs/03_LOGGING_APP.md`)
- [x] Phase 2 – Header & types
- [x] Phase 3 – Manager & Registry
- [x] Phase 4 – Lifecycle & Crash Recovery
- [x] Phase 5 – Security
- [x] Phase 6 – Framework ring buffer & sync
- [x] Phase 7 – Reference logging producer package
- [x] Phase 8 – Reference logging center package
- [x] Phase 9 – Build + core demos + app demos + README
- [x] Phase 10 – Integration tests, stress tests, benchmarks (optional)

---

## 7. Câu lệnh chuẩn để resume session

User chỉ cần gửi 1 trong các câu sau ở session mới:

- "Tiếp tục Phase 1" → Claude đọc PLAN.md rồi viết design doc.
- "Tiếp tục Phase N, đã xong tới Phase M" → Claude tự rà file đã có rồi tiếp.
- "Show PLAN" → Claude đọc và in tóm tắt §1 + §6.
- "Cut scope: bỏ phase X, Y" → Claude cập nhật §1 và §6.
