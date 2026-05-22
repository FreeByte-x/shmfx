# 02 — Design Review: SharedMemory Framework (`shmfx`) + Distributed Logging

> **🧊 FROZEN** sau v0.3. Toàn bộ 14/14 items review này đã được fold vào `01_DESIGN.md` v0.2 hoặc v0.3.
> Active backlog: **[`04_OPEN_ISSUES.md`](04_OPEN_ISSUES.md)**. File này giữ làm audit trail review v0.1.

> Đánh giá dựa trên `docs/01_DESIGN.md` và `PLAN.md`.

## Đánh Giá Tổng Quan

Thiết kế có nền tảng tốt: mục tiêu rõ, scope được khóa trong `PLAN.md`, layout được đặc tả theo offset, có lifecycle, registry, crash recovery, security và logging reference app. Với một Phase 1 design document, mức chi tiết hiện tại đủ để bắt đầu Phase 2.

Tuy nhiên, trước khi code, nên chỉnh vài điểm vì hiện có một số giả định dễ gây lỗi ABI, race condition hoặc mục tiêu performance không thực tế.

Ghi chú cập nhật v0.3: `docs/01_DESIGN.md` hiện là phiên bản 0.3 và đã incorporate thêm một số điểm từ `docs/03_DESIGN_REVIEW.md`: chuyển `std::atomic<T>` member sang POD storage + `std::atomic_ref<T>`, đổi `*_start_time` sang `uint64_t`, sửa lại perf bucket và siết regex name. Các mục đã được xử lý được gạch ngang bên dưới. Các nhận xét mới theo v0.3 nằm ở mục **Review Lại v0.3**.

## Ưu Điểm

- Scope rõ ràng: Linux/POSIX shm, C++20, không cố làm cross-platform hay serialization framework.
- Layout header 256B có offset cụ thể, giúp tooling inspect dễ hơn.
- Tách control plane và data plane hợp lý: robust mutex cho lifecycle/registry, lock-free ring cho hot path.
- Có registry discovery, janitor, heartbeat, quota, naming rule, error code và tunables.
- So sánh với Boost.Interprocess, iceoryx, DDS, DPDK khá đúng định vị: `shmfx` nằm giữa raw POSIX shm và hệ IPC lớn.
- Logging app được thiết kế thực dụng: mỗi service một ring riêng, `log_center` drain tập trung, rotation có size/time/SIGHUP.

## Review Ban Đầu v0.1

### Nhược Điểm / Rủi Ro Chính

