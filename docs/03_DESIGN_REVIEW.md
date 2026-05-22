# 03 — Design Review: `01_DESIGN.md` v0.2

> **🧊 FROZEN** sau v0.3. 5/19 items đã ✅ fix trong v0.3; 14 items còn open đã chuyển sang **[`04_OPEN_ISSUES.md`](04_OPEN_ISSUES.md)** (single source of truth). File này giữ làm audit trail review v0.2.

> Self-review v0.2 sau khi đã incorporate `02_DESIGN_REVIEW`. Tài liệu này tập trung 3 thứ: **bug thực sự cần fix**, **trade-off đã đánh đổi (Gain / Cost / Mitigation)**, và **risk chưa xử lý**. Không lặp lại các issue đã giải quyết trong `02_DESIGN_REVIEW`.

## Đánh Giá Tổng Quan

V0.2 đã giải quyết được hầu hết các issue ở `02_DESIGN_REVIEW`, đặc biệt là HMAC mutable, PID reuse, registry seqlock, mutex consistent order. Tuy nhiên việc đáp ứng review đã đánh đổi *vẫn để lại trade-off thực*; bản v0.2 chưa nói rõ phần đánh đổi đó, nên Phase 2 dễ ngộ nhận là "tất cả vấn đề đã được giải quyết hoàn toàn".

Có **2 bug code-level cần fix trước Phase 2** (sai ngữ nghĩa C++ hoặc tràn số). Còn lại là trade-off — chấp nhận được nhưng phải document.

---

## ~~1. Bug Cần Fix Trước Phase 2~~ ✅ FIXED (v0.3)

> **Status**: Cả 2 bug đều đã được giải quyết trong `01_DESIGN.md` v0.3 — xem changelog 0.2 → 0.3. Diagnostic gốc giữ lại để tham chiếu.

### ~~1.1 `RegistryEntry snap = slot;` không hợp lệ — `std::atomic` không copy được~~ ✅

> **FIXED in v0.3**: chuyển sang `std::atomic_ref<T>` trên storage POD. `RegistryEntry` giờ `is_trivially_copyable_v` → `RegistryEntry snap = slot;` hợp lệ. Áp dụng cùng cho `ShmHeader` mutable, `LogRingMeta::lost_count`, `RingCursor::pos_storage`. Xem `01_DESIGN.md` §4.1 intro + §4.1.1 static_asserts + §5.2 seqlock code.

Vị trí: `01_DESIGN.md` §5.2, code block reader path (line 324).

```c++
RegistryEntry snap = slot;                // POD copy   ← SAI
```

`RegistryEntry` chứa `std::atomic<uint32_t> seq` (line 282). `std::atomic<T>` có copy constructor bị **delete** explicitly bởi standard. Đoạn code trên không compile.

**Hệ quả nếu Phase 2 copy y nguyên**: build fail. Dev phải tự sửa, có thể chọn workaround sai (vd `memcpy` — UB với atomic theo strict aliasing).

**Đề xuất fix (C++20-correct, không UB)**:

```c++
// Bỏ atomic khỏi struct, dùng atomic_ref khi cần access seq.
struct RegistryEntry {                       // 128 byte, POD, copyable
    uint32_t seq_storage;                    // accessed via std::atomic_ref<uint32_t>
    uint32_t _pad0;
    char     name[64];
    // ... các field khác như cũ ...
};
static_assert(std::is_trivially_copyable_v<RegistryEntry>);

// Writer:
std::atomic_ref<uint32_t> seq(slot.seq_storage);
uint32_t s = seq.load(std::memory_order_relaxed);
seq.store(s + 1, std::memory_order_release);   // odd
// ... write fields ...
seq.store(s + 2, std::memory_order_release);   // even

// Reader:
std::atomic_ref<uint32_t> seq(slot.seq_storage);
for (;;) {
    uint32_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1u) { cpu_relax(); continue; }
    RegistryEntry snap = slot;                 // OK: POD copyable
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = seq.load(std::memory_order_acquire);
    if (s1 == s2) return snap;
}
```

