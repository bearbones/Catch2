# 05 — Organisational migration plan

How consuming teams move from Catch2 to the new framework, and from serial to
parallel execution, without losing confidence in their test results. It is
written for the migration lead and for the teams that own the test suites.

## Principles

1. **Two independent switches.** *Which framework* and *how many jobs* are
   separate switches, and they are flipped at different times. A suite first
   moves to the new framework at `--jobs 1`, which by design behaves exactly
   like Catch2. Only once it is green and stable does it get `--jobs > 1`.
2. **One-flag rollback at every stage.** Set `--jobs 1`, or set the
   `<PREFIX>_JOBS=1` environment variable fleet-wide. Nothing in stages 1–3
   needs a source revert.
3. **A flaky test is information, not noise.** A test that fails only under
   `--jobs N` is reporting shared state. Tag it (`[!lock:…]`/`[!exclusive]`) and
   file the fix. **Never** retry it into green.
4. **Measure.** Track wall-clock time, flake rate and TSan findings per suite,
   before and after.

## Stage 0 — Inventory consumers (migration lead, before M1)

For every test binary in the organisation, collect:

| Item | Why it matters |
|---|---|
| Catch2 version and how it is consumed (CMake `find_package`, FetchContent, vendored amalgamated, Conan/vcpkg, Bazel) | Determines the packaging path |
| Reporters used in CI (JUnit, XML, SonarQube, TeamCity, …) and which tools parse the output | Output identifiers (D11), capture behaviour |
| Test registration: `catch_discover_tests` (one CTest test per case), sharding helpers, or one CTest test per binary | Decides whether in-process parallelism helps at all (see Stage 3) |
| Custom `main`, custom reporters, **event listeners** (especially ones overriding test-level events) | Listener policy (RP-6/D9) can clamp `--jobs` |
| `CATCH_CONFIG_*` macros in use | Compat translation coverage |
| `StringMaker` specialisations, exception translators, custom matchers | Covered by Option A; needed for Option B |
| Tests that spawn threads and assert from them | Will need `Catch::thread` (Stage 4) |
| Uses of `printf` / stdio in tests that matter for captured output | C stdio is not captured per test in parallel mode |
| Current suite wall-clock time and flake rate | Baseline for success metrics |

A short script can collect most of this: grep for the patterns above, plus
`--list-tests`/`--list-reporters`/`--list-listeners` from each binary.

## Stage 1 — Drop-in swap at `--jobs 1` (from M1)

1. **Switch the dependency.** Change `find_package(Catch2 …)` /
   `Catch2::Catch2WithMain` to the new package, linking
   `<proj>::catch2_compat` so `<catch2/…>` includes keep working.
2. **Switch the CMake helpers.** `catch_discover_tests` still works as a
   wrapper. Switch to `<proj>_discover_tests` when convenient.
3. **Run the suite in CI next to the old one** for at least one week, with
   identical arguments.
4. **Diff the JUnit/XML output** of the two runs, after normalising durations
   and timestamps. The only expected differences are:
   - the framework name and version fields (D11);
   - the RP-8 fixes. A section path that used to be reported reversed on
     exceptional exit, such as `…/B2/B`, is now `…/B/B2`. A crash now emits the
     partial-run end;
   - anything that differs between the Catch2 version you were on and the
     upstream release the framework is based on. Get this from the upstream
     release notes. Teams on an old 3.x will see, for example, the 3.7.1 exit
     codes and the 3.9.0 change of default order to random.
5. **Exit criterion:** two weeks of identical pass/fail results, and
   output diffs limited to the list above.

## Stage 2 — Parallel-readiness hygiene (still `--jobs 1`)

| Check | How | What a failure means |
|---|---|---|
| **Order independence** | Run with `--order rand --rng-seed <s>` for ≥ 5 different seeds | A test depends on state left by another. Fix it, or `[!lock:<resource>]` both tests. |
| **Data races in tests and code under test** | Build the test binary with `-fsanitize=thread`. Run at `--jobs 1`, then `--jobs 2` once available | TSan reports. Fix, or tag `[!exclusive]` with a ticket. |
| **Process-global mutations** | `grep` for `setenv`, `putenv`, `chdir`, `setlocale`, `std::locale::global`, `signal(`, `sigaction`, `umask`, `freopen`, `std::cout.rdbuf(` | Tag `[!exclusive]` or refactor (UT-2). |
| **Fixed resources** | Look for hard-coded temp paths, ports, IPC names, database names | Use the framework's per-test temp directory and unique-name helpers. For ports, bind to port 0 and read back the assigned port. Otherwise use `[!lock:<name>]` (UT-3). |
| **Timing-sensitive tests** | Sleeps, `--min-duration` thresholds, performance assertions | `[!exclusive]`, or move them to a benchmark binary (UT-6). |
| **Listeners** | `--list-listeners`, then read each listener's overrides | Test-level overrides clamp `--jobs` to 1 until ported to `IConcurrentEventListener` (Stage 4). |

