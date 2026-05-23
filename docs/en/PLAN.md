# PLAN: SharedMemory Framework (`shmfx`) + Reference Apps

> Goal: build `shmfx` as a reusable shared-memory framework. Distributed Logging is a separate reference app/package and must not shape the core API.

---

## 1. Work Overview

| # | Phase | Deliverable | Estimate | Can split session? |
|---|-------|-------------|----------|--------------------|
| 1 | Core Design Document | `docs/en/01_DESIGN.md` + `docs/en/03_LOGGING_APP.md` | 1 pass | yes |
| 2 | Framework Core - Header & Layout | `shm_header.h`, `shm_types.h` | 1 pass | yes |
| 3 | Framework Core - Manager & Registry | `shm_manager.{h,cpp}`, `shm_registry.{h,cpp}` | 1 pass | yes |
| 4 | Framework Core - Lifecycle & Crash Recovery | `shm_lifecycle.{h,cpp}` | 1 pass | yes |
| 5 | Framework Core - Security | `shm_security.{h,cpp}` | 0.5 pass | merged with 4 |
| 6 | Framework Core - Sync primitives | `shm_ring.{h,cpp}`, `shm_sync.{h,cpp}` | 1 pass | yes |
| 7 | Reference App - Logging Producer API | `apps/logging/include/shmfx_logging/logger.h`, `apps/logging/src/logger.cpp` | 0.5 pass | merged with 8 |
| 8 | Reference App - Logging Center | `apps/logging/src/log_center.cpp` | 1 pass | yes |
| 9 | Build system + framework README + demos | `CMakeLists.txt`, `README.md`, `examples/`, `apps/logging/examples/` | 1 pass | yes |
| 10 | Optional Integration / Stress / Benchmark | `tests/`, `benchmark/` | 1 pass | can cut |

Total: about 7-8 focused sessions. Each phase is designed to stand on its own.

---

## 2. Token-Safe Working Rules

1. Do one phase per session unless two phases are explicitly marked as small/merged.
2. Write code and docs to files in this repo; keep chat summaries short.
3. At the end of each phase, update the checkpoint in section 6.
4. To resume in a new session, ask: "Read `/apps/source/sharemem/docs/en/PLAN.md`, continue Phase N."
5. Cut priority if needed: Phase 10 tests, then Phase 9 secondary demos, then future/nice-to-have features.

---

## 2.1 Coding Convention

1. Public or subtle code must include comments explaining contracts, ABI, concurrency, ownership, or design rationale.
2. Every public header declaration/definition must have Doxygen comments.
3. Public enums, structs, constants, and shared-memory layout fields should document their semantics.
4. Public headers should not expose uncommented functions, including small `constexpr` or `inline` helpers.
5. From Phase 3 onward, production code phases should include focused unit tests for the main behavior.

---

## 3. Target Repository Layout

```text
/apps/source/sharemem/
├── docs/
│   ├── en/
│   │   ├── PLAN.md
│   │   ├── 01_DESIGN.md
│   │   ├── 02_OPEN_ISSUES.md
│   │   ├── 03_LOGGING_APP.md
│   │   └── 04_USAGE.md
│   └── vi/
│       ├── PLAN.md
│       ├── 01_DESIGN.md
│       ├── 02_OPEN_ISSUES.md
│       ├── 03_LOGGING_APP.md
│       └── 04_USAGE.md
├── include/shmfx/
├── src/
├── apps/logging/
├── examples/
├── tests/
├── benchmark/
├── CMakeLists.txt
└── README.md
```

---

## 4. Locked Technical Decisions

| Area | Decision |
|------|----------|
| Target platform | Linux with POSIX `shm_open` + `mmap`; Windows is design-only. |
| Language | C++20; `std::span`, `<concepts>`, and `<bit>` are allowed. |
| Naming | `/shmfx.<namespace>.<name>`; `log` is only one namespace consumer. |
| Magic | `0x53484D46` (`SHMF` ASCII). |
| Version | 16-bit major + 16-bit minor. |
| Header size | Fixed 256-byte `ShmHeader`. |
| Layout | `[ShmHeader 256B][user metadata N B][payload...]`. |
| Concurrency | Robust pthread mutex for control plane + generic lock-free MPSC record ring for data plane. |
| Crash recovery | Heartbeat + reference count + lazy/daemon janitor. |
| Security | UNIX permissions + optional immutable-header HMAC + namespace validation. |
| Discovery | Singleton registry segment named `/shmfx.registry`. |
| Record ring | Generic records: `[u32 len][u16 type][u16 flags][u64 ts_or_user][payload]`. |
| Reference apps | Logging lives in `apps/logging`, builds optionally, and must not create reverse dependencies into `libshmfx`. |

Changes to this table must be made here before implementation.

---

## 5. Phase 1 Design Outline

```text
01_DESIGN.md
1. Goals & Scope
2. Use cases & Requirements
3. Architecture
4. Memory Layout
5. Naming & Discovery
6. Lifecycle Management
7. Concurrency Model
8. Security
9. API Surface
10. Comparison with existing systems
11. Reference Apps
12. Appendix: error codes, sizing, tunables
```

Distributed Logging design: `docs/en/03_LOGGING_APP.md`.

---

## 6. Checkpoint

- [x] Phase 1 - Core design + logging app design
- [x] Phase 2 - Header & types
- [x] Phase 3 - Manager & Registry
- [x] Phase 4 - Lifecycle & Crash Recovery
- [x] Phase 5 - Security
- [x] Phase 6 - Ring buffer & sync
- [x] Phase 7 - Logging producer package
- [x] Phase 8 - Logging center package
- [x] Phase 9 - Build + demos + README
- [x] Phase 10 - Integration tests, stress tests, benchmarks

---

## 7. Resume Commands

- "Continue Phase 1" means read this plan and write/update the design docs.
- "Continue Phase N, completed through Phase M" means inspect the repo and proceed.
- "Show PLAN" means summarize sections 1 and 6.
- "Cut scope: remove phase X, Y" means update sections 1 and 6.
