# Evidence: ThreadSanitizer run against the current tree

**Purpose.** This run confirms empirically that the shared state on Catch2's
assertion path (inventory items EX-4, EX-6 and ST-2) is unsafe under
concurrency. It is not a static-analysis guess.

## Setup

- **Sources.** `extras/catch_amalgamated.{hpp,cpp}` from this tree (Catch2
  v3.6.0, devel commit `33e24b1`), unmodified.
- **Test.** [`tsan_repro.cpp`](tsan_repro.cpp): one `TEST_CASE` that spawns
  four `std::thread`s. Each thread runs 200 iterations of
  `INFO(...)` + `CHECK(...)` + `CHECK(std::to_string(...) ...)`.
- **What it stands in for.** Assertions from spawned threads go through exactly
  the objects that concurrently running test cases would share:
  - the single `RunContext`;
  - the message vector;
  - the `ReusableStringStream` pool.

  So it serves as a proxy for parallel test-case execution, which cannot be
  exercised without the new scheduler.
- **Toolchain.** GCC 13.3, Linux x86-64. The environment's Clang 18 lacked the
  TSan runtime.

```sh
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -c catch_amalgamated.cpp
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread catch_amalgamated.o tsan_repro.cpp -o tsan_repro
TSAN_OPTIONS="halt_on_error=0" setarch -R ./tsan_repro "assertions from threads"
```

`setarch -R` disables ASLR, which some kernels need for TSan's memory layout.

## Result

**Two runs, 130 and 106 distinct TSan reports.** TSan deduplicates identical
race pairs, so the real number of racing accesses is higher, and the count
varies with scheduling. **Both runs** showed:

- data races;
- a **heap-use-after-free** in `std::vector<MessageInfo>::push_back`, called
  from `RunContext::pushScopedMessage`;
- then a **SEGV**.

After the SEGV, the first run hung and had to be killed. The second run exited
with TSan's error code (66).

Top racing functions in the first run, by number of reports, mapped to
inventory items:

| Reports | Function | Inventory |
|---|---|---|
| 29 | `RunContext::resetAssertionInfo` (`m_lastAssertionInfo`) | EX-4 |
| 27 | `MessageInfo::MessageInfo` (copying into the shared vector; `globalCount`) | EX-6 |
| 16 | `RunContext::pushScopedMessage` (vector reallocation) | EX-6 |
| 14 | `StringStreams::add` (pool vector reallocation) | ST-2 |
| 11 | `StringStreams::release` | ST-2 |
| 7 | `RunContext::assertionPassed` (`m_totals`, `m_lastAssertionPassed`) | EX-4 |
| 7 | `ReusableStringStream::~ReusableStringStream` | ST-2 |
| 6 | `ReusableStringStream::str` | ST-2 |
| 5 | `RunContext::popScopedMessage` (erase by sequence) | EX-6 |
| 1 | `RunContext::assertionEnded` (reporter call with shared totals) | EX-4, RP-1 |
| 1 | **heap-use-after-free** in `MessageInfo` copy, via `pushScopedMessage` | EX-6 |

## Reproducing on the rebased tree

After P0.2 (fast-forward to upstream), run the same repro twice:

1. **Default build.** It should still race, because upstream thread safety is
   opt-in.
2. **With `-DCATCH_CONFIG_THREAD_SAFE_ASSERTIONS`.** It should be clean. This
   confirms upstream's API-level fixes.

Neither run exercises *concurrent test cases*. That requires the Phase 4
scheduler. P2.7 adds a dedicated harness for it.