Exit criterion: all of the above clean or tagged, with tickets for the tagged
items.

## Stage 3 — Enable parallelism (from M4 beta)

1. **Pick the execution shape:**

| Current registration | Recommended shape | Why |
|---|---|---|
| One CTest test per binary | `--jobs auto` on that test, plus CTest `PROCESSORS` = jobs | The simplest big win. |
| `catch_discover_tests` (one CTest test per test case) | Switch to `<proj>_discover_tests(... MODE binary JOBS auto)` (or `MODE shard SHARDS k JOBS j`) | Per-test-case processes see no in-process speed-up and pay start-up cost for every test. Keep the old mode only if you need CTest's per-test timeouts or per-test filtering in dashboards. |
| Sharding helper | Keep the shards and add `--jobs` within each one | Shards × jobs ≤ cores. |

2. **Start at `--jobs 2`,** then `auto`. Watch the flake rate daily for two
   weeks.
3. **Handle new failures** under principle 3: tag, ticket, fix. Do not retry.
4. **Exit criterion:** the flake rate is not above the serial baseline over
   4 weeks, and the wall-clock improvement is recorded.

## Stage 4 — Adopt the new APIs

- **Threads that assert.** Replace `std::thread` with `Catch::thread` (or bind
  with `ScopedTestContext`) in tests that assert from spawned threads. Under
  `--jobs > 1`, unbound threads that assert produce a framework error (D5).
- **Listeners.** Port listeners that do per-test setup or teardown to
  `IConcurrentEventListener`.
- **Unique resources.** Replace hand-rolled unique temp directories and names
  with the framework helpers.
- **Remove obsolete tags.** Drop `[!exclusive]` and `[!lock]` from tests whose
  tickets have been fixed.

## Stage 5 — Remove compatibility shims (1.x → 2.0 window)

- Stop linking `<proj>::catch2_compat`, and switch includes to `<proj/…>`.
  A provided script does the mechanical rewrite.
- Replace deprecated `CATCH_CONFIG_*` spellings, if the decision on D1
  introduced a new prefix.
- The compat target is removed no earlier than the next major version.
  Announce this 6 months in advance.

## Wave plan

| Wave | Who | Entry | Exit |
|---|---|---|---|
| **Pilot** (2–3 suites) | Volunteers with *different* shapes: one large unit-test binary, one suite using `catch_discover_tests`, one with custom listeners or reporters | M1 for Stage 1, M4 for Stage 3 | Stages 1–3 complete; issues fed back before M7 |
| **Early adopters** | Teams with slow suites (highest payoff) | M7 (1.0 GA) | Stage 3 complete |
| **General** | All remaining suites | Early-adopter exit plus 1 month | Stage 3 complete or a documented exemption |
| **Clean-up** | Everyone | General exit | Stage 5 |

Rough throughput: after the pilots, a typical suite takes **1–3 engineer-days**
for Stages 1–2. Stage 3 effort depends entirely on how much shared state the
suite has: from hours to a few weeks for suites with heavy global state. Budget
the long tail explicitly.

## Support model

- **One channel.** A single support channel and a "parallel-migration" issue
  label in the framework repository.
- **Office hours.** Weekly during waves.
- **Shared triage.** A triage rota between the framework team and the
  migration lead. Every report gets classified as *framework bug*, *test bug
  (shared state)*, or *needs new framework feature*.
- **Two dashboards:** suite wall-clock (before/after) and flake rate per suite
  (serial baseline versus parallel).

## Success metrics

| Metric | Target |
|---|---|
| Median CI test-stage wall-clock for migrated suites | ≥ 40% reduction (8-core runners) |
| Flake rate after Stage 3 | Not above the serial baseline |
| Suites on the new framework at `--jobs 1` | 100% by the end of the General wave |
| Suites at `--jobs > 1` | ≥ 80% by the end of the General wave; the rest with documented exemptions |
| Framework bugs open longer than 2 weeks during waves | 0 at severity P1 |