- ~~HMAC hiện tính trên `header[0..160)`, nhưng vùng này chứa các field mutable như `owner_pid`, `ref_count`, `state`, `heartbeat_*`. Mỗi lần heartbeat/refcount đổi thì HMAC sẽ invalid. Nên chỉ HMAC phần immutable, hoặc tách `static_hmac` và dynamic runtime fields.~~ Đã xử lý trong v0.2 bằng immutable block `[0, 128)` + HMAC field riêng.
- ~~`pthread_mutex_t` bị giả định size 40B. Điều này đúng với một số ABI glibc x86_64, nhưng không phải contract portable. Nếu vẫn lock Linux/glibc thì cần ghi rõ ABI target và `static_assert(sizeof(pthread_mutex_t) <= 40)`.~~ Đã xử lý bằng ABI lock-in Linux/glibc x86_64/aarch64 và static assert.
- ~~`std::atomic<T>` trong shared memory không được C++ standard bảo đảm cho inter-process, dù thường hoạt động trên Linux với lock-free atomics. Thiết kế nên nói rõ đây là Linux ABI assumption, có `static_assert(is_always_lock_free)`, hoặc dùng GCC/Clang atomic builtins trên storage integer.~~ Đã xử lý tốt hơn trong v0.3: struct dùng POD storage và truy cập đồng bộ qua `std::atomic_ref<T>`, kèm `is_trivially_copyable` và static assert.
- ~~Refcount crash recovery chưa đủ chặt. Nếu process crash sau attach, janitor không thể biết chính xác refcount nào thuộc PID nào. Pseudocode janitor attach rồi kiểm `ref_count == 0` cũng tự làm tăng refcount, nên điều kiện này khó đúng. Cần per-process lease/attach table hoặc chấp nhận refcount chỉ là heuristic.~~ Đã xử lý một phần: janitor dùng RO attach không bump refcount và ghi rõ hạn chế refcount rò. Còn vấn đề mới liên quan RO/SPSC, xem Review Lại v0.2.
- ~~`kill(owner_pid, 0)` có rủi ro PID reuse. Nên lưu thêm process start time từ `/proc/<pid>/stat`, hoặc dùng `pidfd` khi có thể.~~ Đã thêm `creator_start_time` / `owner_start_time`; `pidfd` để future.
- ~~Registry snapshot không lock có thể đọc entry đang ghi dở. Nên dùng seqlock/version per slot, atomic state per slot, hoặc lock read path.~~ Đã chuyển sang seqlock per-slot.
- ~~Registry struct dùng `capacity` làm kích thước array trong struct, không hợp lệ với runtime capacity trong C++. Nên dùng fixed max capacity hoặc tính offset thủ công trên payload.~~ Đã đổi sang `MAX_REGISTRY_ENTRIES = 1024`.
- ~~Eventfd sharing chưa có protocol rõ. `eventfd` là fd-local; `pidfd_getfd` có permission/ràng buộc kernel, UDS thì cần control channel. Nếu muốn đơn giản, Phase 1 nên chọn rõ UDS broker, polling registry, hoặc futex-based wake.~~ Đã chọn adaptive polling cho v0.1; eventfd + UDS chuyển thành future.
- ~~Record format đặt `u64 ts_ns` sau `u32 len`, gây misalignment. Nên đổi thành ví dụ `u32 len`, `u16 level`, `u16 flags`, `u64 ts_ns`, rồi payload.~~ Đã reorder record header 16B.
- ~~Mục tiêu `LOG_INFO` p50 `< 500 ns` không thực tế nếu bao gồm `vsnprintf`, timestamp và thỉnh thoảng `eventfd_write`. Nên tách benchmark: ring push raw, formatted log without wake, full producer path.~~ Đã tách benchmark bucket.
- ~~`pthread_mutex_consistent()` đang được gọi quá sớm trong wrapper minh họa. Đúng hơn là caller phải recover state xong rồi mới mark consistent.~~ Đã sửa thứ tự recover rồi mới `pthread_mutex_consistent()`.
- ~~Security phần `umask` nguy hiểm trong process đa luồng vì `umask` là process-global. Nên tránh đổi umask runtime hoặc bọc rất cẩn thận; tốt hơn tạo restrictive rồi `fchmod`.~~ Đã bỏ `umask`; còn một nhận xét mới về mode tạo file, xem Review Lại v0.2.
- ~~Naming có mâu thuẫn nhỏ: namespace mô tả kebab/snake nhưng regex chỉ cho hyphen, không cho underscore. `/shmfx.registry` cũng là special case không match rule chung.~~ Đã cho `_` trong regex và khai báo special-case bootstrap name.

## Đề Xuất Cải Tiến

### 1. Chốt Lại ABI Cho Phase 2

- ~~Linux + glibc + x86_64/aarch64.~~
- ~~`static_assert(sizeof(ShmHeader) == 256)`.~~
- ~~`static_assert(alignof(ShmHeader) >= 8)`.~~
- ~~`static_assert(std::atomic<uint64_t>::is_always_lock_free)`.~~

### 2. Tách Header Thành Hai Vùng

- ~~Immutable: magic, version, size, offsets, type, name, created_at.~~
- ~~Mutable: owner, refcount, state, heartbeat.~~