`std::atomic_ref` là tính năng C++20 (chính lý do nâng standard ở 02_DESIGN_REVIEW). Đây là cách dùng đúng quy chuẩn.

### ~~1.2 `creator_start_time` / `owner_start_time` là `uint32_t` — overflow ở uptime > ~1.5 năm~~ ✅

> **FIXED in v0.3**: cả 2 field bump u32 → u64. Re-layout header: drop `header_size` (luôn = 256, contract qua `version_major`) để tiết kiệm 4B cho immutable; mutable block 32→40B; `control_mutex` shift offset 192→200; `_reserved` 24→16. Static asserts cập nhật ở §4.1.1.

Vị trí: §4.1, offset 60 và 164.

`/proc/<pid>/stat` field 22 (`starttime`) là **clock ticks since boot**. Kernel khai báo `unsigned long long`. Với `CLK_TCK = 100` (mặc định Linux x86_64), `uint32_t` chứa được:

```
2^32 / 100 / 86400 ≈ 497 ngày ≈ 1.36 năm
```

Server long-running (telco, backbone, embedded gateway) hoàn toàn có thể uptime > 1.5 năm. Sau overflow:

- Process khởi động lúc tick `2^32 + k` lưu vào header = `k` (32-bit truncated).
- Liveness check compare `k` với `read_proc_start_time()` cũng truncate về `k`.
- **Vô tình match** — không bị false negative.
- Nhưng nếu kernel đổi cách emit (đôi khi kernel `vmlinux` bug fix lại field 22), behavior không xác định.

**Đề xuất fix**: bump cả hai field lên `uint64_t`. Hệ quả layout:

- `creator_start_time` 4 → 8 byte: immutable block +4 byte.
- `owner_start_time` 4 → 8 byte: mutable block +4 byte.
- Re-pack header bằng cách:
  - Immutable: gộp `creator_pid` (4) + `creator_start_time` (8) = 12 byte → cần thêm 4 byte padding. Hoặc đổi `name[64]` xuống `name[60]` (tiếc 4 byte name nhưng vẫn đủ cho regex 60 ký tự).
  - Mutable: thay vì `owner_pid (4) + owner_start_time (4) + ref_count (4) + state (4) = 16 byte` thành `owner_pid (4) + owner_start_time (8) + ref_count (4) + state (4) = 20`; pad lên 24. Bỏ 4 byte từ `_reserved[24]` xuống `_reserved[20]`.

Phase 2 phải chốt layout cuối cùng. Trade-off thay thế: chấp nhận u32 nhưng document hạn chế "không support uptime > 1 năm" — hợp lý cho desktop/cloud, không hợp lý cho embedded.

---

## 2. Trade-off Đã Đánh Đổi (Gain / Cost / Mitigation)

Mục này phân tích từng quyết định v0.2, làm rõ phần *đã trả giá* mà v0.2 chưa nói thẳng.

### 2.1 Tách header thành Immutable / HMAC / Mutable

| | |
|---|---|
| **Gain** | HMAC không invalid khi heartbeat tick mỗi 100ms. Verify-once-at-attach trở nên rẻ và đúng. |
| **Cost** | Mutable state (`owner_pid`, `ref_count`, `state`, `heartbeat_*`) **không** được integrity-protected nữa. Một process cùng UID có quyền ghi `/dev/shm/<name>` có thể:<br>• Set `ref_count = 0` → janitor unlink segment đang live → DoS.<br>• Set `state = DEAD` → block attach mới.<br>• Set `heartbeat_last_ns = now` → fake-alive owner đã chết → block recovery.<br>• Set `owner_pid` → hijack ownership. |
| **Mitigation** | (a) Mặc định `perm = 0600` (chỉ owner UID ghi được). (b) `flags` immutable nằm trong HMAC → kẻ tấn công không thể bật/tắt feature. (c) Phase 4 có thể thêm canary CRC32 trên mutable block, kiểm soft mỗi N tick. |
| **Verdict** | Chấp nhận. V0.1 *về lý thuyết* HMAC mutable nhưng *thực tế* không thể verify mỗi access (cost SHA256 ~500ns vượt target 80ns/push). V0.2 *trung thực hơn*. Cost ròng = 0 trong threat model "trusted same-UID". |

