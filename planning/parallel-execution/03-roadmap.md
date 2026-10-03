# 03 — Roadmap

Phases, work items, estimates, exit gates and staffing to get from this tree
to a detached 1.0 with in-process parallel execution. The organisational
migration that follows is in [05-migration-guide.md](05-migration-guide.md).
Work-item IDs (`P2.1` …) are also listed in
[work-breakdown.csv](work-breakdown.csv) for import into a tracker.

**About the estimates.** They are planning-grade ranges in
*engineer-weeks* (ew). They assume engineers who are fluent in modern C++
concurrency and have read this plan. They include implementation, tests and
review, but not the 25% contingency added in the totals. Treat them as the
starting point for the team's own estimation, not as commitments.

---

## Summary

| Phase | Purpose | Estimate (ew) | Ends with |
|---|---|---|---|
| [0](#phase-0--mobilise-and-rebase) | Decisions, rebase on upstream, CI sanitizers, perf baseline | 5.5–6 | **M0** baseline locked |
| [1](#phase-1--detach) | New identity, compatibility layer, packaging | 5–6.5 | **M1** detached alpha |
| [2](#phase-2--re-entrant-core-serial-behaviour-unchanged) | Split `RunContext`, thread binding, re-entrancy fixes | 8.5–10 | **M2** re-entrant core |
| [3](#phase-3--event-pipeline) | Record → sequence → replay for all reporting | 6.5–8 | **M3** pipeline parity |
| [4](#phase-4--parallel-execution) | Worker pool, scheduler, capture demux, `--jobs` | 7.5–8.5 | **M4** parallel beta |
| [5](#phase-5--crash-handling-and-platform-hardening) | Fatal-error redesign, Windows/macOS hardening | 5–5.5 | **M5** crash-safe beta |
| [6](#phase-6--assertions-from-spawned-threads) | Context handles, `Catch::thread`, shared-mode locking | 4–4.5 | **M6** 1.0 RC |
| [7](#phase-7--integration-documentation-10) | CMake integration, docs, release | 5.5–7.5 | **M7** 1.0 GA |
| **Total** | | **47.5–56.5** | +25% contingency ≈ **59–71** |

**Calendar.**

| Team size | Months to 1.0 GA | Notes |
|---|---|---|
| 3 engineers | **≈ 6–8** | Beta (M4/M5) around month 5. Phases 5 and 6 run alongside 4. The critical path is about 20 weeks before contingency and review latency. |
| 2 engineers | ≈ 9–11 | — |
| 1 engineer | Not recommended | The work is serialised and has no second reviewer for the concurrency code. |

**Critical path:** P0.2 → P1.1 → P2.1 → P3.1/P3.2 → P4.1/P4.3 → P5.1 → P7.3.

```
Month        1        2        3        4        5        6        7        8
Phase 0  ████░
Phase 1     ░████
Phase 2          ███████████
Phase 3                     ████████
Phase 4                              ████████
Phase 5                         ··········███████        (B: starts after P2, flush needs P3)
Phase 6                                  ███████          (B/C: after P2.2)
Phase 7                                          ░░░███████
Migration                                       pilot ──► waves (see 05)
Milestones  M0     M1           M2         M3        M4/M5    M6      M7
```

---

## Phase 0 — Mobilise and rebase

**Why first.** This tree is a strict ancestor of upstream `devel`: 308 commits
behind, no local changes. Moving to the latest upstream release is a
fast-forward now and a manual port after detachment. It also brings in
upstream's 3.9–3.12 thread-safe assertion and message work, the 3.7 output
redirect rework, `TEST_CASE_PERSISTENT_FIXTURE`, and several tracker and
reporter fixes.

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P0.1 | Decision workshop. Resolve D1–D12 in [06](06-risks-and-decisions.md#decision-register) and record the outcomes there. | — | 1 | Every decision has an owner and an outcome, or a dated deferral. |
| P0.2 | Fast-forward to the latest upstream release (v3.16.0 at the time of writing). Tag `upstream-fork-point`. Full CI run. | all | 0.5 | Green CI on the new base. |
| P0.3 | Re-anchor [01-inventory](01-inventory.md) line references on the new base and mark the items upstream fixed. | all | 0.5 | Inventory updated; IDs unchanged. |
| P0.4 | CI: add TSan (clang, Linux) and ASan+UBSan jobs. Add `evidence/tsan_repro.cpp` as an *expected-failure* test. Non-blocking for now. | INF-2 | 1–1.5 | Jobs run on every PR. |
| P0.5 | Performance harness: assertion fast-path microbenchmark, SelfTest wall-clock, peak memory. Record the baselines. | §11 of 02 | 1 | Baseline numbers committed; regression job runs. |
| P0.6 | Thread linkage in CMake/Meson/Bazel/Conan/pkg-config. Skeleton `CATCH_CONFIG_NO_THREADS` build job. | INF-1 | 1 | All build systems link threads; the no-threads job builds. |
| P0.7 | Byte-identity harness: `approvalTests.py` parameterised by extra CLI args, so the same baselines can be checked under `--jobs N` later. | INF-3 | 0.5 | Harness merged (it only runs `--jobs 1` for now). |

**Gate M0:**
- Rebased.
- Decisions recorded.
- Sanitizer and perf jobs running.
- Baseline numbers published.

## Phase 1 — Detach

Details and options are in [04-detachment.md](04-detachment.md).

| ID | Work item | ew | Done when |
|---|---|---|---|
| P1.1 | Apply the new identity (D1): include root, library and target names, CMake package, pkg-config, Conan/Bazel/Meson names, and the namespace strategy (recommended: keep `Catch` and add an inline ABI namespace). Do it with a committed, re-runnable rename script. | 2–2.5 | Builds and tests green under the new names. |
| P1.2 | Compatibility layer: a `catch2-compat` target providing `<catch2/...>` forwarding headers, `CATCH_CONFIG_*` macro translation and deprecation diagnostics. | 1 | A consumer smoke project builds unmodified against compat. |
| P1.3 | Branding and legal: runtime strings (`--libidentify`, console banner), output-format identifiers (XML root and JSON keys, per D11), README/SECURITY/CoC/templates, `NOTICE` + `UPSTREAM.md` provenance ledger, license checker update. | 1 | Legal review signed off. |
| P1.4 | Release tooling: version scheme, release scripts, amalgamation script, header-guard and license checks. | 0.5–1 | A dry-run release produces artefacts. |
| P1.5 | Internal distribution: package-registry publishing, CMake config, and a consumer smoke-test project in CI. | 0.5–1 | The pilot team can consume the alpha. |

**Gate M1:**
- An alpha package is consumable by a pilot project with **no source changes**,
  through the compat layer.
- CI is green on the full matrix.

## Phase 2 — Re-entrant core (serial behaviour unchanged)

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P2.1 | Split `RunContext` into `RunCoordinator` and `TestExecutionContext`. Per-test `Totals`. `TestGroup` folds into the coordinator. | EX-3, EX-4 | 3–4 | Baselines identical. |
| P2.2 | Thread-local binding with a DLL-safe accessor. Cache the resolved context in `AssertionHandler`, `Section`, `ScopedMessage` and `Capturer`. | EX-1, EX-2 | 1–1.5 | No remaining global result-capture reads (lint check). |
| P2.3 | Per-context RNG. `sharedRng()` resolves through the binding. | ST-3 | 0.5 | `RandomNumberGeneration` tests and generator baselines unchanged. |
| P2.4 | Re-entrancy fixes: `Singleton` init and cleanup, string-stream pool (thread-local), `gmtime_r`, frozen registries and sort cache, atomic counters, `call_once` benchmark environment. | ST-1, ST-2, ST-4–ST-9 | 1.5 | TSan clean on the P2.7 harness. |
| P2.5 | Fix the event-contract bugs and re-approve baselines, with each diff reviewed explicitly: outer-first deferred `sectionEnded`, missing `testCasePartialEnded` on the fatal path, the `assertionStarting` pairing decision. | RP-8 | 1 | The `…/B2/B` baseline path becomes `…/B/B2`. Every re-approved diff is listed in the PR. |
| P2.6 | Owner-thread checks for `SECTION`/`GENERATE`/`BENCHMARK`. | EX-5, API-5 | 0.5 | Negative tests produce the diagnostic. |
| P2.7 | Concurrency harness: run N `TestExecutionContext`s on N threads directly, bypassing the scheduler, under TSan. | — | 1 | Harness in CI, TSan clean. |

**Gate M2:**
- Approval baselines identical apart from the reviewed P2.5 diffs.
- TSan clean on the harness.
- Fast path within budget (≤ 3% Release, ≤ 5% Debug).

## Phase 3 — Event pipeline

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P3.1 | `EventRecorder`: deep copies, eager expansion, preference-union filtering, serialisable versioned format. | RP-1, RP-2 | 2–2.5 | Round-trip tests for every event type. |
| P3.2 | Sequencer: ordinals, ordered replay, recomputed running totals, single-writer guarantee, lookahead accounting. | RP-1, RP-4 | 1.5–2 | Unit tests with out-of-order completion. |
| P3.3 | `TestCaseStats::durationInSeconds`. XML and TeamCity reporters use it. | RP-3 | 0.5 | Durations are correct under replay. |
| P3.4 | Listener policy: `IConcurrentEventListener`, legacy test-level override detection and `--jobs` clamp (D9). | RP-6 | 1.5 | Validators for both interfaces. |
| P3.5 | `MultiReporter` stdout re-emission from the sequencer. | RP-5 | 0.5 | ExtraTests X24/X25 pass. |
| P3.6 | Route **all** reporting at `--jobs 1` through the pipeline. `ValidatingTestListener` and ExtraTests X20–X28 green. | INF-4, INF-5 | 0.5–1 | Old direct-dispatch path deleted. |

**Gate M3:**
- Byte-identical baselines at `--jobs 1` through the pipeline.
- Recorder overhead with `-s` ≤ 15%.
- Fast-path budget still met.

## Phase 4 — Parallel execution

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P4.1 | Worker pool and scheduler: `--jobs N/auto`, ordinals, lookahead, exclusive phase, `[!lock:]`/`[!exclusive]`/`[!parallel]` tags (widen `TestCaseProperties`), `--parallel tagged`. | SEM-1, SEM-3 | 2–2.5 | Scheduler unit tests, including lock starvation and exclusivity. |
| P4.2 | Abort semantics: atomic failure count, stop dispatch, cooperative cancel, `skipTest` for tests never dispatched. | SEM-2 | 0.5 | `-x` tests at several `--jobs` values. |
| P4.3 | `DemuxStreambuf` capture. fd-capture and C-stdio policy (D6). | OS-1, RP-10 | 2 | ExtraTests X24/X25/X27 pass at `--jobs N`; the capture stress test is TSan clean. |
| P4.4 | Benchmarks run exclusively; warning for an untagged `BENCHMARK` reached in a parallel phase. | SEM-4, ST-7 | 0.5 | Benchmark tests are stable at `--jobs auto`. |
| P4.5 | Approvals at `--jobs N`. SelfTest's own static state made re-entrant or tagged. | INF-3, INF-6 | 1 | Every reporter is byte-identical at `--jobs 1/2/8/auto`. |
| P4.6 | Soak: nightly 1000× SelfTest at `--jobs auto` under TSan, with random seeds. | INF-2 | 0.5–1 | 7 consecutive clean nights. |
| P4.7 | `<PREFIX>_JOBS` environment override and its precedence rules. | — | 0.5 | Documented and tested. |
| P4.8 | Per-test resource helpers: a unique temporary directory per test and a unique name token, for files, IPC and database names. | UT-3 | 0.5 | Unique across concurrent tests and partial runs; cleaned up on success. |

**Gate M4 (parallel beta):**
- G1, G2 and G5 met on Linux, macOS and Windows.
- TSan soak clean.
- SelfTest at least 3× faster on 8 cores.
- Peak memory ≤ 2×.

## Phase 5 — Crash handling and platform hardening

This phase can start after P2.2 with a second engineer. The flush stage needs
P3.2.

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P5.1 | Fatal-error redesign: install once per run, per-worker alternate stacks and stack guarantees, SIGINT/SIGTERM masking, three-stage handler with a bounded `try_lock` flush. | OS-2, RP-9 | 2.5–3 | — |
| P5.2 | Crash test matrix: SIGSEGV, SIGABRT, stack overflow and SEH access violation, in parallel, for every reporter. The output document must be well formed, with the crashed test plus the aborted tests. | OS-2, INF-5 | 1 | Matrix green on all three operating systems. |
| P5.3 | Win32 console: single writer; VT mode when available. | OS-3 | 0.5 | Colour tests pass on Windows. |
| P5.4 | Platform matrix: MSVC (DLL and static), MinGW (emutls), GCC/Clang on Linux, Apple Clang, and the no-threads build. | EX-1, INF-1 | 1 | All jobs green. |

**Gate M5:** G6 met on all three operating systems.

## Phase 6 — Assertions from spawned threads

This phase can start after P2.2. Upstream's 3.9–3.12 per-thread message work
(brought in by P0.2) is the starting point.

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P6.1 | `TestContextHandle`, `ScopedTestContext`, `currentTestContext()`. Shared-mode flag, per-context mutex and per-thread slots. | API-1, API-3, API-4 | 1.5 | — |
| P6.2 | `Catch::thread` and `Catch::async`, with `REQUIRE`/`FAIL`/`SKIP` and exception propagation on join. | API-2 | 1 | — |
| P6.3 | Unbound-thread policy (D5), diagnostics, and the end-of-test check for still-bound threads. | API-1, ST-10 | 0.5 | — |
| P6.4 | Thread-safe `MatcherUntypedBase::toString`. Documentation for shared objects. | API-6 | 0.5 | — |
| P6.5 | TSan suites for in-test threads. Shared-mode performance budget (≤ 25% uncontended). | — | 0.5–1 | — |

**Gate M6 (1.0 RC):**
- G4 met.
- The original TSan repro is clean when it uses `Catch::thread`.
- With a raw `std::thread` at `--jobs N`, the same repro produces the
  documented diagnostic.

## Phase 7 — Integration, documentation, 1.0

| ID | Work item | Inventory | ew | Done when |
|---|---|---|---|---|
| P7.1 | CMake integration: discovery mode per binary or per shard that passes `--jobs` and sets `PROCESSORS`; sharded-tests helper updated. | INF-7 | 1.5 | `DiscoverTests` and the sharding scripts are covered. |
| P7.2 | Documentation rewrite: limitations, a new "parallel execution" guide, thread safety, reporters and listeners, CLI, configuration, migration. | INF-9 | 2 | Reviewed by a pilot team. |
| P7.3 | Release engineering: 1.0 RC and GA, packaging to every internal registry, amalgamated build. | INF-8 | 1 | GA artefacts published. |
| P7.4 | Buffer for pilot feedback. | — | 1–3 | No open P1/P2 pilot issues. |

**Gate M7 (1.0 GA):**
- Every G1–G8 goal met.
- Two or more pilot suites run in CI at `--jobs auto` with no increase in flake
  rate over 4 weeks.

---

## Testing strategy (applies across phases)

| Layer | What | When |
|---|---|---|
| Byte identity | All approval baselines at `--jobs 1`, `2`, `8` and `auto` must match | Every PR from M4 on |
| Sanitizers | ASan+UBSan on every PR. TSan on every PR for the concurrency harnesses; full SelfTest soak nightly | Phase 0 onward |
| Determinism | Same `--rng-seed` and `--order rand` give identical output across `--jobs` values | Every PR from M4 on |
| Fault injection | Crash matrix (signal × reporter × OS) | Every PR touching OS-2 code; nightly |
| Stress | Random `--jobs`, random seeds, `-x` variants, lock-heavy synthetic suites | Nightly |
| Performance | Fast-path, shared-mode, recorder, wall-clock and memory budgets | Every PR (gating from M2) |
| Dog-fooding | SelfTest itself runs at `--jobs auto` in CI | From M4 |
| Consumer canaries | Pilot organisation suites built and run nightly against `main` | From M1 (serial), M4 (parallel) |

## Staffing

| Role | Load | Owns |
|---|---|---|
| **Engineer A**: core runtime | Full time | P2.1, P2.2, P3.1–P3.2, P4.1–P4.2; reviewer for B |
| **Engineer B**: OS integration and concurrency | Full time from month 2 | P4.3, Phase 5, Phase 6; reviewer for A |
| **Engineer C**: build, CI, tooling, docs | Full time | Phase 0 infrastructure, Phase 1, P4.5–P4.6, Phase 7 |
| **Tech lead / architect** | ~25% | Decision register, design reviews, gate sign-off |
| **Migration lead** | ~50% from M4 | Pilot selection, migration waves, consumer support ([05](05-migration-guide.md)) |
| **Legal / OSS office** | Ad hoc | Licence and notices review in Phase 1 |

Two people must review all concurrency-sensitive code (binding, sequencer,
scheduler, demux, fatal handler). One of them must not be the author.

## Post-1.0 candidates

Ordered by expected value:

1. **Process-isolation backend** (`--isolation process`). Re-exec workers that
   stream `TestRecord`s over pipes. It gives crash isolation, fd capture, and
   per-test timeouts (`--test-timeout`). The scheduler, sequencer and record
   format are reused (02 §12).
2. **Duration-aware scheduling.** Use the previous run's durations to dispatch
   longest-first. This cuts the long tail.
3. **Completion-order live reporting.** TeamCity `flowId`, and an IDE-friendly
   streaming JSON mode.
4. **Read/write resource locks** and lock declarations at file or fixture level.
5. **Parallel section paths within one test case** (N1). Research only.
