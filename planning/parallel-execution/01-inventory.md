# 01 — Inventory: what blocks multi-threaded execution

This is a catalogue of every construct in this tree (Catch2 v3.6.0+, devel
commit `33e24b1`) that breaks, or changes meaning, once test cases run concurrently
on several threads in one process. It also covers assertions made from threads
a test spawns itself. Each item has:

- **ID**: referenced by the roadmap and work breakdown.
- **Where**: `path:line`, relative to `src/catch2/` unless the path says otherwise.
- **Hazard**: one of the three severities below.
- **Fix**: the intended remediation, detailed in [02-target-architecture.md](02-target-architecture.md).
- **Effort**: S ≤ 1 engineer-week, M = 1–3, L = 3–6, XL > 6.

| Severity | Meaning |
|---|---|
| **S1: memory-unsafe** | A data race that corrupts memory or is undefined behaviour. Crashes and heisenbugs. |
| **S2: semantic** | No memory corruption, but results are wrong: misattributed assertions, wrong totals, interleaved or out-of-order output, lost determinism. |
| **S3: latent** | Racy only at startup or teardown, or in rarely used paths. Fix opportunistically. |

---

## Summary heat map

| Area | Items | S1 | S2 | S3 | Dominant effort |
|---|---|---|---|---|---|
| [EX: execution-context plumbing](#ex--execution-context-plumbing) | 6 | 3 | 3 | – | **L** (RunContext split) |
| [ST: process-global static state](#st--process-global-static-state) | 10 | 5 | 1 | 4 | S each |
| [OS: process-wide OS resources](#os--process-wide-os-resources) | 5 | 2 | 2 | 1 | **L** (capture, fatal handling) |
| [RP: reporter / listener pipeline](#rp--reporter-and-listener-pipeline) | 10 | 2 | 7 | 1 | **L** (record and replay) |
| [SEM: run semantics and CLI](#sem--run-semantics-and-cli-behaviour) | 9 | – | 4 | 2 | S–M |
| [API: user-facing macros used from threads](#api--user-facing-api-used-from-several-threads) | 8 | 3 | 5 | – | M |
| [INF: build, test and tooling](#inf--build-test-infrastructure-and-tooling) | 9 | – | – | – | M overall |
| [UT: hazards in *consumers'* test code](#ut--hazards-in-consumers-test-code) | 10 | – | – | – | Migration risk, not framework work |

Counts are per item, using the item's worst severity. INF and UT items are work
or risk, not defects, so they carry no severity. Neither do SEM-3, SEM-5 and
SEM-7, which need no change beyond tests.

### Empirical confirmation

A single test that runs `INFO` + `CHECK` from four `std::thread`s was built
against the unmodified amalgamated sources with GCC 13 `-fsanitize=thread`.
Assertions from spawned threads go through the same shared objects that
concurrent test cases would. ThreadSanitizer reported **106–130 distinct
races** (two runs). Both runs then hit a **heap-use-after-free** in
`RunContext::pushScopedMessage`, then a **SEGV**. Details and the repro source are in
[evidence/README.md](evidence/README.md). The racing functions match
EX-4, EX-6, ST-2 and the `m_totals`/`m_lastAssertionInfo` items below.

### Root cause in one paragraph

Catch2 has exactly one process-global "current test" slot (`Context`). It holds
one `IResultCapture*`, which points at the single `RunContext`. That object
owns the per-test state (section tracker, messages, "last assertion", totals)
and the run-level state (reporter, abort decision). Every macro
(`REQUIRE`, `SECTION`, `INFO`, `GENERATE`, `BENCHMARK`) finds that object
through a global lookup. Reporters receive events synchronously, and they
assume one test is active at a time and that events nest strictly. Around
that core are process-wide OS facilities that Catch2 toggles per test case:
`std::cout` buffer swaps, `dup2` on fds 1 and 2, signal handlers and the SEH
filter.

So most of the work is **one structural refactor plus a handful of re-entrancy
fixes**, not a long list of independent bugs. The refactor splits run-level
state from per-test state and binds per-test state to threads. The fixes cover
the string-stream pool, RNG, message counter, enum registry and `gmtime`. The
hard engineering is in three places: output capture (OS-1), crash handling
(OS-2), and keeping reporter output deterministic (RP-1 to RP-3).

### Where upstream Catch2 stands (v3.16.0, checked 2026-10-03)

This repository's `HEAD` (`33e24b1`, July 2024) is a **strict ancestor** of
upstream `devel`, 308 commits behind, with no local changes. Catching up is a
fast-forward today. After detachment it would be a manual port. Since this
tree was cut, upstream has done part of the "assertions from spawned threads"
work:

- v3.9.0 added experimental opt-in thread-safe assertions.
- v3.10.0 made the message macros thread-safe.
- v3.12.0 made the feature stable as `CATCH_CONFIG_THREAD_SAFE_ASSERTIONS`.

Upstream has **not** done concurrent test-case execution. Its docs still say
execution "is strictly serial, and there are no plans to change this"
(`docs/limitations.md`). The FAQ calls parallelism "the job of an external
test runner".

| Inventory item | Upstream v3.16.0 status |
|---|---|
| EX-1 global context slot | **Open.** `Context::currentContext` is now a static object rather than a pointer, but there is still exactly one. |
| EX-2/3/4 single `RunContext`, serial loop | **Open.** One `RunContext`, one reporter, one tracker, serial `TestGroup` loop. "Last assertion" and message state became thread-local statics (opt-in), and reporter calls take one `m_assertionMutex`. |
| EX-5 tracker | **Open by design.** Upstream docs: sections and generators from spawned threads are "here to stay" unsupported. |
| EX-6 message identity | **Fixed (opt-in).** Thread-local message holders and a thread-local ID counter. |
| ST-1 `Singleton<>` lazy init | **Open.** Same hand-rolled pattern. |
| ST-2 string-stream pool | **Fixed (opt-in)** with a mutex. A race in it was fixed again in 3.11.0 (#3031). |
| ST-3 shared RNG | **Open.** Single `sharedRng()`, reseeded per test. |
| ST-4 enum registry | **Fixed.** `CATCH_REGISTER_ENUM` now builds a function-local static and no longer mutates the registry. |
| ST-6 `gmtime` | **Open.** Moved to `catch_tostring.cpp`, still `std::gmtime` on non-MSVC. |
| ST-7 benchmark env cache | **Open.** |
| OS-1 output capture | **Reworked in 3.7.0** into one redirect object per run, deactivated while reporters write. Still process-wide `rdbuf`/`dup2`. |
| OS-2 fatal handling | **Open.** Still engaged per test case. 3.11.0 made the SEH filter chain to the previous one. |
| RP-1…RP-7 reporter pipeline | **Open.** Serial event contract unchanged. 3.9.0 added `shouldReportAllAssertionStarts`. |
| API-1/3/4 assertions and messages from spawned threads | **Fixed (opt-in).** `REQUIRE`/`FAIL`/`SKIP` in a spawned thread still terminate the process (API-2 open). |
| MSVC `thread_local` + DLL export (EX-1 caveat) | **Worked around.** The `CATCH_INTERNAL_THREAD_LOCAL` macro applies `thread_local` only in thread-safe builds. |

Upstream measured the cost of its opt-in mode at **4–20% typically, up to 40%**
on the optimised passing-assertion fast path (`docs/thread-safety.md`, commit
`2a8a8a72`). This is why the architecture here uses *thread confinement*: no
locks on the assertion path unless a test explicitly shares its context with
threads it spawned. See [02 §4](02-target-architecture.md#4-assertions-from-user-spawned-threads).

The roadmap's first step is to fast-forward to the latest upstream release
**before** detaching ([03 Phase 0](03-roadmap.md#phase-0--mobilise-and-rebase)),
then re-anchor the line numbers below. The item IDs stay stable; only the
`path:line` references move.

---

## EX — Execution-context plumbing

### EX-1 Global current-context slot · S2 (+S3) · M
- **Where:**
  - `internal/catch_context.hpp:19-22`; `internal/catch_context.cpp:14` (`Context* Context::currentContext`).
  - Lazy creation at `catch_context.hpp:37-43` and `catch_context.cpp:24-28`.
  - `RunContext` installs itself at `internal/catch_run_context.cpp:175` and never uninstalls.
- **What:** one global pointer holds one `IResultCapture*` (the run) and one `IConfig*`.
  - It is declared `CATCH_EXPORT static` and read by an `inline` accessor, so shared-library builds reach it across a DLL boundary.
  - Lazy creation is unsynchronised.
  - The capture pointer dangles once `RunContext` is destroyed.
- **Why it blocks:** every thread resolves to the same run, so concurrent tests share all of their state.
- **MSVC caveat:** you cannot simply make the slot `thread_local`, because thread-local data cannot be given DLL linkage with `__declspec(dllexport)`.
- **Fix:**
  - Keep `IConfig*` global. It is immutable after startup and set before workers start.
  - Replace the result-capture slot with a **thread-local binding** to a per-test execution context, read through an exported **non-inline** accessor. Static builds may inline it.
  - Create the context eagerly in `Session`.
  - Clear the binding when a test ends.

### EX-2 `getResultCapture()` is the universal hook · S2 · M
- **Where:** defined at `internal/catch_run_context.cpp:692-697`. Every call site:

| Call site | Location |
|---|---|
| `AssertionHandler` constructor, i.e. every assertion macro | `internal/catch_assertion_handler.cpp:23` |
| `CHECKED_IF` / `CHECKED_ELSE` | `internal/catch_test_macro_impl.hpp:62,67` |
| `Section` constructor and destructor | `internal/catch_section.cpp:18,31,47,49` |
| `ScopedMessage` constructor and destructor | `catch_message.cpp:25,35` |
| `Capturer` | `catch_message.cpp:44` |
| `UNSCOPED_INFO` | `catch_message.hpp:114` |
| Generators | `generators/catch_generators.cpp:31,37` |
| `BENCHMARK` | `benchmark/catch_benchmark.hpp:64,80,88,90,92` |
| Fatal-signal path (through `getCurrentContext()`) | `internal/catch_fatal_condition_handler.cpp:61-63` |

- **Why it blocks:** every one of these must reach the *calling thread's* test.
  - `~ScopedMessage` **re-fetches** the global instead of caching the context it pushed to (`catch_message.cpp:35`). If the binding changes, the pop goes to the wrong place.
- **Fix:**
  - Resolve through the thread-local binding (EX-1).
  - Cache the resolved context in every RAII object (`Section`, `ScopedMessage`, `Capturer`, `AssertionHandler`).
  - `AssertionHandler` already caches its context (`internal/catch_assertion_handler.hpp:29`), which is the good pattern to copy.

### EX-3 One `RunContext` per run, executed serially · S1 · L
- **Where:**
  - `catch_session.cpp:77-138`: `TestGroup` owns one `RunContext m_context` (`:134`); `execute()` loops serially (`:107-124`).
  - `internal/catch_run_context.hpp:35-155`.
- **What:** `RunContext` mixes run-level and per-test responsibilities.
  - **Run-level:** owns the reporter, emits `testRunStarting`/`testRunEnded` (`catch_run_context.cpp:176,180`), keeps run totals, decides `aborting()`.
  - **Per-test:** tracker, sections, messages, last-assertion info, fatal-handler guard.
- **Fix:** split it into two objects.
  - **`RunCoordinator`** (one per run): reporters and sequencer, aggregate totals, atomic failure count and abort flag, scheduler.
  - **`TestExecutionContext`** (one per running test case): `TrackerContext`, section stacks, per-thread message stacks, per-test `Totals`, RNG, capture sinks, event recorder.
  - This is the central refactor; most other items hang off it.

### EX-4 Per-test fields living in the run object · S1 · (part of EX-3)
- **Where:** `internal/catch_run_context.hpp:137-154`.

| Field | Problem under concurrency |
|---|---|
| `m_totals` | Run-wide. Per-test results come from snapshot subtraction (`catch_run_context.cpp:184,237,243,250`). With concurrent tests, the snapshots include other tests' increments, so per-test pass/fail is wrong. `AssertionStats` also carries *running run totals* to reporters (`:290`). |
| `m_lastAssertionInfo`, `m_lastResult`, `m_lastAssertionPassed`, `m_shouldReportUnexpected` | "Last assertion" state. It is used to attribute unexpected exceptions (`:507,:536`) and fatal errors (`:448`), and by `CHECKED_IF`/`CHECKED_ELSE` and the deprecated `getLastResult()`. It must be per test and, for in-test threads, per thread. TSan flagged `resetAssertionInfo` and `assertionPassed` most often. |
| `m_lastResult` | Holds an `AssertionResult` whose `LazyExpression` points at a **stack temporary** (`internal/catch_lazy_expr.hpp:22`; `catch_run_context.cpp:607`). It is harmless only because reporters consume it synchronously. |
| `m_messages`, `m_messageScopes` | `INFO`/`CAPTURE`/`UNSCOPED_INFO` stacks. TSan found a use-after-free here; see EX-6. |
| `m_activeSections`, `m_unfinishedSections`, `m_trackerContext` | Section and generator state. See EX-5. |
| `m_fatalConditionhandler` | Process-wide in reality. See OS-2. |

- **Fix:** move these into `TestExecutionContext`. Thread-sensitive parts (last assertion, messages) become per-thread sub-state within it.

### EX-5 Section/generator tracking is single-threaded by design · S2 · S
- **Where:**
  - `internal/catch_test_case_tracker.hpp/.cpp`.
  - `GeneratorTracker` at `internal/catch_run_context.cpp:30-166`.
- **Good news:** `TrackerContext` has **no globals**. One per `TestExecutionContext` is enough for test-case-level parallelism.
- **Hazard:** it mutates a tree through `m_currentTracker`. A `SECTION`/`GENERATE` executed on a *spawned* thread inside a test corrupts it.
- **Fix:**
  - Record the owner thread when a `TestExecutionContext` is bound.
  - `SECTION`/`DYNAMIC_SECTION`/`GENERATE`/`BENCHMARK` on any other thread becomes a hard, well-described error.
  - Parallelising the *partial runs* of one test case (different section paths) is out of scope for 1.0. Paths are discovered dynamically. See SEM-8.

### EX-6 Message identity and storage · S1 · M
- **Where:**
  - `internal/catch_message_info.cpp:19,23`: `++globalCount`, a plain `unsigned`. The source comment at `:22` reads "*This may need protecting if threading support is added*".
  - `internal/catch_message_info.hpp:30-35`: equality compares `sequence` only.
  - `internal/catch_run_context.cpp:414-424`: `popScopedMessage` erases by equality.
- **Hazard:**
  - Racing increments produce duplicate sequence numbers, so popping one thread's `INFO` removes another's.
  - The `std::vector` itself is mutated concurrently. TSan reported a heap-use-after-free in `pushScopedMessage`.
  - Semantically, `INFO` is lexically scoped, and therefore per thread. One shared stack attaches thread A's context to thread B's failures.
- **Fix:**
  - Make the counter 64-bit and atomic.
  - Keep message stacks per thread inside the test context.
  - Pop by identity (stack discipline), not by search.
  - Define `UNSCOPED_INFO` as "until the next assertion **on this thread**".

---

## ST — Process-global static state

### ST-1 `Singleton<>` lazy init and teardown · S3 · S
- **Where:** `internal/catch_singletons.hpp:26-30` (`static Singleton* s_instance = nullptr; if(!s_instance) …`) and `internal/catch_singletons.cpp:16-18`, `:26`, `:28-34`.
- **Hazard:**
  - The hand-rolled lazy init races if first use is concurrent.
  - `cleanupSingletons()` deletes the instances but never resets `s_instance`, so any use after cleanup is a use-after-free.
- **Fix:** use function-local magic statics or `std::call_once`, force initialisation in `Session` before any worker starts, and null the pointer on cleanup.

### ST-2 `ReusableStringStream` pool · **S1** · S
- **Where:** `internal/catch_reusable_string_stream.cpp:18-50`. The pool is `Singleton<StringStreams>` (`:43,44,50`).
- **Used by:** every stringification (`catch_tostring.*`), every `INFO`/`WARN`/`UNSCOPED_INFO` (`catch_message.hpp:35`), `DYNAMIC_SECTION`, expression reconstruction (`catch_assertion_result.cpp:22`), `CATCH_ENFORCE`, matchers, Approx and reporters.
- **Hazard:** `m_streams.push_back` can reallocate while another thread indexes `m_streams[m_index]`. TSan flagged `StringStreams::add`/`release` and `ReusableStringStream::str`/`~ReusableStringStream`.
- **Fix:**
  - Make the pool `thread_local`. This keeps the amortisation and needs no locks.
  - Make sure thread-exit destruction order is safe. The pool must outlive any `ReusableStringStream` on that thread, which holds because those are stack objects.

### ST-3 Shared RNG · **S1** + reproducibility · S
- **Where:**
  - `internal/catch_context.cpp:36-39` (`static SimplePcg32 s_rng`).
  - Reseeded per test case at `internal/catch_run_context.cpp:226`; the comment at `:195-225` explains why.
  - Consumed by `random()` generators via `generators/catch_generators_random.cpp:17`.
- **Hazard:** concurrent tests reseed and advance each other's generator. That is a data race, and it makes "same seed, same values" false.
- **Fix:**
  - Give each `TestExecutionContext` its own `SimplePcg32`, seeded with the config seed when the test starts.
  - This reproduces today's per-test sequences **bit for bit**, because today's RNG is already reset at every test start.
  - `sharedRng()` resolves to the bound context's RNG.

### ST-4 Registry mutation during the run · S1 (narrow) · S
- **Where:**
  - `CATCH_REGISTER_ENUM` registers lazily on first stringification (`catch_tostring.hpp:662`), which leads to `internal/catch_enum_values_registry.cpp:67` (`m_enumInfos.push_back`).
  - Runtime `REGISTER_TEST_CASE` (`internal/catch_test_registry.cpp:67-81`).
- **Hazard:**
  - Two different registered enums stringified for the first time on two threads race on the vector.
  - The magic static only protects one enum.
  - Elements are `unique_ptr`, so returned references stay valid; only the vector races.
- **Fix:**
  - Put a mutex in `EnumValuesRegistry::registerEnum`.
  - Forbid test registration after the run starts, with a diagnostic.

### ST-5 Lazily sorted test list cache · S3 · S
- **Where:** `internal/catch_test_case_registry_impl.hpp:49-50` (`mutable` cache) and `internal/catch_test_case_registry_impl.cpp:142-151`.
- **Fix:** compute it once before workers start and expose an immutable view.

### ST-6 `std::gmtime` in `StringMaker<system_clock::time_point>` · **S1** · S
- **Where:** `catch_tostring.hpp:639`. This is the non-MSVC branch; MSVC uses `gmtime_s` at `:637`.
- **Hazard:** `gmtime` returns a pointer to a process-wide static `tm`. It runs during assertions and `CAPTURE`.
- **Fix:** use `gmtime_r` (POSIX) / `gmtime_s`.

### ST-7 Benchmark clock-environment cache · S1 (narrow) · S
- **Where:** `benchmark/detail/catch_estimate_clock.hpp:108-121`. A hand-rolled lazy assign to `static unique_ptr<Environment>` runs a 0.5–1 s measurement.
- **Fix:** use `std::call_once`. More importantly, benchmarks run exclusively (SEM-4).

### ST-8 Mutable public statics users set from tests · S2 · S
- **Where:** `StringMaker<float>::precision` and `StringMaker<double>::precision` (`catch_tostring.hpp:300,306`; `catch_tostring.cpp:242,248`).
- **Hazard:** a test that tweaks precision leaks the change into concurrently running tests.
- **Fix:** document them as run-global settings to be set before `Session::run()`, and make them `std::atomic<int>`.

### ST-9 Startup-only counters · S3 · S
- **Where:**
  - `catch_test_case_info.cpp:79-80` (`makeDefaultName` counter, used at registration).
  - `catch_session.cpp:149` (`alreadyInstantiated`).
- **Fix:** make the counter atomic. The Session flag is fine as it is.

### ST-10 Teardown ordering · S3 · S
- **Where:**
  - `cleanUp()` runs from `~Session` (`catch_session.cpp:183`) and from `~LeakDetector` (`internal/catch_main.cpp:17`, `internal/catch_leak_detector.cpp:36-38`).
  - It deletes the singletons and the context.
- **Hazard:** detached user threads or unjoined workers that still use Catch after cleanup hit a use-after-free.
- **Fix:**
  - Join the worker pool before cleanup.
  - Count live bindings per test context and report an error when a test ends with threads still bound to it.

---

## OS — Process-wide OS resources

### OS-1 stdout/stderr capture · **S1** · L
- **Where:**
  - **Default capture:** an `rdbuf()` swap of `std::cout`/`std::cerr`/`std::clog` (`internal/catch_output_redirect.cpp:30-59`).
  - **`CATCH_CONFIG_EXPERIMENTAL_REDIRECT`:** `dup`/`dup2` on fds 1 and 2 into temporary files (`:111-134`).
  - Both are engaged per partial run at `internal/catch_run_context.cpp:511-521`.
- **When it is active:** whenever any reporter sets `shouldRedirectStdOut`. That covers **JUnit** (`reporters/catch_reporter_junit.cpp:90`), **XML** (`catch_reporter_xml.cpp:31`), **SonarQube** (`catch_reporter_sonarqube.hpp:23`), **JSON** (`catch_reporter_json.cpp:49`) and **TeamCity** (`catch_reporter_teamcity.hpp:28`). In practice, capture is on in nearly every CI configuration.
- **Hazards:**
  - Swap and restore are not LIFO across threads, so `std::cout` can be left pointing at a destroyed pooled `ostringstream` (a use-after-free).
  - Output lands in whichever test swapped last.
  - fd-level redirection is inherently process-wide.
  - Default capture already misses C stdio. The `printf` in `tests/SelfTest/UsageTests/Tricky.tests.cpp:313-316` shows up as `loose text artifact` inside the approved JUnit and SonarQube baselines.
- **Fix:**
  - Install **one demultiplexing `streambuf`** on `cout`/`cerr`/`clog` for the whole run. It routes each write to the sink of the *calling thread's* bound test, or passes it through when the thread is unbound.
  - fd-level capture cannot be attributed per thread. Choose a policy (D6): force `--jobs 1` when it is enabled, or provide process isolation (stretch goal).
  - See [02 §6](02-target-architecture.md#6-output-capture).

### OS-2 Fatal signal / SEH handling · **S1** · L
- **Where:** `internal/catch_fatal_condition_handler.cpp`.
- **POSIX:**
  - Static `altStackMem`, `altStackSize`, `oldSigStack`, `oldSigActions[]` (`:170-173`).
  - A single-instance assert in the constructor.
  - `sigaltstack` (`:223`), a **per-thread** setting, installed only on the engaging thread.
  - `sigaction` (`:229`), which is **process-wide**.
- **Windows:**
  - `SetUnhandledExceptionFilter` (`:126,:130`).
  - A static previous filter (`:103`).
  - `SetThreadStackGuarantee` (`:110`), applied only to the thread that constructed `RunContext`.
- **Engagement:** handlers are installed and removed **around every test-case invocation** (`FatalConditionHandlerGuard`, `catch_run_context.cpp:555`).
- **The handler:**
  - It calls `reportFatal` (`:61-63`), then the single global capture's `handleFatalErrorCondition` (`catch_run_context.cpp:440-483`).
  - That drives the reporters (allocations, iostreams), emits `testRunEnded` from inside the handler, and re-raises the signal.
- **Hazards:**
  - Concurrent engage and disengage overwrite the saved "previous" handlers, so Catch's own handler gets saved as the previous one.
  - Worker threads have no alternate stack, so stack overflows go unreported.
  - The crash is attributed to whichever test the global slot points at.
  - In a multi-threaded process, a handler that allocates or locks can **deadlock** on locks held by other threads.
  - Every in-flight test's results are lost.
- **Fix:** see [02 §7](02-target-architecture.md#7-fatal-errors-crashes-signals-seh).
  - Install handlers once per run, before workers start.
  - Give each worker its own alternate stack and stack guarantee.
  - Resolve the faulting thread's context through a thread-local. Synchronous signals are delivered to the faulting thread.
  - Use a minimal async-signal-safe path first, then a bounded best-effort flush using `try_lock`.
  - Report the other in-flight tests as aborted.
  - Offer process isolation as the robust answer (decision D7).

### OS-3 Win32 console colour · S2 · S
- **Where:** `internal/catch_console_colour.cpp:110-114,149-153` (`SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), …)`).
- **Hazard:** console attributes are process-global. Concurrent writers interleave colours.
- **Fix:** all reporter output comes from the single sequencer thread (RP-1). Prefer ANSI/VT mode on Windows 10 and later.

### OS-4 Debug break (`-b`) · S3 · S
- **Where:** `internal/catch_assertion_handler.cpp:39-55` (`AssertionHandler::complete`).
- **Behaviour:** it works per thread, but other workers keep running while the debugger is stopped.
- **Fix (optional):** pause dispatch while a break is pending, and document recommending `--jobs 1` when debugging.

### OS-5 Other process-wide state touched by *tests* · S2 · —
- **What:** cwd, environment variables, locale, umask, signal masks, rlimits.
- **Status:** Catch2 does not mutate these, but tests that do will interfere with each other. Tracked as UT-2.

---

## RP — Reporter and listener pipeline

### RP-1 Reporters assume one active test and strictly nested, serial events · **S1** · L
- **Where:** the event contract is in `interfaces/catch_interfaces_reporter.hpp` and `docs/reporter-events.md:24-89`. Per-reporter mutable state that assumes one active test:

| Reporter or base | State |
|---|---|
| `StreamingReporterBase` | `currentTestCaseInfo`, positional `m_sectionStack` (`reporters/catch_reporter_streaming_base.hpp:38-55,64-68`) |
| `CumulativeReporterBase` | `m_rootSection`, `m_deepestSection`, `m_sectionStack`; asserts at `reporters/catch_reporter_cumulative_base.cpp:132,143` |
| Console | Lazily printed header built from the section stack (`reporters/catch_reporter_console.cpp:538-579`); `TablePrinter` spans 3–4 benchmark events |
| XML | `XmlWriter` tag stack and `m_sectionDepth` |
| JSON | Three writer stacks, asserted at every step |
| TAP | Running `counter` |
| TeamCity | Header-printed flag and timer |
| JUnit | `m_okToFail` set in `testCaseStarting` |
| `MultiReporter` | Plain, unlocked loops (`reporters/catch_reporter_multi.cpp`) |

- **Fix:**
  - Record each test case's events into a per-test **EventRecorder**.
  - A **Sequencer** replays completed tests to the existing reporter chain on one thread, in canonical order.
  - Existing reporters then keep working, and output is deterministic and identical to `--jobs 1`. See [02 §5](02-target-architecture.md#5-event-pipeline-record--sequence--replay).

### RP-2 Borrowed / lazy payloads cannot cross threads or outlive the assertion · S1 · M
- **Where:**
  - `LazyExpression` points at a stack-allocated expression (`internal/catch_lazy_expr.hpp:22`; set at `internal/catch_run_context.cpp:607`).
  - `AssertionStats` references `m_messages` (`:290`).
  - `StringRef` payloads in benchmark and fatal events (`benchmark/catch_benchmark.hpp:92` passes a temporary).
- **Fix:**
  - The recorder deep-copies every event payload and eagerly expands expressions. `CumulativeReporterBase` already does this at `reporters/catch_reporter_cumulative_base.cpp:103-117`.
  - Only record what at least one sink will consume: use the union of preferences, and keep the passing-assertion fast path when no sink wants passes.

### RP-3 Reporters time test cases with their own timers · S2 · S
- **Where:**
  - XML `m_testCaseTimer` (`reporters/catch_reporter_xml.cpp:75,198`).
  - TeamCity `m_testTimer` (`reporters/catch_reporter_teamcity.cpp:133,153`).
  - JUnit `suiteTimer` is run-level, so it becomes parallel wall-clock time. That is acceptable.
- **Hazard:** under replay these measure replay time, which is roughly zero.
- **Fix:**
  - Add `durationInSeconds` (measured on the worker) to `TestCaseStats`; today it has none (`interfaces/catch_interfaces_reporter.hpp:85-97`).
  - Migrate the XML and TeamCity reporters to use it.

### RP-4 Running run-totals inside `AssertionStats` · S2 · S
- **Where:** `internal/catch_run_context.cpp:290`. The console reporter reads it.
- **Fix:** the Sequencer recomputes running totals in canonical order during replay. Output stays byte-identical.

### RP-5 `MultiReporter` re-emits captured output to the real stdout · S2 · S
- **Where:** `reporters/catch_reporter_multi.cpp:136-151`. It writes to `Catch::cout()`/`cerr()` before `testCasePartialEnded` when a non-capturing reporter is present.
- **Fix:** this now happens on the sequencer thread. The demux `streambuf` passes unbound threads straight through, so the output reaches the terminal, not a test's capture.

### RP-6 Listener semantics · S2 · M
- **Where:** `docs/event-listeners.md`. Listeners exist to perform side effects *while* a test runs, for example per-test global setup in `testCaseStarting`. They are always notified before reporters (`reporters/catch_reporter_multi.cpp:24-28`).
- **Hazard:** under replay, test-level listener callbacks no longer happen while the test runs.
- **Fix:**
  - Run-level events (`testRunStarting`/`testRunEnded`) stay live and serial.
  - Add an opt-in `IConcurrentEventListener` that receives test-level events live on the worker thread and must be thread-safe.
  - For legacy listeners that override test-level events, decide (D9) between "forces `--jobs 1`", the recommended default, and "replayed, documented change".

### RP-7 Loss of live progress · S2 · M (optional)
- **What is lost:**
  - The console reporter prints a failure only after its test completes.
  - The benchmark progress flush happens at replay time.
  - TeamCity loses its IDE "running" state.
- **Fix (optional):**
  - A live status line on stderr in console mode.
  - A "completion order" live mode, with TeamCity `flowId` support (TeamCity's mechanism for parallel streams; nothing in this tree emits it today).

### RP-8 Pre-existing violations of the event contract · S2 · S–M
These are bugs in today's serial code that the replay layer would have to reproduce exactly to keep baselines byte-identical:

1. `assertionStarting` without a matching `assertionEnded` for passing assertions (`internal/catch_run_context.cpp:583-586`), and through `MultiReporter`'s per-member filter (`reporters/catch_reporter_multi.cpp:117-128`).
2. **Deferred `sectionEnded` events come out outer-first.**
   - Unwinding pushes inner sections before outer ones, and `handleUnfinishedSections` iterates in reverse (`internal/catch_run_context.cpp:564-573`).
   - Reporters pop positionally, so section stats are swapped.
   - This is visible in the approved baseline: `tests/SelfTest/Baselines/junit.sw.approved.txt:1920` names the path `.../B2/B` instead of `.../B/B2`.
3. The fatal path never emits `testCasePartialEnded`, and it emits `testRunEnded` from inside the signal handler (`internal/catch_run_context.cpp:440-483`).

- **Fix:** correct these, with baseline re-approval, in Phase 2, **before** building replay.

### RP-9 Head-of-line buffering vs crashes · S2 · (part of OS-2)
- **Hazard:** with ordered replay, a crash can strand finished-but-not-yet-replayed tests in memory.
- **Fix:** the crash path must flush every completed buffer in order before reporting the crash.

### RP-10 Shared output sinks · S3 · S
- **Where:** `CoutStream`/`CerrStream` capture `std::cout`'s `rdbuf()` when they are constructed (`internal/catch_istream.cpp:96,109`). Several reporters may target the same sink.
- **Status:** safe when all reporter writes happen on the sequencer thread.
- **Fix:** construct reporter streams before installing the demux buffer, or route them explicitly to the original buffers.

---

## SEM — Run semantics and CLI behaviour

### SEM-1 Order and determinism · S2 · S
- **Where:**
  - `--order decl|lex|rand` and `--rng-seed` sort the tests (`internal/catch_test_case_registry_impl.cpp`).
  - `TestGroup` keeps them in a `std::set<TestCaseHandle const*>`, ordered by pointer into the sorted vector (`catch_session.cpp:87-104,135`).
- **Under concurrency:** the order becomes the *dispatch* order. Report order must stay canonical.
- **Hazard:** order-dependent suites, where test A leaves state that B relies on, start failing intermittently.
- **Fix:** canonical report order by default, and dispatch in canonical order. Longest-first scheduling is a later optimisation.

### SEM-2 `--abort` / `-x N` · S2 · S–M
- **Where:** `RunContext::aborting()` (`internal/catch_run_context.cpp:496-498`), `populateReaction` (`:658-661`), `TestGroup::execute` (`catch_session.cpp:110-113`).
- **Under concurrency:**
  - The failure count is global.
  - Which tests ran before the abort becomes nondeterministic.
  - In-flight tests have to be cancelled cooperatively.
- **Fix:**
  - An atomic failure counter in the coordinator.
  - Stop dispatching when the threshold is reached.
  - In-flight tests abort at their next assertion (through `shouldThrow`).
  - Report `skipTest` for tests that were never dispatched.
  - Document that the `-x` result set is deterministic only at `--jobs 1`. This affects the `console.swa4` approval baseline.

### SEM-3 Sharding · — · S
- **Where:** `internal/catch_sharding.hpp`; applied at `catch_session.cpp:104`.
- **Status:** orthogonal. Shard first, then parallelise within the shard. `extras/CatchShardTests.cmake` keeps working.

### SEM-4 Benchmarks · S2 · S
- **Hazard:**
  - Concurrent load invalidates timings.
  - Console benchmark rows span several events.
  - ST-7.
- **Problem:** a test's use of `BENCHMARK` is only discovered at runtime.
- **Policy:**
  - Tests tagged `[!benchmark]` (already a special tag, hidden by default; `catch_test_case_info.cpp:61-62`) or `[!exclusive]` run in an **exclusive** phase with no other workers active.
  - An untagged `BENCHMARK` reached during a parallel phase emits a warning on its result.

### SEM-5 Expected-failure tags · — · S
- **Where:** `[!shouldfail]`/`[!mayfail]` adjust per-test totals (`internal/catch_run_context.cpp:251-255`).
- **Status:** works unchanged once totals are per-test (EX-4).

### SEM-6 Durations, `--min-duration`, `-d yes` · S2 · S
- **Status:** per-test wall time inflates under contention. Document it; optionally also report CPU time.

### SEM-7 Exit code · — · S
- **Status:** `min(255, failed assertions)` (`catch_session.cpp:354`) is an aggregate, so it is unchanged.

### SEM-8 Unit of parallelism = test case · S3 · XL (stretch)
- **What:** a test case with N section leaves or generator values runs N partial runs, sequentially.
- **Why parallelising them is hard:** the paths are only discovered by executing the test, and each run re-executes shared setup.
- **Status:** a stretch goal. It needs a path-discovery pass and a cloneable tracker. Not needed for 1.0.

### SEM-9 Startup and shutdown interactions · S3 · S
- **What:** `--wait-for-keypress` (`catch_session.cpp:260-268`), `--list-*`, `--libidentify`.
- **Status:** unaffected, because they run before or after the parallel phase. `--libidentify` output must change on detachment (see 04).

---

## API — User-facing API used from several threads

(Concurrent test cases each run on their own worker, so they do not need this
section. It applies to tests that spawn their own threads, which the
architecture should support as well.)

### API-1 Assertions from spawned threads · S1 · M
- **Status today:** unsupported (`docs/limitations.md:60-92`, "*We currently do not plan to support thread-safe assertions*") and racy (see the TSan evidence).
- **Hazard after EX-1:** a thread-local binding means an unbound spawned thread has *no* context.
- **Fix:**
  - An explicit propagation API (capture a handle, bind it in the child thread) plus helper wrappers. See [02 §4](02-target-architecture.md#4-assertions-from-user-spawned-threads).
  - A policy for unbound threads (D5).
  - The assertion path within one context synchronises on a per-context mutex.

### API-2 `REQUIRE`/`FAIL`/`SKIP` throw on the calling thread · S1 · S
- **Where:** `internal/catch_assertion_handler.cpp:39-55`.
- **Hazard:** in a `std::thread`, an uncaught `TestFailureException` calls `std::terminate`, which ends the whole parallel run.
- **Fix:**
  - The helper wrappers catch these exceptions and mark the test aborted.
  - On `join()`, they rethrow on the test's thread.
  - Document behaviour for raw `std::thread`.

### API-3 `CHECKED_IF` / `CHECKED_ELSE` read "last assertion passed" · S2 · S
- **Where:** `internal/catch_test_macro_impl.hpp:62,67`.
- **Fix:** keep this state per thread inside the context.

### API-4 `INFO` / `CAPTURE` / `UNSCOPED_INFO` scope · S2 · (EX-6)
- **Fix:** message stacks are per thread. A child thread does not inherit the parent's `INFO`s. This is a decision; inheriting a snapshot at bind time is possible.

### API-5 `SECTION` / `GENERATE` / `BENCHMARK` off the owner thread · S2 · S
- **Fix:** a hard error with a clear message (EX-5).

### API-6 Shared user objects with internal caches · S1 (user-triggered) · S
- **Where:**
  - Matchers' `mutable m_cachedToString` (`matchers/catch_matchers.hpp:35`).
  - Generators' `mutable m_stringReprCache` (`interfaces/catch_interfaces_generatortracker.hpp:22`).
  - `RepeatGenerator::m_returned` (`generators/catch_generators_adapters.hpp:103`).
  - `AssertionResultData::reconstructedExpression` (`catch_assertion_result.hpp:28`).
- **Fix:**
  - Document "do not share a matcher instance across threads".
  - Optionally make `MatcherUntypedBase::toString` thread-safe with `std::call_once`.
  - `AssertionResult` objects stay private to the recorder.

### API-7 Deprecated `getLastResult()` / `getCurrentTestName()` · S2 · S
- **Where:** `interfaces/catch_interfaces_capture.hpp:101-104`.
- **Fix:** give them per-thread semantics, add `currentTestCaseInfo()`, and keep the deprecated forms for the compatibility period.

### API-8 Exceptions disabled (`CATCH_CONFIG_DISABLE_EXCEPTIONS`) · S2 · S
- **Status:** a failing `REQUIRE` calls `std::terminate`, as it does today. With concurrency it also kills in-flight tests.
- **Fix:** document it, and default to `--jobs 1` in that configuration.

---

## INF — Build, test infrastructure and tooling

| ID | Finding | Where | Work |
|---|---|---|---|
| INF-1 | **No threading dependency in any build system.** The C++14 floor is enforced only via `target_compile_features`. `USE_ASYNC` is gated on `_REENTRANT`. | `src/CMakeLists.txt:365-369`; `internal/catch_compiler_capabilities.hpp:234-237,356-358`; root `meson.build`, `BUILD.bazel`, `conanfile.py`, `CMake/catch2*.pc.in` | `find_package(Threads)` and a PUBLIC link; meson `dependency('threads')`; Bazel `-pthread`; Conan `system_libs`; pkg-config. Add a `CATCH_CONFIG_NO_THREADS` opt-out for single-threaded platforms. **M** |
| INF-2 | **No sanitizers in CI.** The only dynamic checking is one Valgrind memcheck job. | `.github/workflows/linux-other-builds.yml` | Add TSan, ASan+UBSan and stress jobs; see the roadmap. **M** |
| INF-3 | **Approval tests assume serial output.** There are 20 baselines (about 110k lines). Only paths, line numbers, durations, hex values and timestamps are normalised, not order. `console.swa4` uses `-x 4`. | `tools/scripts/approvalTests.py:50-149,209-238`; `tests/SelfTest/Baselines/` | Run every approval at `--jobs 1` *and* `--jobs N` and require identical output (canonical replay). **M** |
| INF-4 | **`ValidatingTestListener`** enforces serial nesting in every SelfTest run. | `tests/SelfTest/TestRegistrations.cpp:33-180` | Keep it for the replay stream. Add a validator for the concurrent-listener interface. **S** |
| INF-5 | **ExtraTests encode serial semantics:** X20 (global static counter, ordered), X21 (exact partial-event sequence), X22, X24/X25/X27 (capture semantics), X28 (listeners-before-reporters through global statics), X36 (crash under JUnit). | `tests/ExtraTests/CMakeLists.txt` and the sources | Add `--jobs N` variants and a crash-in-parallel test. **M** |
| INF-6 | **SelfTest's own static state.** | `tests/SelfTest/IntrospectiveTests/PartTracker.tests.cpp:164-165`; `UsageTests/Generators.tests.cpp:269`; `IntrospectiveTests/InternalBenchmark.tests.cpp:46-68` | Make these re-entrant or tag them `[!serial]` so SelfTest can dog-food parallel mode. **S** |
| INF-7 | **CMake test discovery runs one process per test case.** In-process parallelism gives no speed-up in that mode. | `extras/CatchAddTests.cmake:69-74,153-161`; `extras/Catch.cmake` | Add a per-binary/per-shard registration mode that passes `--jobs` and sets the CTest `PROCESSORS` property, to avoid oversubscription under `ctest -j`. **M** |
| INF-8 | **Amalgamated build and generated config header.** | `tools/scripts/generateAmalgamatedFiles.py`; `catch_user_config.hpp.in` | Keep new sources amalgamation-safe and add the new `CATCH_CONFIG_*` toggles. **S** |
| INF-9 | **Docs state the opposite of the new design.** | `docs/limitations.md:60-112`; `docs/assertions.md:147-150`; `docs/reporters.md:55-70`; `docs/event-listeners.md:10-11`; `docs/command-line.md` | Rewrite. **M** |

---

## UT — Hazards in consumers' test code

These are not framework defects. They are the main *migration* risk: a suite
that is green when serial and flaky when parallel. The framework can only
detect them partly, so the migration guide ([05](05-migration-guide.md))
handles them through opt-in rollout, tagging and sanitizer runs.

| ID | Hazard | Typical symptom | Mitigation offered by the framework |
|---|---|---|---|
| UT-1 | Global or static mutable state in tests or the code under test: singletons, caches, registries, static counters | Intermittent wrong values | `[!serial]`, resource locks, TSan runs |
| UT-2 | Process-wide environment: `chdir`, `setenv`, `setlocale`/`std::locale::global`, umask, signal handlers, rlimits, clock mocking | Failures in *other* tests | `[!exclusive]`; documented checklist |
| UT-3 | Fixed paths, temp files, ports, IPC names, shared databases | Collisions, "address in use" | Per-test unique temp-directory and name helpers (new API, P4.8); bind to port 0; resource locks |
| UT-4 | Order-dependent tests | Fail under `--order rand` | Run `--order rand` with several seeds *before* enabling `--jobs` |
| UT-5 | Asserting on captured stdout; printing through `printf`/`write(2)` | Output missing or misattributed | Documented: only C++ streams are captured per thread |
| UT-6 | Timing-sensitive tests (sleeps, performance thresholds) | Timeouts under load | `[!exclusive]`; warn on `--min-duration` use |
| UT-7 | Tests that already assert from spawned threads (racy today) | Now well-defined if bound; error if unbound | Thread helpers (API-1) |
| UT-8 | Tests that use many threads or a lot of memory | Oversubscription, out-of-memory | `--jobs`; weight tags (stretch) |
| UT-9 | Third-party libraries with thread-unsafe global initialisation | Crashes during the first parallel tests | Initialise in a custom `main` before `Session::run()` |
| UT-10 | Listeners or custom `main`s doing per-test global setup | Setup races or is skipped | RP-6 policy; `IConcurrentEventListener` |
