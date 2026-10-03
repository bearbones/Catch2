# 06 — Decisions and risks

## Decision register

Each decision needs an owner and an outcome by the phase in the "Needed by"
column. Phase 0's decision workshop (P0.1) should close everything marked
**P0**. The "Recommendation" column is this plan's proposal. The "Outcome"
column is for the organisation to fill in.

| ID | Decision | Options | Recommendation | Needed by | Outcome |
|---|---|---|---|---|---|
| **D1** | Project identity and namespace strategy | A) source-compatible: keep the `Catch` namespace and macros, add an inline ABI namespace, new package and include root · B) full rebrand with a compat shim | **A** for 1.x. Revisit B for 2.0 with an automated rewriter ([04 §2](04-detachment.md#2-identity-names-namespaces-and-the-source-compatibility-question)) | P0 | |
| **D2** | C++ language baseline | C++14 (as upstream) · C++17 · C++20 | **C++17**. `std::optional`/`std::variant` simplify the event records, and inline variables and `std::shared_mutex` help too. All the concurrency primitives needed exist in C++14, so choose C++14 only if a consuming platform requires it. C++20 (`jthread`, `stop_token`, `latch`) only if every consumer toolchain supports it. | P0 | |
| **D3** | Fork point | Detach from this tree (3.6.0+) · fast-forward to the latest upstream release first | **Fast-forward first.** It is a strict fast-forward today (308 commits, no local changes), and it brings upstream's thread-safety and redirect work. | P0 | |
| **D4** | Default concurrency and opt-in model | Run-level opt-in (`--jobs`, default 1) with per-test opt-out tags · per-test opt-in (`[!parallel]`) · parallel by default | **Run-level opt-in, default 1, in 1.0**, plus `--parallel tagged` for teams that want per-test opt-in. Revisit defaulting to `auto` in 2.0, once migration data exists. | P2 | |
| **D5** | Assertions from **unbound** threads | Hard error always · fall back to the single running test at `--jobs 1` and error at `>1` · silently attribute to "some" test | **Fallback at `--jobs 1` with a warning; error at `>1`.** This keeps current (upstream 3.9+) behaviour working in serial mode, and it never misattributes. | P3 | |
| **D6** | Output that cannot be attributed per thread (C stdio, fd-level writes) | Uncaptured pass-through · clamp `--jobs` to 1 when fd capture is configured · fd capture only during the exclusive phase | **Pass-through by default. fd capture is allowed only for `[!exclusive]` tests, or at `--jobs 1`.** Process isolation is the long-term answer. | P3 | |
| **D7** | Crash policy | In-process best effort only · in-process best effort plus a post-1.0 process-isolation backend | **Both.** Ship best effort in 1.0 (02 §7). Put the process backend first on the post-1.0 list. | P2 | |
| **D8** | Report order | Canonical (deterministic) · completion order | **Canonical by default.** Completion order is opt-in, for interactive and IDE use. | P3 | |
| **D9** | Legacy listeners that override test-level events | Clamp `--jobs` to 1 · replay them with a documented semantic change | **Clamp, with a clear notice** and an opt-in flag to accept replay semantics. | P3 | |
| **D10** | Supported platforms and toolchains | As upstream (GCC 5+, Clang 6+, MSVC 2017+, …) · trimmed to the organisation's actual toolchains | **Trim to the organisation's toolchains** plus a no-threads build. Every supported toolchain costs CI time and workarounds. | P0 | |
| **D11** | Output-format identifiers (`Catch2TestRun`, `catch2-version`, `--libidentify`) | Keep · rename · keep plus a `framework` field and a reporter option for new names | **Keep plus a `framework` field** in 1.x ([04 §2](04-detachment.md#output-format-identifiers-d11)) | P1 | |
| **D12** | Build systems to support | CMake, Meson, Bazel, Conan, amalgamated (as upstream) · subset | **Whatever the organisation consumes** (from the Stage 0 inventory). Drop the rest to save maintenance. Keep the amalgamated build if any team vendors it. | P0 | |
| **D13** | Upstream-watch commitment | None · release-by-release triage ([04 §6](04-detachment.md#6-relationship-with-upstream-after-the-cut)) | **Release-by-release triage**, about 0.5 engineer-day per upstream release. | P1 | |

## Risk register

Likelihood and impact are rated **H**igh, **M**edium or **L**ow.

| ID | Risk | L | I | Mitigation | Early-warning signal |
|---|---|---|---|---|---|
| **R1** | **Hidden shared state in consumers' tests** causes flaky failures after `--jobs` is enabled, eroding trust in the framework | H | H | Opt-in parallelism; the Stage 2 hygiene checks; tags rather than retries; TSan builds of consumer suites; fleet-wide env-var rollback | Flake rate rises on any suite after Stage 3 |
| **R2** | **Assertion fast-path regression** (thread-locals, recorder) slows every suite, including serial ones | M | H | Thread confinement (no locks unless shared); perf budgets gate from M2; microbenchmarks per PR. Upstream's 4–40% shows what a naive approach costs. | Perf job breaches budget |
| **R3** | **A crash in one test takes all in-flight tests down**, with weaker attribution than serial runs | H | M | Three-stage handler with a bounded flush; aborted tests reported; premature-exit sentinel; process-isolation backend post-1.0; `--jobs 1` for crash-prone suites | Crash-matrix failures; teams pinning `--jobs 1` |
| **R4** | **Signal-handler deadlock** in a multi-threaded process (another thread holds malloc, iostream or our own locks) | M | M | Async-signal-safe stage 1 always runs; stage 2 uses only `try_lock` with a time bound; TLS touched at thread start with initial-exec model | Hangs in the crash matrix |
| **R5** | **Report differences break downstream tooling** (dashboards, Sonar, JUnit parsers) | M | H | Canonical order; byte-identity gates at every `--jobs` value; output identifiers kept (D11); pilot diffs in Stage 1 | Stage 1 output diffs outside the expected list |
| **R6** | **Platform quirks with `thread_local`**: MSVC DLL export, MinGW emutls cost, old Apple toolchains | M | M | DLL-safe accessor (02 §3); platform matrix in P5.4; trim platforms (D10) | Platform-job failures, perf deltas on MinGW |
| **R7** | **Divergence from upstream** loses valuable upstream fixes | H | L–M | Fast-forward before detaching (D3); release-by-release triage (D13); `UPSTREAM.md` ledger | Triage backlog grows past one release |
| **R8** | **Scope creep** (section-path parallelism, process isolation, live reporting) delays 1.0 | M | M | Non-goals N1–N4 are explicit; post-1.0 list; gate reviews by the tech lead | Work on post-1.0 items before M6 |
| **R9** | **Approval-baseline churn hides regressions** (re-approving large diffs without review) | M | H | Re-approval only in P2.5, with a reviewed diff list; byte-identity across `--jobs` from M4; no bulk re-approval after that | PRs touching `Baselines/` outside P2.5 |
| **R10** | **Memory growth from buffered records** when a slow early test holds back replay, especially with `-s` | M | M | Lookahead limit (`K × jobs`); head-of-line streaming; peak-memory budget (≤ 2×) | Memory job breaches budget |
| **R11** | **Listener ecosystem friction**: custom listeners clamp `--jobs` to 1, so suites see no benefit | M | M | Stage 0 inventory finds them early; `IConcurrentEventListener` with a porting guide; migration-lead support | Many suites stuck at 1 job due to clamp notices |
| **R12** | **Key-person risk.** The concurrency core is understood by one or two people. | M | H | Two-reviewer rule; design docs (this folder) kept current; rotate ownership between Engineers A and B across phases | Review latency on core PRs |
| **R13** | **CTest integration mismatch.** Teams keep one-process-per-test-case discovery and see no speed-up, so they conclude the project failed. | M | M | Migration guide Stage 3 table; new discovery modes in P7.1; publish before/after numbers from the pilots | Migrated suites with no wall-clock change |