### 2.2 PID reuse defense bằng `start_time` đọc từ `/proc/<pid>/stat` ✅ partial

> **Partially FIXED in v0.3**: (a) bug u32 overflow đã fix (§1.2). (b) Fallback path trong `owner_is_dead()` đã được tường minh hoá ở §6.3 — `read_proc_start_time()` trả `bool`; nếu fail (sandbox), skip bước (2), chỉ dùng `kill(pid,0)` + heartbeat staleness. Caveat (4) container start_time mismatch và (c) pidfd upgrade vẫn open.

| | |
|---|---|
| **Gain** | Phát hiện được PID reuse trong container/PID namespace nhỏ (Docker default 32k PID, k8s pod). |
| **Cost** | (1) Thêm 1 syscall + parse text mỗi liveness check. Cost ~5-10 µs (negligible cho path 100ms tick).<br>~~(2) **Container/sandbox không expose `/proc`**: fallback chưa định nghĩa~~ → ✅ Fixed: §6.3 contract "fail ⇒ skip bước (2)".<br>~~(3) Bug §1.2~~ → ✅ Fixed.<br>(4) Container có CLOCK_BOOTTIME khác host: nếu `/proc` từ host nhưng process trong container, start_time có thể không khớp. **Vẫn open.** |
| **Mitigation** | ~~(a) Fix §1.2 (u64)~~ ✅. ~~(b) Phase 4 fallback policy~~ ✅ §6.3.<br>(c) Future: `pidfd_open()` (kernel ≥ 5.3) thay luôn — đã ghi §12.4 Q8. |
| **Verdict** | Major risks đã giảm. Container CLOCK_BOOTTIME edge case (4) cần Phase 4 hoặc pidfd. |

### 2.3 `ref_count` chỉ tăng khi RW attach

| | |
|---|---|
| **Gain** | Janitor inspect được mà không tự bump số đếm; logic "ref_count == 0 ⇒ destroy" trở nên có nghĩa. |
| **Cost** | (1) **RO consumer bị "orphan" sau unlink**: POSIX semantic của `shm_unlink` chỉ remove name, không invalidate mmap đang sống. Janitor unlink → tên biến mất khỏi `/dev/shm` → consumer RO tiếp tục đọc mmap cũ (stale data) cho tới `munmap`. Không SIGBUS, nhưng dữ liệu có thể bị producer mới ghi đè nếu name được tạo lại với inode khác... thực ra inode mới sẽ là segment mới (different physical pages), nên consumer cũ vẫn an toàn về mặt memory — chỉ là đọc nội dung "đã chết".<br>(2) `ref_count` không phản ánh tổng số attach. Tooling `shmfxctl ls` nếu hiển thị "ref_count" có thể gây hiểu lầm cho user. |
| **Mitigation** | (a) `ShmHandle::mode()` API public — tooling có thể tự đếm RO attach qua `/proc/*/maps` nếu cần.<br>(b) `LogCenter` (consumer chính của framework) tự handle orphan bằng cách periodic re-validate `state` — nếu `DEAD` thì detach.<br>(c) Document trong README: "ref_count = writer count, not total attach". |
| **Verdict** | Chấp nhận. Alternative (RO cũng bump nhưng dùng flag riêng) thêm complexity mà không giải quyết orphan vấn đề kernel-level. |

### 2.4 Registry seqlock per-slot

