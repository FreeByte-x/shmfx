# 01 - Design Document: SharedMemory Framework Core (`shmfx`)

> Version: 0.5. Core `libshmfx` is a reusable shared-memory framework; application-specific workflows live outside the core. Technical lock-in decisions are tracked in [`PLAN.md`](PLAN.md).

## 1. Goals & Scope

`shmfx` is a C++20 framework for Linux shared memory. It provides:

1. A standard 256-byte segment header so tools and processes can inspect shared-memory objects consistently.
2. Safe lifecycle management for create, attach, detach, destroy, crash recovery, and cleanup.
3. Discovery through a singleton registry segment.
4. Shared-memory concurrency primitives: robust process-shared mutexes and a generic lock-free MPSC record ring.
5. Basic security through POSIX permissions, namespace validation, DoS guards, and optional immutable-header HMAC.
6. A clear core/app boundary: logging, telemetry, KV cache, and similar workflows use `libshmfx` but do not belong inside it.

Non-goals:

- Cross-host transport such as multicast or RDMA.
- General-purpose serialization/schema support.
- Portable v0.1 support for Windows, musl/Alpine, or 32-bit Linux.
- Strong capability security or mTLS. HMAC is an integrity check, not a full same-UID attacker defense.

## 2. Use Cases & Requirements

| Use case | Description |
|----------|-------------|
| UC1 | N producer processes to one consumer through MPSC record rings. |
| UC2 | One producer to many consumers for future broadcast telemetry. |
| UC3 | Shared KV/cache data between worker processes. |
| UC4 | Tooling such as `shmfxctl ls/inspect` over registry and headers. |
| UC5 | Distributed Logging reference app, described in [`03_LOGGING_APP.md`](03_LOGGING_APP.md). |

Functional requirements:

- Create a segment from `(name, size, type, permissions)`.
- Attach by name and validate magic, version, layout, and HMAC when enabled.
- Manage RW reference counting through RAII handles.
- Maintain owner heartbeat and owner identity with PID start time to reduce PID-reuse mistakes.
- Recover or clean dead segments after process crashes.
- Provide a generic non-blocking MPSC record ring.
- Provide robust mutex wrappers with an explicit recovery contract.
- Keep app payload semantics outside the core ABI.

Core performance targets:

| Bucket | Definition | Target |
|--------|------------|--------|
| Raw ring push | `MpscRing::try_push(64B)` on a hot uncontended ring | p50 < 150 ns, p99 < 1 us |
| Framework wrapper | Validate + push 64B record | p50 < 250 ns, p99 < 2 us |
| Consumer drain | One record visible in caller buffer | p50 < 100 ns/record |

## 3. Architecture

```text
Process A
  App code
    |
    v
  ShmManager / ShmHandle
    |
    v mmap
/dev/shm/shmfx.<namespace>.<name>
  [ShmHeader 256B]
  [user metadata]
  [payload]

/dev/shm/shmfx.registry
  Registry entries for discovery and janitor cleanup
```

Core modules:

- `shm_manager`: create, attach, detach, destroy, and RAII mapping.
- `shm_registry`: singleton registry and lazy janitor.
- `shm_lifecycle`: owner heartbeat, liveness checks, and crash cleanup.
- `shm_security`: name validation, namespace policy, permissions, and HMAC.
- `shm_ring`: generic MPSC record ring.
- `shm_sync`: robust mutex guard and low-level synchronization helpers.

Reference apps live outside these modules. Distributed Logging is under `apps/logging/`.

## 4. Memory Layout

Every segment uses:

```text
[ShmHeader 256B][user metadata meta_size][payload bytes]
```

`ShmHeader` is split into:

- Immutable block covered by optional HMAC: magic, version, type, flags, sizes, creator identity, and name.
- Mutable runtime block outside HMAC: owner PID/start time, refcount, state, heartbeat.
- Control mutex block: robust pthread mutex for lifecycle/control-plane transitions.

Inter-process atomic fields are stored as plain POD integers and accessed through `std::atomic_ref<T>`. This keeps shared-memory structs trivially copyable and avoids relying on `std::atomic<T>` object semantics across processes.

Record-ring metadata lives in the segment metadata region. Ring data lives in the payload region.

## 5. Naming & Discovery

User segment names follow:

```text
/shmfx.<namespace>.<name>
```

Default namespaces are `log`, `tlm`, `kv`, `app`, and `sys`. The registry bootstrap segment `/shmfx.registry` is a special case.

