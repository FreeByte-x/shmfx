# 04 — Open Issues Backlog (post-v0.3)

> Single source of truth cho các issue còn open sau toàn bộ review v0.1 → v0.3.
> Reviews [`02_DESIGN_REVIEW.md`](02_DESIGN_REVIEW.md) (v0.1) và [`03_DESIGN_REVIEW.md`](03_DESIGN_REVIEW.md) (v0.2) **frozen** sau v0.3; chi tiết lịch sử còn ở đó. File này là nơi duy nhất track open work từ đây trở đi.

## Trạng thái sau v0.3

| Source | Resolved | Open |
|--------|----------|------|
| `02_DESIGN_REVIEW` (v0.1 review) | **14/14 ✅** — fold hết vào v0.2 / v0.3 | 0 |
| `03_DESIGN_REVIEW` (v0.2 review) | **12/19 ✅** — `§1.1`, `§1.2`, `§2.1`, `§2.2 partial`, `§2.3`, `§2.5`, `§2.6`, `§2.9`, `§2.10`, `§3.2`, `§3.5`, `§3.7` | 7 |
| **Tổng open** | | **7** |

## Backlog

Ký hiệu ưu tiên:

- 🔴 **Blocker** — chặn phase tiếp theo hoặc cần user decision
- 🟠 **High** — phase work cần wiring sớm để không phá lại
- 🟡 **Medium** — phase work bình thường
- 🟢 **Low** / accept-with-doc

| ID | Pri | Title | Source | Phase | Resolution path |
|----|-----|-------|--------|-------|-----------------|
| ~~OI-01~~ ✅ | 🟢 | HMAC không cover mutable state — DoS surface trong threat model "trusted same-UID" | 03 §2.1 | 9 (README) | Resolved in Phase 9 README: documented immutable-header-only HMAC and explicit same-UID mutable-state limitation. |
| ~~OI-02~~ ✅ | 🟡 | RO consumer tiếp tục đọc stale memory sau `shm_unlink` ("orphan") | 03 §2.3 | 9 README + app consumers | Resolved in Phase 8 for logging app and documented in Phase 9 README: `LogCenter` self-validates `state == DEAD`, drains once more, then detaches. |
| ~~OI-03~~ ✅ | 🟢 | Reader có thể starve trên registry seqlock nếu writer rất nóng | 03 §2.4 | 3 | Resolved in Phase 3: `Registry::list()` dùng bound `retry < 100` và fallback acquire `control_mutex`. |
| ~~OI-04~~ ✅ | 🔴 | **SPSC-only có scale cho UC user không, hay cần MPSC ngay v0.1?** | 03 §2.5 | Trước Phase 6 | **Resolved**: user chọn **MPSC ngay v0.1**. Phase 6 implement generic `MpscRing` trong core; logging app chỉ là một consumer của primitive này. |
| ~~OI-05~~ ✅ | 🟡 | Adaptive polling burn ~1% CPU idle + tail latency tới 200µs | 03 §2.6 | 9 README + 8 | Resolved in Phase 8 implementation and Phase 9 README: `LogCenter::poll_once()` uses adaptive spin/yield/sleep backoff; eventfd wakeup remains future. |
| OI-06 | 🟢 | Cửa sổ µs giữa `shm_open(...,0)` và `fchmod` mode 0000 | 03 §2.7 | n/a | Accept — segment chưa published vào registry trong cửa sổ này, không reachable. |
| ~~OI-07~~ ✅ | 🟠 | `pthread_mutex_consistent()` order shift sang caller — dễ quên → mutex `ENOTRECOVERABLE` vĩnh viễn. Gộp luôn 3.4 (registry mutex chết). | 03 §2.8 + §3.4 | 4 | Resolved in OI-07 implementation: `ShmMutexGuard` RAII không auto-consistent; Registry recovery sửa bitmap/slot/count trước `guard.mark_consistent()`. |
| ~~OI-08~~ ✅ | 🟢 | ABI lock-in: không port musl / Alpine / 32-bit / Windows | 03 §2.9 | 9 README | Resolved in Phase 9 README: documented Linux/glibc target and no musl/Alpine/32-bit/Windows support in v0.1. Static asserts still catch unsupported ABI at compile time. |
| ~~OI-09~~ ✅ | 🟡 | Owner spawn 1 thread heartbeat / segment → N segment = N thread | 03 §3.1 | 4 hoặc 7 | Resolved in Phase 4: process-wide heartbeat worker tracks all owner `ShmHeader*` and ticks them from one shared thread. |
| ~~OI-10~~ ✅ | 🟡 | Ownership transfer cần RW attach → consumer co-owner phải RW thay vì RO | 03 §3.2 | 8 (logging app) + framework docs | Resolved in Phase 8: `LogCenter` attaches producer rings with `AttachMode::ReadWrite`, so it bumps `ref_count` and can act as lifecycle co-owner while draining. |
| ~~OI-11~~ ✅ | 🟡 | Janitor chưa có thuật toán force-unlink "zombie" — `SEGMENT_ZOMBIE_NS` đã định nhưng pseudocode `01_DESIGN §6.4` chưa cover | 03 §3.3 | 4 | Resolved in Phase 4: `Registry::gc()` marks dead-owner segments DRAINING, unlinks dead `rc == 0`, and force-unlinks old DRAINING zombies only when `/proc/*/maps` has no live mapping. |
| OI-12 | 🟢 | `MAX_REGISTRY_ENTRIES = 1024` là hard cap compile-time | 03 §3.6 | 3 nếu user cần | Thêm `RegistryPayload::version`; v1 = 1024, v2 = 8192. `Registry::open_or_create()` chọn v lớn nhất compatible với code. Với OI-04 đã chọn MPSC, segment count giảm về xấp xỉ số process nên 1024 đủ cho v0.1 mặc định. |
| ~~OI-13~~ ✅ | 🟢 | Cache line 64B hardcoded — sai trên Apple M-series (128B) | 03 §3.7 | 6 (data plane) | Resolved in Phase 6: hot ring cursor alignment dùng `SHMFX_CACHE_LINE_BYTES`/`CACHE_LINE_SIZE` từ `std::hardware_destructive_interference_size` khi toolchain hỗ trợ, fallback 64. |
| OI-14 | 🟢 | Container CLOCK_BOOTTIME khác host — `start_time` mismatch nếu `/proc` cross-namespace | 03 §2.2(4) | Future (Q8) | Phase 4 giữ fallback `kill(pid,0) + /proc/<pid>/stat` đúng design. `pidfd_open()` vẫn là future opt-in vì ABI hiện không lưu pidfd. |