| | |
|---|---|
| **Gain** | Read path không lock, không bị block bởi writer. Discovery rẻ hơn (`list()` không serialize). |
| **Cost** | (1) **Reader starvation**: nếu writer ghi liên tục một slot, reader retry mãi. Hiếm trong UC (writer = register/unregister, rate thấp), nhưng vẫn là attack surface.<br>(2) Reader phải copy POD 128B mỗi snapshot — cache traffic khi list 1024 slot = 128 KB.<br>(3) Code phức tạp hơn so với simple lock; dễ sai memory_order khi review/maintain.<br>(4) Bug §1.1 ở trên. |
| **Mitigation** | (a) Bound retry count, fallback acquire `control_mutex` nếu retry > 100 (starvation prevention).<br>(b) Reader chỉ copy slot nếu bit `bitmap[i]` set — skip slot rỗng. Giảm cache traffic.<br>(c) Unit test seqlock với TSan + concurrent writer/reader. |
| **Verdict** | Chấp nhận. Alternative (RWLock trên payload) đơn giản hơn nhưng pthread rwlock không robust → owner chết khi đang write = mutex hỏng vĩnh viễn. Seqlock không có vấn đề này. |

### 2.5 Ring scope v0.1 = SPSC only

| | |
|---|---|
| **Gain** | Implementation đơn giản, dễ verify, fit timeline Phase 6. Test surface nhỏ. |
| **Cost** | (1) **Multi-thread producer trong 1 process phải dùng TLS ring per-thread** → app có 100 thread ⇒ 100 segment per process ⇒ `MAX_REGISTRY_ENTRIES = 1024` chỉ chứa được ~10 process như vậy.<br>(2) `log_center` phải attach + poll 100 ring/process × N process — chi phí discovery + polling tăng theo số ring chứ không số process.<br>(3) Producer thread short-lived (worker pool spawn/destroy) sẽ liên tục create/unlink segment — load lên registry mutex. |
| **Mitigation** | (a) `Logger::init` có option `share_ring_across_threads = false/true` — true sẽ dùng 1 ring per-process với coarse-grained mutex push (slow path, không lock-free); false là TLS như §11.2.<br>(b) MPSC implement ở Phase sau (§12.4 Q6) khi profile thực cho thấy TLS không scale.<br>(c) `MAX_REGISTRY_ENTRIES` có thể bump lên 4096/8192 nếu cần — cost 1 MB. |
| **Verdict** | Chấp nhận có điều kiện. Nếu UC v0.1 thực sự là "service đơn-luồng/few-thread + log_center", TLS đủ. Nếu app target là worker pool 100+ thread, *phải* implement MPSC trước demo, đừng chờ Phase 6. **Đây là quyết định cần xác nhận với user.** |

### 2.6 Wake-up = adaptive polling thay vì eventfd

| | |
|---|---|
| **Gain** | Không cần protocol share fd giữa process. Không phụ thuộc `pidfd_getfd` (kernel ≥ 5.6) hay UDS broker. |
| **Cost** | (1) `log_center` idle burn ~1% CPU (200µs sleep). Trên hệ thống 100 vCPU container limit 2 vCPU, 1% = đáng kể (50% của 1 CPU/100 instance).<br>(2) **Tail latency p99 += polling interval**. Với 200µs sleep + scan, p99 worst-case ≈ 200µs + ring_count × ~50ns. Mục tiêu `Logger full path p99 < 5µs` ở §2.3 **không khả thi** nếu định nghĩa "full path" bao gồm consumer thấy được record — chỉ khả thi nếu định nghĩa "producer trả về `try_push`".<br>(3) Scan cost O(N rings) mỗi iteration — không scale tốt khi `LogCenter` attach > vài trăm ring. |
| **Mitigation** | (a) §2.3 *đã* định nghĩa rõ "full path" là producer-side; nhưng nên ghi thêm bucket *consumer-visible latency* riêng (target < 500µs p99) để khỏi mơ hồ.<br>(b) Adaptive sleep có thể tăng max sleep lên 1ms nếu idle lâu — giảm CPU thêm ~5x.<br>(c) Khi user cần latency < 100µs, switch sang eventfd path (§12.4 Q5) — nhưng v0.1 không deliver. |
| **Verdict** | Chấp nhận cho v0.1. **Phải document trong README**: "log_center có lag 0–200µs end-to-end. Không phù hợp cho audit log realtime hay alert critical." |