~~HMAC chỉ nên phủ immutable header, hoặc thêm generation counter nếu muốn bảo vệ mutable control state.~~

### 3. Sửa Lifecycle / Refcount

- Thêm attach lease table theo PID trong registry hoặc segment metadata. Chưa làm; v0.2 chọn heuristic + zombie timeout.
- ~~Lưu `pid + start_time` để chống PID reuse.~~
- ~~Janitor dùng attach mode nội bộ không tăng refcount, hoặc map header read-only để kiểm tra.~~

### 4. Làm Registry An Toàn Hơn

- ~~Mỗi slot có `atomic<uint32_t> seq`.~~
- ~~Writer: seq odd -> ghi fields -> seq even.~~
- ~~Reader retry nếu seq đổi hoặc odd.~~

~~Cách này vẫn giữ read path không lock nhưng tránh snapshot rách.~~

### 5. Giảm Scope Ring Buffer V0.1

- ~~Implement SPSC trước cho logging mỗi process một ring.~~
- ~~MPSC để Phase sau nếu cần multi-thread producer thật.~~
- ~~SPMC broadcast nên tách khỏi v0.1 nếu mục tiêu chính là distributed logging.~~

### 6. Chỉnh API Cho C++20

- ~~Giữ `std::span<std::byte>` cho `metadata()` và `payload()` vì dự án đã chấp nhận C++20.~~
- ~~Cập nhật `PLAN.md` và `docs/01_DESIGN.md`: `C++ standard` từ C++17 sang C++20.~~
- ~~Tránh `fmt`-style API nếu dependency-free; dùng `printf`-style rõ ràng hoặc cung cấp callback formatter.~~

### 7. Cập Nhật Performance Targets

- ~~`ring.try_push` raw: mục tiêu sub-microsecond.~~
- ~~`Logger::log` formatted: p50/p99 riêng.~~
- ~~`eventfd_write` batched: benchmark riêng.~~ Đã thay bằng full-path/adaptive polling bucket.

~~Điều này giúp test Phase 10 không thất bại vì target sai phạm vi.~~

### 8. Làm Rõ Wake-Up Mechanism

- ~~Nếu dùng `eventfd`, cần UDS control socket để producer/log_center trao đổi fd.~~
- ~~Nếu không muốn control socket, dùng polling + adaptive sleep cho v0.1, rồi tối ưu sau.~~

## Review Lại v0.2

### Đã Khắc Phục Tốt

- C++20 đã được áp dụng trong `docs/01_DESIGN.md` và `PLAN.md`.
- HMAC mutable-field bug đã được sửa bằng immutable block `[0, 128)`.
- ABI assumptions đã rõ hơn và có static assert cho Phase 2.
- PID reuse được xử lý bằng `pid + start_time`.
- Registry đã chuyển sang fixed capacity + seqlock per-slot.
- Record header đã align lại thành `[u32 len][u16 level][u16 flags][u64 ts_ns]`.
- Wake-up v0.1 đã đổi sang adaptive polling, tránh kéo UDS/eventfd vào scope sớm.
- `umask` và `pthread_mutex_consistent()` đã được chỉnh theo hướng an toàn hơn.

### Nhận Xét Mới / Blocker Trước Phase 2

