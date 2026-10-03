# Parallel test execution and detachment from Catch2: plan of record

**Status:** draft for organisational review · **Analysed tree:** Catch2
v3.6.0+, devel commit `33e24b1` · **Upstream compared:** v3.16.0 (2026-08-25)
and `devel` at `222e2339` · **Date:** 2026-10-03

## What this is

This folder contains three things:

1. An inventory of everything in Catch2 that prevents running test cases
   concurrently on multiple threads, and assertions from threads a test spawns.
2. The architecture that replaces those constructs.
3. A phased plan to build the result as an independent project and to migrate
   the organisation onto it.

| Doc | For | Contents |
|---|---|---|
| [01-inventory.md](01-inventory.md) | Engineers | 67 itemised constructs with `file:line`, severity, fix and effort. Includes upstream's current status for each. |
| [02-target-architecture.md](02-target-architecture.md) | Engineers, tech lead | Execution model, context binding, record/sequence/replay, capture, crash handling, scheduling, budgets, alternatives. |
| [03-roadmap.md](03-roadmap.md) | Management, tech lead | 8 phases, 46 work items, estimates, gates, staffing, testing strategy. |
| [work-breakdown.csv](work-breakdown.csv) | Project management | The same work items with dependencies, for import into a tracker. |
| [04-detachment.md](04-detachment.md) | Tech lead, legal, release | Identity and namespace strategy, renaming surfaces, licence, upstream relationship. |
| [05-migration-guide.md](05-migration-guide.md) | Migration lead, consuming teams | Stages, waves, rollback, metrics. |
| [06-risks-and-decisions.md](06-risks-and-decisions.md) | Decision makers | 13 decisions with recommendations; 13 risks with mitigations. |
| [evidence/](evidence/README.md) | Anyone sceptical | ThreadSanitizer run that confirms the races on this tree. |

## Findings

1. **The problem is structural.**
   - Catch2 has one process-global "current test" slot, which points at one
     `RunContext`. That object holds both run-level and per-test state.
   - Every macro (`REQUIRE`, `SECTION`, `INFO`, `GENERATE`, `BENCHMARK`) finds
     it through a global lookup.
   - Reporters assume one active test and strictly nested events.
   - Most of the 67 inventory items follow from these facts. Splitting the run
     from the test and binding tests to threads resolves the bulk of them.
2. **Three areas carry real engineering risk.**
   - **Output capture:** `std::cout` buffer swaps and `dup2` on fds 1 and 2 are
     process-wide. Capture is on whenever JUnit, XML, SonarQube, JSON or TeamCity
     reporting is used, which covers most CI.
   - **Crash handling:** signal handlers and the SEH filter are installed per
     test, and the handler reports from inside itself.
   - **Deterministic reporting** under concurrent execution.
3. **The races are real.** A four-thread `CHECK`/`INFO` test against the
   unmodified tree produced 106–130 ThreadSanitizer reports, a heap-use-after-free
   and a SEGV.
4. **Upstream fixed part of the problem but rules out the rest.**
   - Since this tree was cut, upstream added *opt-in* thread-safe assertions
     and messages (v3.9–3.12), at a measured 4–40% cost.
   - Upstream still runs test cases strictly serially, and its docs say there
     are no plans to change that.
   - This tree is a **strict ancestor** of upstream, 308 commits behind. It can
     be fast-forwarded today at zero conflict cost.
5. **Detaching is justified.** Upstream will not take the core feature, and the
   changes land in exactly the code upstream rewrites most often. A
   source-compatible detachment keeps consumer test code unchanged:
   - keep the `Catch` namespace and the macros;
   - add an inline ABI namespace;
   - give the project a new package and include root.

## Plan

**Architecture** ([02](02-target-architecture.md)):

- **Run/test split.** A `RunCoordinator` per run; a `TestExecutionContext` per
  running test case, bound thread-locally to its worker.
- **Thread confinement.** No locks on the assertion path unless a test shares
  its context with threads it spawned. This avoids upstream's global cost.
- **Record → sequence → replay.**
  - Each test's events are recorded with deep copies.
  - A sequencer replays them in canonical order on one thread.
  - Existing reporters keep working, and output at `--jobs N` is
    **byte-identical** to `--jobs 1`.
  - Serial and parallel modes share this one code path.
- **Capture.** A single demultiplexing `streambuf` attributes C++ stream output
  to the writing thread's test.
- **Crashes.** Signal/SEH handlers are installed once per run. The handler works
  in three stages: async-signal-safe note, bounded flush, re-raise.
- **Scheduling.** `--jobs N|auto`, with `[!exclusive]` and `[!lock:<name>]`
  tags. Benchmarks run exclusively.
- **Determinism.** A per-test RNG seeded exactly as today, so random generators
  are bit-for-bit identical at any `--jobs` value.

**Schedule** ([03](03-roadmap.md)):

- **Effort:** ≈ 47.5–56.5 engineer-weeks, ≈ 59–71 with contingency.
- **Duration:** with three engineers, roughly **6–8 months to 1.0**, and a
  parallel beta around month 5.
- **Migration** runs in waves from the beta: pilot, early adopters, general,
  clean-up ([05](05-migration-guide.md)).

## Decisions needed now (Phase 0)

These have to be made before engineering starts in earnest. The full register,
with options and recommendations, is in [06](06-risks-and-decisions.md).

| ID | Decision | Recommendation |
|---|---|---|
| D1 | Identity and namespace strategy | Source-compatible: keep `Catch`, add an inline ABI namespace, new package and include root |
| D2 | C++ baseline | C++17, unless a consuming platform requires C++14 |
| D3 | Fork point | Fast-forward to latest upstream **before** detaching |
| D10 | Supported toolchains and platforms | Trim to what the organisation uses, plus a no-threads build |
| D12 | Build systems to support | Whatever the Stage 0 consumer inventory shows |

## Method and limits of this analysis

**How the analysis was done:**

- Read the execution core (session, run context, tracker, sections, messages,
  assertion handler, generators, benchmarks, output redirect, fatal handler,
  RNG, registries) directly at the cited lines.
- Swept the whole `src/catch2` tree for process-global state, non-reentrant
  libc calls, process-wide side effects and lazy caches.
- Reviewed every reporter and the listener and event contract.
- Surveyed the build, packaging, test and CI infrastructure.
- Compared against the upstream `devel` source and docs, and against prior art:
  Rust libtest, doctest, GoogleTest, Boost.Test, JUnit 5, NUnit, xUnit.net,
  pytest-xdist and Go.
- Compile-checked the inline-namespace compatibility claim with GCC 13 and
  Clang 18.

**Limits:**

- Estimates are planning-grade ranges, not commitments.
- Line references are anchored to `33e24b1` and move after the recommended
  rebase. Item IDs stay stable (P0.3).
- No prototype of the scheduler or sequencer was built.
- The performance budgets are targets, informed by upstream's measurements,
  not yet measured on this design.