### 2.7 `umask` → `fchmod`

| | |
|---|---|
| **Gain** | Thread-safe. |
| **Cost** | Cửa sổ ~µs giữa `shm_open(...,0)` và `fchmod(fd, 0600)` mà segment tồn tại với mode `0000`. Trong cửa sổ này, process khác cùng UID gọi `shm_open(...)` sẽ `EACCES`. |
| **Mitigation** | Segment chưa được publish vào registry trong cửa sổ này → nobody-else nên biết tên để open. Window là race-free khỏi external view. |
| **Verdict** | Chấp nhận. Trade-off rất nhẹ so với rủi ro `umask` race. |

### 2.8 `pthread_mutex_consistent` chuyển sang caller

| | |
|---|---|
| **Gain** | Đúng semantic — caller validate state trước, mark consistent sau. |
| **Cost** | (1) API bị "leaky": caller phải nhớ gọi `pthread_mutex_consistent(mu.raw())` thủ công. Quên = mutex chuyển `ENOTRECOVERABLE` ở lần unlock kế tiếp → segment hỏng vĩnh viễn.<br>(2) `mu.raw()` expose `pthread_mutex_t*` ra ngoài → abstraction leak. |
| **Mitigation** | (a) Wrap thành RAII helper: `ShmMutexGuard guard(mu); if (guard.prior_owner_dead()) { recover(); guard.mark_consistent(); }` — encapsulate `raw()` lại trong class.<br>(b) Documentation + lint: search `prior_owner_dead()` mà không kèm `mark_consistent()` trong cùng scope. |
| **Verdict** | Chấp nhận. Phase 4/OI-07 implement `ShmMutexGuard`; guard không auto-consistent vì recovery là domain-specific. |

### 2.9 ABI lock-in (Linux + glibc + x86_64/aarch64)

| | |
|---|---|
| **Gain** | Static asserts catch break ngay compile-time. Semantic atomic-on-shm well-defined trong scope này. |
| **Cost** | (1) Không port được sang musl (Alpine container phổ biến trong k8s — `pthread_mutex_t` size khác).<br>(2) Không port được Windows (PLAN §4 đã loại nhưng vẫn worth noting).<br>(3) `static_assert(sizeof(pthread_mutex_t) <= 40)` brittle nếu glibc bump internal trong major version. |
| **Mitigation** | (a) Phase 9 README cảnh báo "Alpine/musl không support, dùng debian-slim".<br>(b) Mutex size assert nên là `static_assert(sizeof(pthread_mutex_t) <= MUTEX_RESERVED_BYTES, "bump version_major nếu glibc thay đổi")` — message giải thích next step. |
| **Verdict** | Chấp nhận. Scope rõ ràng = ít footgun. |

### ~~2.10 `std::atomic<T>` trên shared memory storage~~ ✅ FIXED (v0.3)

> **FIXED in v0.3**: chuyển toàn bộ sang `std::atomic_ref<T>` trên storage POD. Struct trivially copyable, ngữ nghĩa "storage POD, access atomic" rõ ràng. Vẫn còn cảnh báo UB-technically ở mức C++ standard (`atomic_ref` cũng không tường minh inter-process), nhưng đã fold thành ABI lock-in §4.1.1 documentation thay vì để latent.

| | |
|---|---|
| **Gain** | ~~API ergonomic~~. Cleaner: storage POD = copyable struct. Loại bỏ bug §1.1. |
| **Cost** | ~~UB technically~~. Vẫn UB nhẹ ở C++ standard level (atomic_ref inter-process không guarantee), nhưng đã document trong ABI lock-in. Compiler emit hardware atomic instructions — well-defined Linux+glibc. |
| **Mitigation** | ✅ Done — `static_assert(std::atomic_ref<u32/u64>::is_always_lock_free)` + `is_trivially_copyable_v<ShmHeader>` đều có ở §4.1.1. |
| **Verdict** | Done. Future stricter portability: fallback `__atomic_*` builtins — đã ghi §4.1.1. |