- **Consumer RO mâu thuẫn với SPSC**: `LogCenter` attach `ReadOnly`, nhưng SPSC consumer phải cập nhật `head` trong shm. RO mapping không thể `try_pop()` nếu `head` nằm trong shared metadata. Cần đổi consumer sang RW không-producer, hoặc tách `writer_ref_count` / `reader_ref_count`.
- **`LogRingMeta` mâu thuẫn kích thước**: §4.2 vẫn nói `LogRingMeta` 64B, nhưng §7.2 thêm `head` và `tail` mỗi cái 64B. Cần viết lại layout ring metadata, ví dụ `LogRingMeta` 192B hoặc tách `RingControl` vào payload. Cập nhật v0.3: vấn đề đã nhẹ hơn vì `RingCursor` không còn nói là member của `LogRingMeta`, nhưng layout thực tế của `head/tail` nằm ở đâu vẫn chưa được mô tả đủ.
- **`PLAN.md §4` chưa đồng bộ hết**: vẫn còn `SPMC/MPSC ring`, payload format cũ `[u32 len][u64 ts][u8 level]`, và `epoll + futex wake`. Các dòng này nên đổi theo v0.2 trước khi code.
- **`docs/01_DESIGN.md` còn sót text cũ**: mục tiêu chính vẫn nói SPMC/MPSC; DoS table vẫn nói SPMC slow consumer; sơ đồ logging vẫn ghi `epoll(eventfd_per_ring)`; tunables vẫn có `RING_WAKE_BATCH` / `RING_WAKE_MAX_DELAY_US`.
- ~~**Registry seqlock pseudocode không compile trực tiếp**: `RegistryEntry snap = slot` không hợp lệ vì struct chứa `std::atomic`. Nên copy từng field thường, hoặc dùng raw storage + atomic builtin.~~ Đã xử lý trong v0.3 bằng POD storage + `std::atomic_ref<uint32_t>` trên `seq_storage`; `RegistryEntry snap = slot` hợp lệ.
- ~~**`owner_start_time` nên là `uint64_t` hoặc composite atomic**: `/proc/<pid>/stat:starttime` nên lưu 64-bit; cặp `owner_pid` atomic + `owner_start_time` plain read có thể đọc lệch. Nên dùng `atomic<uint64_t> owner_identity = (start_time << 32) | pid` nếu chấp nhận start_time 32-bit, hoặc hai atomic với ordering rõ.~~ Đã xử lý trong v0.3: `owner_start_time` là `uint64_t`, writer ghi start_time trước rồi store `owner_pid` release; reader load `owner_pid` acquire rồi đọc start_time.
- **`shm_open(..., mode=0)` có rủi ro zombie permission**: nếu process crash trước `fchmod`, object mode `000` có thể làm janitor thường không mở được. Nên tạo với `0600`, sau đó `fchmod(fd, perm)`; cửa sổ ban đầu chỉ hẹp hơn yêu cầu, không rộng hơn.
- **API ReadOnly vẫn trả mutable view**: `ShmHandle::header()` trả `ShmHeader&`, `metadata()` / `payload()` trả `std::span<std::byte>`. Với RO handle nên trả `const ShmHeader&` và `std::span<const std::byte>`, hoặc tách `ReadOnlyShmHandle` / `ReadWriteShmHandle`.

### Hạng Mục Còn Mở

- Refcount crash recovery vẫn là heuristic, chưa có attach lease table. Chấp nhận được nếu được ghi rõ là Phase 4 sẽ quyết định policy force-unlink.
- Cần cập nhật tunables cho adaptive polling, ví dụ `POLL_SPIN_LIMIT`, `POLL_YIELD_LIMIT`, `POLL_SLEEP_US`, thay cho `RING_WAKE_*`.
- Nếu SPSC-only là lock-in v0.1, các use case UC2/SPMC nên ghi rõ là future hoặc non-blocking placeholder.

## Kết Luận v0.2

Thiết kế v0.2 đã xử lý phần lớn review ban đầu. Chưa nên sang Phase 2 ngay cho tới khi sửa 2 blocker: RO consumer vs SPSC head update, và layout `LogRingMeta`/ring cursor. Sau đó cần đồng bộ `PLAN.md` với quyết định v0.2 để các phase code không đi theo lock-in cũ.

## Review Lại v0.3

### Đã Khắc Phục Tốt Trong v0.3

