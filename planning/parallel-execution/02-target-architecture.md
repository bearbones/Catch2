# 02 — Target architecture

This document describes the end state that the roadmap builds toward. It is
written against the inventory IDs in [01-inventory.md](01-inventory.md).
Names such as `RunCoordinator` and `TestExecutionContext` are working titles.
The `Catch::` namespace is used throughout for readability; the final
namespace is decision D1 ([04](04-detachment.md#2-identity-names-namespaces-and-the-source-compatibility-question)).

## 1. Goals and non-goals

**Goals for 1.0**

| ID | Goal |
|---|---|
| G1 | Run **test cases** concurrently on a pool of worker threads in one process, selected with `--jobs N` / `--jobs auto`. |
| G2 | **Deterministic reports.** For every built-in reporter, output at `--jobs N` is byte-identical to `--jobs 1` (after the usual duration and timestamp normalisation). |
| G3 | **No regression at `--jobs 1`.** Same events, same order, same output as today, apart from the deliberate RP-8 bug fixes. The assertion fast path stays within a 3% budget (§11). |
| G4 | **Well-defined assertions from threads a test spawns**, including `REQUIRE`/`FAIL`/`SKIP`, with explicit context propagation. |
| G5 | **Per-test stdout/stderr capture** that works under concurrency for C++ streams, with a documented policy for C stdio. |
| G6 | **Crash in one test:** that test is reported as crashed, the other in-flight tests as aborted, completed tests are flushed, and the report documents are closed. |
| G7 | **Opt-outs for tests that need isolation:** per-test `[!exclusive]` and named `[!lock:<name>]` tags. |
| G8 | A **single-threaded build** (`CATCH_CONFIG_NO_THREADS`) for platforms without threads, behaving exactly like today. |

**Non-goals for 1.0** (candidates for later releases)

| ID | Non-goal |
|---|---|
| N1 | Running the partial runs of *one* test case (its section or generator paths) in parallel. See SEM-8. |
| N2 | Process isolation and per-test timeouts. The design keeps room for them (§12). |
| N3 | Live, completion-order reporting as the default. It is offered as an option only (§5.5). |
| N4 | Automatic detection of thread-unsafe user code. This is TSan's job and is covered by the migration guide. |

## 2. Execution model

```
                          Session::run()
                                │
                    ┌───────────▼────────────┐
                    │     RunCoordinator     │   one per run
                    │  - ordered test plan   │   (replaces TestGroup + the run half of RunContext)
                    │  - Scheduler           │
                    │  - Sequencer           │──────────────► reporter chain (unchanged reporters,
                    │  - aggregate Totals    │   one thread    MultiReporter, legacy listeners)
                    │  - atomic fail count   │
                    └───┬──────────┬─────────┘
          dispatch      │          │       completed TestRecords (ordinal-tagged)
                ┌───────▼──┐  ┌────▼─────┐
                │ Worker 1 │  │ Worker N │   std::thread pool, size = --jobs
                └───┬──────┘  └────┬─────┘
                    │ binds        │ binds
          ┌─────────▼──────────┐   ▼
          │ TestExecutionContext│  one per *running* test case
          │ - TrackerContext    │  (the per-test half of today's RunContext)
          │ - per-test Totals   │
          │ - per-thread state  │  last assertion, message stacks (per bound thread)
          │ - RNG               │
          │ - capture sinks     │
          │ - EventRecorder     │
          └─────────────────────┘
                    ▲
                    │ explicit bind (ScopedTestContext / Catch::thread)
            user-spawned threads
```

- **`RunCoordinator`** owns everything that is run-wide:
  - the plan (filtered, sorted, sharded, ordinal-numbered);
  - the scheduler;
  - the sequencer and the reporter chain;
  - aggregate totals;
  - the atomic failure counter and abort flag;
  - installing the process-wide hooks (capture demux, signal/SEH handlers) once per run.
- **`TestExecutionContext`** owns everything that belongs to one test case's
  execution across all of its partial runs. That is everything in EX-4 except
  the reporter.
- **Workers** pull a test from the scheduler, create a `TestExecutionContext`,
  bind it to the thread, and run today's `runTest` loop (partial runs until the
  tracker completes). They then unbind and hand the finished `TestRecord` to the
  sequencer.
- At `--jobs 1` there is one worker, which can be the main thread. Every event
  still flows through record → sequence → replay, so serial and parallel modes
  share a single code path (§5.4).

## 3. Context binding (EX-1, EX-2)

```cpp
namespace Catch {
    class TestContextHandle;                 // copyable, refcounted view of a TestExecutionContext
    TestContextHandle currentTestContext();  // throws/diagnoses if the calling thread is unbound

    class ScopedTestContext {                // RAII: binds a handle to the calling thread
    public:
        explicit ScopedTestContext(TestContextHandle);
        ~ScopedTestContext();                // unbinds, flushes this thread's pending UNSCOPED_INFO
    };
}
```

- **Storage.** The binding is one thread-local pointer.
  - In static builds it is an `inline` `thread_local` read.
  - In shared-library builds it is reached through an exported, non-inline
    accessor, because MSVC cannot `dllexport` thread-local data (EX-1).
  - Upstream's `CATCH_INTERNAL_THREAD_LOCAL` shows the same constraint.
- **What reads it.** `getResultCapture()` and every RAII macro object
  (`AssertionHandler`, `Section`, `ScopedMessage`, `Capturer`) resolve the
  binding **once, at construction**, and keep the pointer (EX-2).
- **Config stays global.** `IConfig` is immutable after `Session` builds it
  and is published before any worker starts.
- **Lifetime.**
  - A `TestExecutionContext` is reference-counted by its handles.
  - When the owning worker finishes the test, the context is *closed*.
  - A bound thread that asserts after close gets a diagnosed internal error
    ("assertion after test end"); it is not a use-after-free.
  - The coordinator reports tests that end with threads still bound (ST-10).

## 4. Assertions from user-spawned threads

**Principle: thread confinement first, locks only when shared.** Concurrent
*test cases* never share a `TestExecutionContext`, so the per-assertion path
needs no synchronisation at all. A lock is needed only when a test hands its
context to threads it spawned (G4).

- **Unshared (default).** Only the owning worker thread is bound.
  - The assertion path is single-threaded, as today.
  - Cost: one thread-local read per assertion.
- **Shared.** The first call to `currentTestContext()` on the owner thread sets
  the context's `shared` flag. This happens *before* the child thread exists,
  so `std::thread` construction provides the happens-before edge.
  - From then on, the context's assertion, message and recording paths take a
    per-context mutex. Each thread's "last assertion" and message stacks live
    in per-thread slots inside the context.
  - A context stays shared until the test ends.
  - The cost is paid only by tests that use threads, unlike a global
    compile-time switch. Upstream's opt-in mode costs 4–40% for everyone who
    enables it.
- **Convenience wrapper:**

```cpp
namespace Catch {
    // Drop-in for std::thread: binds the current test context in the child,
    // catches TestFailureException / TestSkipException / any exception,
    // records it against the test, and rethrows it on join() in the parent.
    class thread;
    template <typename F> auto async(F&&) -> std::future<...>; // same contract
}
```

| Situation | Behaviour |
|---|---|
| `CHECK`/`WARN`/`SUCCEED`/`FAIL_CHECK` on a bound thread | Recorded against the test. Thread-safe. |
| `REQUIRE`/`FAIL` failure on a bound thread | Recorded. The test is marked aborted. It throws on the calling thread. `Catch::thread` catches it and rethrows on `join()`. In a raw `std::thread`, the user must catch, otherwise `std::terminate` (documented, API-2). |
| `SKIP` on a bound non-owner thread | Recorded as a skip request. It takes effect when the owner joins (via `Catch::thread`), otherwise when the test ends. |
| `INFO`/`CAPTURE`/`UNSCOPED_INFO` | Per thread. A child does **not** inherit the parent's messages (API-4). An `inherit_messages` flag on `Catch::thread` is a possible extension. |
| `CHECKED_IF`/`CHECKED_ELSE` | Read the calling thread's own "last assertion passed" (API-3). |
| `SECTION`/`DYNAMIC_SECTION`/`GENERATE`/`BENCHMARK` on a non-owner thread | Hard error with a precise message (EX-5, API-5). |
| Any assertion macro on an **unbound** thread | Policy D5. Recommended: at `--jobs 1`, fall back to the single running test and emit a once-per-call-site warning, which keeps today's behaviour working. At `--jobs > 1`, report a framework error naming the source location and fail the run. Attribution is impossible, and silently guessing is worse. |

## 5. Event pipeline: record → sequence → replay

### 5.1 EventRecorder (RP-1, RP-2)

- **What it records.** Each `TestExecutionContext` owns a recorder. The recorder
  implements the test-level part of `IEventListener`:
  - `testCaseStarting`
  - `testCasePartialStarting`/`Ended`
  - `sectionStarting`/`Ended`
  - `assertionStarting`/`Ended`
  - benchmark events
  - `testCaseEnded`
  - `skipTest`
- **Self-contained events.** Each event is a deep copy:
  - assertion expressions are expanded eagerly, so no `LazyExpression` escapes;
  - `StringRef` payloads are copied;
  - message vectors are snapshotted;
  - the `TestCaseInfo` pointer is kept, because the registry outlives the run.
- **Filtering.** The recorder applies the union of the sinks' preferences up
  front.
  - Passing assertions are not materialised unless some sink wants them.
  - This preserves today's fast path for `handleExpr` passes (`internal/catch_run_context.cpp:583-586`).
- **Duration.** Test-case duration is measured on the worker and stored in a new
  `TestCaseStats::durationInSeconds` field (RP-3).
- **Serialisable.** The format is a flat, versioned list of tagged records. This
  costs nothing extra now, and lets process-isolated workers (§12) stream the
  same records over a pipe later.

### 5.2 Sequencer (G2)

- **Ordering.**
  - Every planned test has an ordinal: its index in the canonical order after filtering, sorting and sharding.
  - The sequencer replays `TestRecord`s strictly in ordinal order.
  - A completed record waits until every lower ordinal has been replayed or marked skipped.
- **Single writer.** Replay happens on one thread. That is either a dedicated
  reporter thread or whichever worker completes the head-of-line test, under a
  sequencer lock. Reporters, `MultiReporter`, the Win32 console colour (OS-3) and
  the shared output sinks (RP-10) therefore see exactly one caller.
- **Live run-level events.** `testRunStarting` is emitted live before dispatch.
  `noMatchingTestCases`, `reportInvalidTestSpec` and `testRunEnded` are emitted
  live after the last replay.
- **Recomputed totals.** Running totals in `AssertionStats` are recomputed during
  replay in canonical order (RP-4), so even the "totals so far" field is
  byte-identical to a serial run.
- **Memory bound.** A slow early test can make later records pile up.
  - The scheduler limits lookahead: at most `K × jobs` tests are dispatched
    beyond the oldest unreplayed ordinal (K is configurable; 8 is a reasonable
    default).
  - Spilling to disk is a stretch option.

### 5.3 Listeners (RP-6)

| Listener kind | When it is called | Thread | Requirement |
|---|---|---|---|
| Run-level events of any listener (`testRunStarting`/`Ended`, `listReporters`…) | Live | Main/coordinator | — |
| Legacy listener test-level events | Replay time | Sequencer | Default policy (D9): if any registered legacy listener overrides a test-level event, `--jobs` is clamped to 1 with a notice. Alternative: replay with a documented semantic change. |
| **New `IConcurrentEventListener`** | Live, on the worker (or bound thread), as the event happens | Worker | Must be thread-safe. Each call carries a `TestExecutionId`. This is the supported way to do per-test setup and teardown in parallel mode. |

`ValidatingTestListener` (`tests/SelfTest/TestRegistrations.cpp:33-180`) keeps
validating the replayed stream. A new validator checks the concurrent interface
per `TestExecutionId`.

### 5.4 One path for serial and parallel

At `--jobs 1`, every event still goes through record → sequence → replay. This
is deliberate:

1. Parallel mode is not a separately maintained code path.
2. Byte-identical output is testable on every CI run, by comparing
   `--jobs 1` with `--jobs N`.
3. The cost is one deep copy per *reported* event. Failing assertions are
   already deep-copied by cumulative reporters.

If profiling shows the copy matters for `-s` runs with millions of passing
assertions, the head-of-line test can stream live (§5.5). That optimisation is
also what restores liveness.

### 5.5 Liveness (RP-7)

- **Head-of-line streaming (optimisation).** The test that currently holds the
  lowest unreplayed ordinal may bypass buffering and stream events to the
  sequencer as they happen. This keeps output deterministic, since that test
  would have been replayed next anyway, and it restores live output for the
  oldest running test.
- **`--report-order completion` (optional).** Records are replayed as they
  complete. Output is no longer deterministic, but a failure appears as soon as
  its test finishes.
  - It is meant for interactive use.
  - It is paired with a TeamCity `flowId` mode, so IDEs can show concurrent
    tests natively.
- **Console progress line.** Shows `[running: 7/8 workers] [done: 412/1300]`
  on stderr when stderr is a TTY.

## 6. Output capture

**Mechanism (OS-1).** Swapping `rdbuf` per test is replaced by installing one
`DemuxStreambuf` on `std::cout`, `std::cerr` and `std::clog`, once per run
(when any sink wants capture, or `--jobs > 1`).

- **Routing writes.** Each `overflow`/`xsputn` call looks up the calling
  thread's bound capture sink (thread-local, set by `ScopedTestContext`).
  - **Bound and capture enabled:** append to that test's buffer. The buffer is
    per partial run, keeping today's `testCasePartialEnded` semantics.
  - **Unbound** (main thread, sequencer, user threads that were never bound):
    forward to the original `streambuf`.
- **Restoring at run end.** The demux is removed once all workers have joined.
  Construction and destruction are strictly LIFO relative to the reporters'
  `CoutStream` (RP-10).

| Output source | Captured per test? | Notes |
|---|---|---|
| `std::cout`/`std::cerr`/`std::clog` on the worker | Yes | — |
| … on a thread bound via `ScopedTestContext`/`Catch::thread` | Yes | Today such output is misattributed. |
| … on an unbound user thread | No, passes through to the terminal | Documented. |
| `printf`, `puts`, `fprintf(stdout)`, `write(1, …)` | **No** in parallel mode | Today's default capture already misses these. |
| fd-level capture (`CATCH_CONFIG_EXPERIMENTAL_REDIRECT` or its successor) | Only for tests in the **exclusive phase**, or at `--jobs 1` | Policy D6. When fd capture is configured, tests that need it can be tagged `[!exclusive]`, and the fd redirect is engaged only while an exclusive test runs alone. Otherwise `--jobs` is clamped to 1 with a notice. |

**Hazards to document:**

- User code that calls `std::cout.rdbuf(x)` itself overrides the demux while it
  is active.
- `sync_with_stdio(false)` has no effect on the demux.
- Writes from the reporter's own streams bypass the demux, as today.

## 7. Fatal errors (crashes, signals, SEH)

**Installation (OS-2): once per run, never per test.**

- **POSIX:**
  - `sigaction` for SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT with `SA_ONSTACK`.
  - Each worker allocates and installs its own `sigaltstack` when it starts.
    Threads spawned through `Catch::thread` get one too.
  - SIGINT and SIGTERM are **blocked in workers** (`pthread_sigmask`), so they
    are delivered to the coordinator thread and handled as a *run interruption*,
    not a test failure.
- **Windows:**
  - A vectored or top-level exception filter is installed once and chains to the
    previous filter (upstream fixed this chaining in 3.11.0).
  - `SetThreadStackGuarantee` is called per worker thread.

**Handler, in three stages:**

1. **Async-signal-safe stage.**
   - Read the faulting thread's context pointer from a TLS slot that was touched
     when the thread started. Use the `initial-exec` TLS model where available,
     so the read cannot allocate.
   - Write a one-line crash note with the test name, signal and worker id via
     `write(2)` / `WriteFile`.
   - Set the global `crashing` flag.
2. **Best-effort flush.** Bounded in time; uses `try_lock` only, never a
   blocking lock.
   - Finalise the crashing test's record: its recorded events, a synthetic
     `FatalErrorCondition` assertion, then close the open sections, the partial
     run and the test case. This fixes the missing `testCasePartialEnded` (RP-8).
   - Mark every other in-flight test as *aborted: crash in `<test>`*.
   - Replay all completed records in ordinal order, then emit `testRunEnded`.
   - Flush the reporter streams.
   - If a lock cannot be taken within the budget (for example a worker died
     holding the malloc lock), give up on stage 2. The note from stage 1
     survives.
3. **Restore and re-raise.** Restore the previous handlers and re-raise the
   signal (or `EXCEPTION_CONTINUE_SEARCH`), as today.

This is best effort by nature. A crash in a multi-threaded process is fragile
in every framework: Go and Rust abort the whole binary, and doctest and
upstream Catch2 also call their handling "best-effort". The robust answer is
process isolation (§12). For CI, a premature-exit sentinel file, like upstream
3.11's `--premature-exit-guard-file`, lets the CI job tell "crashed" apart from
"failed".

## 8. Scheduling

**Plan construction.** The plan is built exactly as today: filter, sort
(`--order`, `--rng-seed`), then shard. Each test gets its ordinal. Sharding
happens before parallelisation (SEM-3).

**Tags.** Special tags are parsed like the existing ones
(`catch_test_case_info.cpp:50-65`). `TestCaseProperties` is widened from
`uint8_t`, which has two free bits left.

| Tag | Meaning |
|---|---|
| `[!exclusive]` | Runs with no other test in flight. Implied by `[!benchmark]` (SEM-4). |
| `[!lock:<name>]` (repeatable) | Never runs concurrently with another test holding the same lock name. May run alongside everything else. A later extension can add read/write lock modes, as in JUnit's `@ResourceLock(mode=READ)`. |
| `[!parallel]` | Only meaningful under `--parallel tagged`: marks the test as eligible for concurrency. |

**Dispatch algorithm:**

1. Walk the plan in ordinal order.
2. Hand an eligible test to an idle worker.
3. A test whose lock is held goes to a pending list and is retried when the lock
   is released. This avoids head-of-line blocking, within the lookahead limit of
   §5.2.
4. An exclusive test drains the pool, runs alone (the exclusive *phase*), and
   then dispatch resumes.

**Opt-in model (D4).** Recommended:

- Run-level opt-in through `--jobs` (default 1 in 1.0), plus per-test opt-out
  through the tags.
- `--parallel tagged` inverts the per-test default (only `[!parallel]` tests run
  concurrently) for teams that want to migrate test by test, NUnit-style.

**Abort (`-x N`, SEM-2).**

- The atomic failure counter is checked at dispatch and in `populateReaction`.
- When it trips, dispatch stops, and in-flight tests abort at their next
  assertion through the existing `shouldThrow` path.
- Tests that were never dispatched get `skipTest` in ordinal order.
- The *set* of tests that ran is deterministic only at `--jobs 1`; this is
  documented.

**Oversubscription.** `--jobs auto` resolves to `hardware_concurrency()`.
The CMake helper sets the CTest `PROCESSORS` property to the same value, so
`ctest -j` does not multiply the two (INF-7).

## 9. Determinism and randomness

- **RNG (ST-3).**
  - Each `TestExecutionContext` owns a `SimplePcg32` seeded with the configured
    seed when the test starts. That is exactly what today's global reseed does
    (`internal/catch_run_context.cpp:195-226`).
  - So every `GENERATE(random(...))` sequence is **bit-for-bit identical** to
    the serial run, whatever the scheduling.
  - `sharedRng()` resolves to the bound context's RNG.
- **Order.** `--order rand` changes the plan, and therefore the report order.
  Same seed, same report, at any `--jobs`.
- **Remaining nondeterminism (documented):**
  - wall-clock durations;
  - the `-x N` executed set;
  - interleaving of output that is *not* captured (unbound threads, C stdio);
  - anything the tests themselves do with time or shared state.

## 10. Configuration surface

**Command line**

| Option | Values | Default (1.0) |
|---|---|---|
| `-j, --jobs` | `N`, `auto` | `1` |
| `--parallel` | `all`, `tagged` | `all` |
| `--report-order` | `canonical`, `completion` | `canonical` |
| `--max-lookahead` | `K` | `8` |

**Environment variable.** `<PREFIX>_JOBS` overrides `--jobs` unless `--jobs`
was given explicitly. This lets an organisation turn parallelism on, or roll it
back, fleet-wide from CI configuration without touching build files.
Precedence: CLI > environment > default.

**Compile-time switches**

| Switch | Effect |
|---|---|
| `CATCH_CONFIG_NO_THREADS` | No `<thread>`/`<mutex>`, no thread-locals (plain globals), `--jobs` rejected if > 1. Today's behaviour. |
| Thread support | Default *on* where `<thread>` exists. Replaces upstream's `CATCH_CONFIG_THREAD_SAFE_ASSERTIONS`: thread confinement makes the cost acceptable by default (§11). |

**Reporter and listener API**

- `ReporterPreferences` gains `supportsConcurrentEvents` (for live reporters
  used with `--report-order completion`).
- New interface: `IConcurrentEventListener`.
- `TestCaseStats` gains `durationInSeconds`.
- The `IResultCapture` interface becomes internal. Upstream 3.15 already
  started moving parts of it to free functions.

## 11. Performance budget and how it is measured

| Metric | Budget | Harness |
|---|---|---|
| Passing-assertion fast path, `--jobs 1`, Release | ≤ 3% slower than the pre-refactor baseline | Microbenchmark: 10⁸ `REQUIRE(i == i)` in one test |
| Same, Debug | ≤ 5% | Same |
| Shared context (spawned-thread asserts), uncontended | ≤ 25% vs unshared | Same benchmark, after `currentTestContext()` |
| SelfTest wall-clock at `--jobs auto` vs `--jobs 1` (8 cores) | ≥ 3× faster (SelfTest is short-test-dominated, so this is a lower bound) | CI job |
| Recorder overhead with `-s` | ≤ 15% vs no recording | SelfTest `-s -r xml` |
| Peak memory at `--jobs auto` | ≤ 2× `--jobs 1` | `/usr/bin/time -v` |

Upstream's thread-safe mode (4–40%) is the reference point for why confinement
matters. The budgets are gates in the roadmap: a phase is not done if it breaks
them.

## 12. Alternatives considered

| Alternative | Verdict |
|---|---|
| **Upstream-style global thread-safety only** (mutex on every assertion, no concurrent test cases) | Solves G4 only, at 4–40% cost for everyone. It is not concurrency of test cases. We adopt upstream's per-thread message model but not its global lock. |
| **Process workers instead of threads** (pytest-xdist, gtest-parallel, NUnit agents): the coordinator re-executes the same binary as N workers and streams serialised `TestRecord`s back over pipes | **Kept as the planned second backend** (stretch, post-1.0). It gives crash isolation, fd-level capture, per-test timeouts (kill the worker) and tolerance of thread-unsafe user code, at the cost of process start-up and serialisation. The scheduler, sequencer, reporters and record format are shared with the thread backend, which is why §5.1 requires records to be serialisable. On Windows it uses `CreateProcess` re-exec, not `fork`. |
| **Keep external parallelism only** (ctest `-j` plus `catch_discover_tests`, or sharding) | Already available. It has no in-process speed-up and pays process start-up and static-init cost per test case. It is what upstream recommends, and the fallback during migration. |
| **Parallelise section paths within a test case** | Deferred (N1). Paths are discovered dynamically, and each path re-executes the shared prefix. It needs a discovery pass and cloneable trackers, and gives little benefit for typical suites. |
| **Make reporters thread-safe and deliver events live from workers** | Rejected as the default. Every reporter would need rewriting, and output would become nondeterministic (breaking G2). Offered only through `IConcurrentEventListener` and `--report-order completion`. |