---

## 3. Risk Chưa Xử Lý / Chưa Có Trong Tài Liệu

Những điểm sau *không* sai trong v0.2 nhưng cũng chưa được đặt thành vấn đề:

### 3.1 Heartbeat thread per owner — chi phí ẩn

Mỗi process tạo segment làm owner ⇒ spawn 1 thread heartbeat tick 100ms. Process owner 10 segment ⇒ 10 thread (hoặc 1 multiplexed). V0.2 không nói rõ. Đề xuất: `Logger::init` (và `ShmManager::create`) chia sẻ 1 thread cấp thư viện, tick mọi segment owner của process.

### 3.2 Owner ownership transfer chưa có ai trigger

§6.3 nói "consumer có thể CAS owner_pid" để take over, nhưng:

- Consumer là RO attach (theo §6.2). RO không có quyền ghi mutable block → CAS fail (mmap PROT_READ).
- Để transfer thực sự, consumer phải re-attach RW → bump ref_count → mâu thuẫn với "consumer chỉ RO".
- Hoặc: `LogCenter` luôn RW-attach để có khả năng transfer + flush; chấp nhận làm bẩn ref_count.

V0.2 chưa giải quyết. Đề xuất Phase 4: `LogCenter` dùng RW attach (semantic là "co-owner"), document tường minh.

### 3.3 Janitor "force unlink zombie" chưa có thuật toán

§6.2 nhắc `SEGMENT_ZOMBIE_NS` (1h) nhưng §6.4 pseudocode không có nhánh "rc > 0 nhưng đã DRAINING quá `SEGMENT_ZOMBIE_NS`". Q7 §12.4 đã ghi nhận. Cần Phase 4.

### 3.4 Registry mutex chết — không có recovery cho registry chính nó

`/shmfx.registry` cũng có `control_mutex` (vì là segment thường). Nếu process hold mutex registry chết giữa update, robust mutex EOWNERDEAD next-locker. Nhưng:

- "Next locker" là ai? Trên framework chưa có background thread giữ vai trò "registry janitor".
- Một process tình cờ register/unregister tiếp theo sẽ phải recover registry state → process đó chịu cost không công bằng, có thể fail luôn (vd janitor pass đầu).

Đề xuất: `ShmMutexGuard` (đề xuất §2.8) cần helper `recover_registry_invariant()` — re-scan bitmap consistency với slots, reset slot dở.

### ~~3.5 Performance targets vs. polling fundamentally không tương thích~~ ✅ FIXED (v0.3)

> **FIXED in v0.3**: §2.3 viết lại thành **4 bucket** đúng như đề xuất bên dưới. Bucket 4 "End-to-end visible" tường minh hoá impact của adaptive polling (≤ 200µs idle sleep) — không còn ambiguity producer-side vs end-to-end.

~~§2.3 target "Logger full path p50 < 600 ns, p99 < 5 µs" — chỉ đo producer trả về `try_push`. Nếu user/PM hiểu "full path" là end-to-end tới file, target sẽ miss hoàn toàn.~~

~~**Đề xuất**: viết lại §2.3 với 4 bucket rõ ràng~~ ✅ Đã áp dụng. Xem §2.3 hiện tại trong `01_DESIGN.md`.

### 3.6 `MAX_REGISTRY_ENTRIES = 1024` là hard cap

Không có path để bump runtime. Nếu app spawn nhiều segment dynamic (vd test sweep, hoặc service mesh có 5k pod cùng host), tràn. V0.2 không alert.

Đề xuất: thêm `RegistryPayload::version` ở header; v1 = 1024, v2 = 8192 (tăng size segment registry). `Registry::open_or_create()` chọn v lớn nhất compatible.

### 3.7 SPSC `head/tail` 2 cache line — cache line size assumption

§7.2 dùng `alignas(64)` — đúng x86_64 / aarch64. Future ARM (Apple M-series có 128B cache line cho L1) sẽ false-share trong cache line. Không phải bug now nhưng nên `constexpr` cache line size theo target.