The registry stores fixed-capacity entries and is read with per-slot seqlock snapshots. `Registry::list(prefix)` returns consistent snapshots and may fall back to the registry control mutex if a writer is too hot.

## 6. Lifecycle Management

State transitions:

```text
Created -> Active -> Draining -> Dead
```

`ShmManager::create()` initializes the object, writes the header, initializes metadata/payload as requested, publishes the registry entry, and returns an RW owner handle.

`ShmManager::attach()` opens an existing object, validates the header, maps it, optionally increments `ref_count` for RW handles, and re-validates state after the increment to avoid attach/unlink races.

`ShmHandle` is move-only RAII. Destroying an RW handle decrements `ref_count`; destroying an RO handle only unmaps.

Owner liveness uses:

- `owner_pid`
- `owner_start_time` from `/proc/<pid>/stat` field 22
- `heartbeat_last_ns`

The janitor runs lazily from operations such as attach/list and may also run as a future daemon. It unregisters stale entries, marks dead-owner segments as draining, and force-unlinks old zombies only after process-map scanning confirms no live mapping remains.

## 7. Concurrency Model

Control plane:

- Uses `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST`.
- If lock returns prior-owner-dead, the caller must recover protected state first, then call `mark_consistent()`.
- If recovery is impossible, do not mark consistent; later waiters should observe unrecoverable state.

Data plane:

- Uses a generic bounded MPSC ring with per-slot sequence counters.
- Multiple producers claim slots through CAS on `tail`.
- A single consumer drains from `head`.
- Producers never block. Full or highly contended rings return `RingFull` and increment `lost_count`.

Record wire format:

```text
[u32 len][u16 type][u16 flags][u64 user][payload]
```

The core reserves `RECORD_FLAG_TRUNCATED`; other semantics belong to the app.

## 8. Security

- Segment creation uses restrictive permissions first, then `fchmod()` to the requested mode.
- HMAC-SHA256 is optional and covers only immutable header bytes.
- Mutable fields such as refcount, lifecycle state, heartbeat, and robust mutex bytes are not MACed.
- HMAC keys are read from `SHMFX_HMAC_KEY` or `/etc/shmfx/hmac.key`.
- Namespace policy limits valid names and enables future quotas.

## 9. API Surface

Primary public APIs:

- `ShmManager::create(const CreateOptions&)`
- `ShmManager::attach(std::string_view, AttachMode)`
- `ShmManager::destroy(std::string_view)`
- `ShmHandle::header()`, `metadata()`, `payload()`, `name()`, `mode()`
- `Registry::instance().list(prefix)`
- `Registry::instance().gc()`
- `MpscRing::initialize(...)`
- `MpscRing::bind(...)`
- `MpscRing::try_push(...)`
- `MpscRing::try_pop(...)`
- `ShmMutexGuard`

App APIs such as `Logger` and `LogCenter` are not part of `include/shmfx/`.

## 10. Comparison

| System | Why not use it directly |
|--------|--------------------------|
| POSIX shm raw | Too low-level; no standard header, discovery, or recovery policy. |
| Boost.Interprocess | Heavier dependency and not aligned with this small C ABI-like layout. |
| Cap'n Proto / FlatBuffers | Serialization format, not shared-memory lifecycle/discovery. |
| iceoryx | Powerful but daemon-based and heavier than this embedded framework target. |
| ROS 2 DDS | Too large and lifecycle-heavy for non-ROS applications. |
| DPDK rings/memzones | Excellent performance but require DPDK/EAL assumptions and often hugepages/root setup. |

`shmfx` targets the middle ground: lighter than daemon-centric IPC stacks, safer and more reusable than raw `shm_open`.

## 11. Reference Apps

Core `libshmfx` provides lifecycle, registry, security, sync, and generic record rings. Workflow packages live separately:

| App | Path | Design |
|-----|------|--------|
| Distributed Logging | `apps/logging/` | [`03_LOGGING_APP.md`](03_LOGGING_APP.md) |

Rules:

- Apps may depend on `include/shmfx/*`.
- Core headers and sources must not include app headers.
- App metadata/payload semantics must be documented in app-specific docs/headers.

## 12. Appendix

Important defaults:

- Header size: 256 bytes.
- Registry name: `/shmfx.registry`.
- User name format: `/shmfx.<namespace>.<name>`.
- Record header size: 16 bytes.
- Ring producer model: MPSC.
- Ring consumer model: single consumer.
- Logging consumer idle sleep default: 200 us.
