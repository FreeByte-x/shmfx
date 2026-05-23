# 02 - Open Issues Backlog

> Single source of truth for issues that remain after the v0.1-v0.3 design reviews. Detailed review files were folded into the design/backlog and removed; git history keeps the original audit trail.

## Status After v0.3

| Source | Resolved | Open |
|--------|----------|------|
| Review v0.1 | 14/14 resolved and folded into v0.2/v0.3 | 0 |
| Review v0.2 | 12/19 resolved | 7 |
| Total open | | 7 |

## Backlog

Priority legend:

- Blocker: blocks the next phase or requires a user decision.
- High: should be wired early to avoid rework.
- Medium: normal phase work.
- Low: acceptable with documentation or future work.

| ID | Priority | Title | Source | Phase | Resolution path |
|----|----------|-------|--------|-------|-----------------|
| ~~OI-01~~ | Low | HMAC does not cover mutable state; same-UID mutable-state DoS remains possible. | Review v0.2 | 9 | Resolved in README limitations. |
| ~~OI-02~~ | Medium | RO consumers can keep stale mappings after `shm_unlink`. | Review v0.2 | 8/9 | Resolved by Logging `LogCenter` RW attach + self-validation, documented in README. |
| ~~OI-03~~ | Low | Registry seqlock readers can starve under a hot writer. | Review v0.2 | 3 | Resolved with bounded retry and mutex fallback. |
| ~~OI-04~~ | Blocker | Decide whether v0.1 needs MPSC instead of SPSC. | Review v0.2 | before 6 | Resolved: implement generic MPSC in core. |
| ~~OI-05~~ | Medium | Adaptive polling idle CPU and tail latency tradeoff. | Review v0.2 | 8/9 | Resolved with adaptive spin/yield/sleep and docs; eventfd remains future. |
| OI-06 | Low | Micro-window between `shm_open(..., 0)` and `fchmod`. | Review v0.2 | n/a | Accepted: segment is not published to registry during this window. |
| ~~OI-07~~ | High | `pthread_mutex_consistent()` order is easy to misuse. | Review v0.2 | 4 | Resolved with `ShmMutexGuard` and explicit `mark_consistent()`. |
| ~~OI-08~~ | Low | ABI is not portable to musl/Alpine/32-bit/Windows. | Review v0.2 | 9 | Resolved in README limitations and static asserts. |
| ~~OI-09~~ | Medium | One heartbeat thread per segment would not scale. | Review v0.2 | 4 | Resolved with process-wide heartbeat worker. |
| ~~OI-10~~ | Medium | Ownership transfer needs RW attach/co-owner semantics. | Review v0.2 | 8 | Resolved in `LogCenter` and framework docs. |
| ~~OI-11~~ | Medium | Janitor zombie force-unlink algorithm was underspecified. | Review v0.2 | 4 | Resolved with draining/dead state and `/proc/*/maps` check. |
| OI-12 | Low | `MAX_REGISTRY_ENTRIES = 1024` is a compile-time cap. | Review v0.2 | future | Add registry payload versioning if larger capacity is required. |
| ~~OI-13~~ | Low | Cache line size was hardcoded to 64B. | Review v0.2 | 6 | Resolved with `SHMFX_CACHE_LINE_BYTES` fallback logic. |
| OI-14 | Low | Container/host clock namespace mismatch may affect PID start-time validation. | Review v0.2 | future | Keep `/proc` fallback for v0.1; consider pidfd opt-in later. |

## Maintainer Rules

1. When resolving an OI, strike it through and record where it was resolved.
2. New issues append to the end as `OI-(N+1)`; do not renumber existing items.
3. When the plan advances to a new phase, check this file's phase mapping and relevant open items.