---

## Decision Recorded

**OI-04 đã resolve**: v0.1 cần **MPSC ngay** trong core framework. Phase 6 (`shm_ring`) phải deliver generic `MpscRing` production-ready.

> **Trong v0.1, mỗi process producer thường có:**
>
> **16+ thread** (worker pool / async runtime / one-thread-per-request).

Hệ quả thiết kế:

- Không dùng TLS ring per-thread làm path chính vì sẽ phình segment count và polling cost.
- Mỗi producer process/service/channel dùng một record ring MPSC; nhiều thread trong process push vào cùng ring.
- `MAX_REGISTRY_ENTRIES = 1024` chưa cần bump ngay vì segment count scale theo process thay vì thread.
- Phase 6 implement MPSC trong core; Phase 7 logging `Logger` dùng MPSC mặc định.

---

## Phase mapping

Các OI fold sẵn vào phase tương ứng, không cần thêm design doc trung gian:

| Phase | OI items |
|-------|----------|
| **2** (`shm_header.h` + `shm_types.h`) | (none — design đã ổn định) |
| **3** (`shm_manager` + `shm_registry`) | OI-03 (seqlock retry bound), OI-12 (registry v2) nếu user yêu cầu |
| **4** (lifecycle + crash recovery) | ~~OI-07 `ShmMutexGuard`~~ ✅, ~~OI-09 heartbeat sharing~~ ✅, ~~OI-11 zombie algorithm~~ ✅, OI-14 pidfd opt-in deferred |
| **6** (ring) | Generic MPSC record ring theo OI-04, ~~OI-13 cache line constant~~ ✅ |
| **8** (logging app / consumers) | ~~OI-02 self-validate~~ ✅, ~~OI-05 polling tuning~~ ✅, ~~OI-10 co-owner semantic~~ ✅ |
| **9** (build + README + demo) | ~~OI-01 README HMAC limitation~~ ✅, ~~OI-02 stale RO mapping note~~ ✅, ~~OI-05 adaptive polling tradeoff~~ ✅, ~~OI-08 platform ABI warning~~ ✅ |

---

## Quy tắc maintain

1. **Khi resolve một OI**: gạch (`~~OI-XX~~ ✅`) trong bảng + ghi "Resolved in <phase/commit>" cuối row Resolution.
2. **Khi phát hiện open issue mới**: thêm OI-(N+1) ở *cuối* bảng, **không** re-number.
3. **02 và 03 frozen** — không sửa nữa, chỉ link tham chiếu.
4. Khi PLAN.md chuyển sang phase mới, kiểm bảng "Phase mapping" để biết OI nào phải address trong phase đó.