---

## 4. Đề Xuất Cho v0.3 (Hoặc Phase 4 Tail Fix)

Ưu tiên giảm dần — items đã apply trong v0.3 được gạch:

1. ~~**Bug fix bắt buộc** (§1.1, §1.2)~~ ✅ v0.3.
2. ~~**`atomic_ref<T>` thay `atomic<T>` member** (§2.10 + §1.1 combined)~~ ✅ v0.3.
3. **`ShmMutexGuard` RAII** (§2.8): chống forget-to-consistent footgun. ✅ Resolved in Phase 4.
4. ~~**`/proc` fallback path** (§2.2)~~ ✅ v0.3 §6.3.
5. ~~**Re-define §2.3 perf bucket** (§3.5)~~ ✅ v0.3.
6. **TLS-vs-shared-ring decision** (§2.5): xác nhận với user UC thật là "few-thread service" hay "high-fan-out worker pool". Có thể cần MPSC ngay v0.1. **Open — cần user input.**
7. **Heartbeat thread sharing** (§3.1). ✅ Resolved in Phase 4.
8. **Cache line size constant** (§3.7). **Open.**
9. **Janitor zombie algorithm** (§3.3). ✅ Resolved in Phase 4.

**Còn lại 5 open items** sau v0.3. Items 3, 7, 8, 9 có thể fold thẳng vào Phase 2/3/4 mà không cần thêm bản design doc. Item 6 cần user decision trước.

---

## Kết Luận

V0.2 đã giải quyết hầu hết feedback v0.1, nhưng việc giải quyết đã đẻ ra trade-off mà bản thân v0.2 chưa nói thẳng. 2 bug code-level (§1) phải fix trước khi viết code Phase 2 — phần còn lại là decision points: chấp nhận trade-off hoặc upgrade lên v0.3.

Đặc biệt cần xác nhận với user **mục §2.5 (SPSC scope) và §3.5 (perf bucket end-to-end)** — đây là 2 chỗ dễ gây kỳ vọng sai khi delivery.

---

### Status sau khi apply v0.3 (option 1 của user)

✅ **Đã fix** trong `01_DESIGN.md` v0.3:
- §1.1 — `atomic_ref<T>` pattern (struct copyable)
- §1.2 — `start_time` u64 (chống overflow uptime)
- §2.2 partial — `/proc` fallback rõ ràng
- §2.10 — atomic_ref clean ngữ nghĩa C++20
- §3.5 — 4 bucket latency có "end-to-end visible"

🟡 **Còn open** (chờ decision/Phase sau):
- §2.1 HMAC mutable (no integrity) — chấp nhận trong threat model "trusted UID"
- §2.3 RO orphan after unlink — document trong README
- §2.4 reader starvation, §2.5 SPSC scope — **cần user xác nhận UC**
- §2.6 polling CPU/latency cost — README disclaimer
- §2.7 fchmod window — micro, accept
- §2.8 mutex API leaky — `ShmMutexGuard` Phase 4
- §2.9 ABI lock-in — accept
- §3.1 heartbeat thread sharing — Phase 4
- §3.2 ownership transfer needs RW — log_center sẽ là "co-owner" (Phase 8)
- §3.3 zombie cleanup algorithm — Phase 4
- §3.4 registry mutex recovery — Phase 4 (gộp §2.8 `ShmMutexGuard`)
- §3.6 `MAX_REGISTRY_ENTRIES` hard cap — Phase 3 nếu user cần
- §3.7 cache line constant — Phase 6 (data plane)

**Bước tiếp theo**: option 1 hoàn tất. Có thể đi Phase 2 (`shm_header.h` + `shm_types.h`) — design contract đã ổn định, static_asserts đã liệt kê đủ ở `01_DESIGN.md` §4.1.1. Hoặc user nêu thêm decision cho §2.5 (SPSC vs MPSC) trước khi Phase 6 nếu UC target là worker pool nhiều thread.