- `std::atomic<T>` member đã được thay bằng POD storage + `std::atomic_ref<T>`, giúp `ShmHeader`, `RegistryEntry`, `RegistryPayload`, `LogRingMeta`, `RingCursor` trivially copyable hơn và tránh lỗi copy/snapshot.
- `RegistryEntry snap = slot` hiện hợp lệ vì `RegistryEntry` không còn chứa `std::atomic<T>` member.
- `creator_start_time` và `owner_start_time` đã đổi sang `uint64_t`, tránh overflow với uptime dài.
- Header layout đã được cập nhật lại rõ ràng: HMAC `[128,160)`, mutable state `[160,200)`, mutex `[200,240)`, padding `[240,256)`.
- Perf targets đã được chia thành 4 bucket rõ hơn: raw ring push, logger producer, consumer drain, end-to-end visible.
- Naming regex đã siết lại để bảo đảm fit `name[64]`.

### Còn Chưa Khắc Phục / Blocker

- **Consumer RO vs SPSC vẫn là blocker**: v0.3 vẫn ghi `log_center` RO-attach và `ShmManager::attach(name, ReadOnly)` cho consumer, trong khi SPSC consumer cần cập nhật `head`. Ownership transfer có nhắc RO re-attach RW, nhưng đó chỉ cho case set state/owner, chưa giải quyết `try_pop()` bình thường.
- **Ring cursor layout vẫn thiếu**: v0.3 giữ `LogRingMeta` 64B và định nghĩa `RingCursor head/tail`, nhưng chưa chỉ rõ `head/tail` nằm ở metadata, đầu payload, hay một `RingControl` riêng. Payload `LOG_RING` vẫn mô tả chỉ gồm slots, nên Phase 6 sẽ thiếu layout để implement.
- **`PLAN.md §4` vẫn chưa đồng bộ với v0.3**: còn `SPMC/MPSC ring`, record format cũ `[u32 len][u64 ts][u8 level]`, và `epoll + futex wake`.
- **`docs/01_DESIGN.md` vẫn còn text cũ**: mục tiêu chính còn `SPMC/MPSC ring`; DoS table còn SPMC slow consumer; sơ đồ logging còn `epoll(eventfd_per_ring)`; tunables còn `RING_WAKE_BATCH` / `RING_WAKE_MAX_DELAY_US`.
- **`shm_open(..., mode=0)` vẫn chưa sửa**: tạo shm mode `000` rồi `fchmod` có rủi ro để lại object không mở được nếu crash giữa chừng. Nên dùng `0600` khi create, sau đó `fchmod(fd, perm)`.
- **API ReadOnly vẫn mutable**: `ShmHandle::header()`, `metadata()`, `payload()` vẫn trả mutable reference/span dù `AttachMode::ReadOnly` tồn tại. Cần overload const theo mode, hoặc tách handle RO/RW.
- **HMAC immutable-only là trade-off còn mở**: v0.3 ghi rõ phần trade-off HMAC mutable chưa apply. Nếu threat model cần phát hiện tamper runtime state, cần thêm generation/MAC riêng hoặc ghi rõ không bảo vệ mutable state.

### Hạng Mục Có Thể Để Phase Sau Nếu Ghi Rõ

- Refcount crash recovery vẫn là heuristic, chưa có attach lease table. Có thể chấp nhận cho v0.1 nếu Phase 4 document rõ force-unlink policy.
- SPMC/UC2 telemetry và MPSC nên được đánh dấu future trong use cases/non-goals nếu SPSC-only là scope v0.1.
- Eventfd + UDS broker có thể giữ ở open question, miễn là mọi sơ đồ/tunables v0.1 đã thống nhất với adaptive polling.

## Kết Luận v0.3

v0.3 đã cải thiện đáng kể phần ABI/layout và snapshot safety. Hai blocker lớn trước Phase 2 vẫn là **RO consumer không tương thích với SPSC head update** và **layout ring cursor chưa được chốt**. Ngoài ra cần đồng bộ `PLAN.md` và quét lại các dòng cũ về eventfd/SPMC/tunables để tài liệu lock-in không mâu thuẫn với thiết kế v0.3.
