# 04 — Detaching from upstream Catch2

## 1. Is detaching the right call?

Yes. The evidence supports it.

**Upstream has ruled out the core feature.**
- `docs/limitations.md` (unchanged since 2017, still present in v3.16.0): "*Catch2 keeps test execution in one process strictly serial, and there are no plans to change this.*"
- `docs/faq.md`: running tests in parallel is "*the job of an external test runner*".
- So the features in [02](02-target-architecture.md) are not going to be accepted upstream.

**The changes hit the areas upstream changes most often.**
- The refactor touches `RunContext` (EX-3), the reporter and listener contract
  (RP-1, RP-6), capture (OS-1) and fatal handling (OS-2).
- Upstream has rewritten exactly these areas several times since this tree was
  cut:
  - the 3.7.0 redirect rework;
  - the 3.9–3.12 thread-safety work;
  - the 3.13 tracker and generator changes;
  - the 3.15 `IResultCapture` changes.
- Staying a fork would mean a merge conflict in the core on most upstream
  releases.

**Separate identity is also technically required.**
- A consumer may end up with upstream Catch2 and this framework in the same
  link. For example, a third-party library ships test utilities built on
  Catch2.
- Identical mangled names would then cause silent ODR violations.
- Distinct package names avoid `find_package(Catch2)` ambiguity on shared build
  machines.

**What detaching costs:**
- Upstream fixes (matchers, stringification, CMake helpers, compiler
  workarounds) have to be ported by hand.
- Section 6 sets up a lightweight process to keep that cheap.

## 2. Identity: names, namespaces and the source-compatibility question

The single most consequential decision (D1) is **how much consumer source code
must change**. Two coherent options:

### Option A — Source-compatible detachment (recommended for 1.x)

Keep everything users *write*, and change everything *packaging and linking*
sees.

| Surface | Today | Option A |
|---|---|---|
| Test macros (`TEST_CASE`, `REQUIRE`, `SECTION`, …) and prefixed forms (`CATCH_REQUIRE`, …) | unchanged | **unchanged** |
| C++ namespace in source | `Catch` | **`Catch`**, with an inline ABI namespace: `namespace Catch { inline namespace <proj>_v1 { … } }` |
| Mangled symbols | `Catch::X` | `Catch::<proj>_v1::X`, so no ODR clash with upstream Catch2 in the same link |
| User specialisations (`namespace Catch { template<> struct StringMaker<Foo> … }`), custom reporters and listeners deriving from `Catch::EventListenerBase`, matchers | work | **work unchanged** |
| Include root | `<catch2/…>` | `<proj/…>`. A separate `proj::catch2_compat` target adds forwarding `<catch2/…>` headers, only for consumers that link it. |
| Config macros (`CATCH_CONFIG_*`) | 76 distinct names | Kept as accepted spellings during 1.x through a translation header. Canonical new prefix (D1). |
| CMake package / targets | `Catch2`, `Catch2::Catch2`, `Catch2::Catch2WithMain` | `<Proj>`, `<proj>::<proj>`, `<proj>::<proj>_with_main` (plus `<proj>::catch2_compat`) |
| pkg-config, Conan, Bazel module, Meson project | `catch2`, `catch2-with-main` | renamed |
| Binary identity (`--libidentify`, console banner) | `framework: Catch2` | `framework: <Proj>` and a `based-on: Catch2 3.x` line |

The inline-namespace technique is the one libc++ uses for `std::__1`. A
specialisation written in the enclosing namespace is valid C++ for a member of
an inline namespace ([namespace.def]). We checked that unmodified upstream-style
user code (a `StringMaker` specialisation in `namespace Catch {}` and a class
derived from `Catch::EventListenerBase`) compiles under GCC 13 and Clang 18 in
C++14 mode, and that the emitted symbols carry the distinct
`Catch::<proj>_v1::` mangling.

### Option B — Full rebrand

New namespace, new macro prefix, new config prefix. A compatibility shim maps
the old spellings to the new ones. The catch is that a namespace *alias*
(`namespace Catch = proj;`) cannot be reopened. Every consumer that specialises
`Catch::StringMaker`, or writes `namespace Catch { … }` for any reason, has to
edit their source. Option B therefore turns the migration into a source
migration for many teams.

**Recommendation:** ship 1.x with Option A. If a full rebrand is wanted later,
do it as a 2.0 with an automated rewriter (clang-tidy check or script). By then
the compat layer will have shown which spellings are actually in use.

### Output-format identifiers (D11)

Downstream tooling parses these, so renaming them is a compatibility decision,
not just branding:

- the XML reporter root element `Catch2TestRun` and attribute `catch2-version`
  (`src/catch2/reporters/catch_reporter_xml.cpp:56,60`);
- the JSON reporter key `catch2-version` (`catch_reporter_json.cpp:65`);
- the `--libidentify` fields (`catch_session.cpp:192-198`).

**Recommendation:**
- Keep the XML and JSON element and key names in 1.x.
- Add a `framework` attribute/key carrying the new name.
- Add a reporter option such as `-r xml::format=<proj>` for consumers who want
  the new names.
- JUnit, SonarQube, TAP and TeamCity formats are external standards and are
  unaffected.

## 3. Renaming surfaces (sized on this tree)

| Surface | Size | Notes |
|---|---|---|
| `#include <catch2/…>` | 349 files | Mechanical. Scripted rename plus forwarding headers. |
| `namespace Catch` | 256 source files | Option A: add the inline namespace in each file (scripted) or via a macro pair `CATCH_NS_BEGIN`/`END`. |
| `CATCH_` macro tokens | ~1,900 occurrences in `src/`, 76 distinct `CATCH_CONFIG_*` names | Option A keeps them. Option B renames and adds a translation header. |
| Header guards `CATCH_*_HPP_INCLUDED` | every header | Checked by `.github/workflows/validate-header-guards.yml`. Rename to avoid clashes if both libraries' headers are on one include path. |
| Build systems | `CMakeLists.txt`, `src/CMakeLists.txt` (targets, VS GUIDs at `:7-8`, export set), `CMake/Catch2Config.cmake.in`, `CMake/catch2*.pc.in`, `meson.build`, `MODULE.bazel`, `WORKSPACE.bazel`, `BUILD.bazel`, `conanfile.py`, `.conan/` | `conanfile.py` and `tools/scripts/releaseCommon.py` find the version through the `# CML version placeholder` comment. Keep that convention. |
| CMake helpers | `extras/Catch.cmake`, `CatchAddTests.cmake`, `CatchShardTests*.cmake`, `ParseAndAddCatchTests.cmake` | Rename the functions (`catch_discover_tests` → `<proj>_discover_tests`) and keep the old names as wrappers during 1.x. |
| Tooling | `tools/scripts/*` (amalgamation, release, approvals, license and guard checks); `tools/misc/coverage-helper.cpp` (finds the repo root by searching the path for "catch") | `checkLicense.py` must accept the new dual notice (§5). |
| Runtime strings | `catch_session.cpp:188,194,196`; `catch_reporter_console.cpp:552`; XML/JSON as above | Tests assert on them: `tests/CMakeLists.txt:379,382`; `tests/TestScripts/testConfigureDefaultReporter.py:46`. |
| Docs | 31 of 33 files in `docs/` mention Catch2; many link to `github.com/catchorg` | Rewrite in Phase 7. Update links in Phase 1. |
| Repo metadata | `README.md` badges, `.github/FUNDING.yml`, `CODE_OF_CONDUCT.md` contact, `SECURITY.md`, issue and PR templates, `data/artwork/`, `appveyor.yml`, `codecov.yml` | Replace or remove. |

## 4. Repository mechanics

1. **Rebase first.** Fast-forward to the latest upstream release (P0.2). This
   tree is a strict ancestor of upstream `devel`, so it is conflict-free.
2. **New repository, not a GitHub fork.** Push the full history to a new
   repository outside the fork network.
   - Keeping the history preserves `git blame` and makes the provenance
     auditable.
   - Being outside the fork network avoids the "forked from catchorg/Catch2"
     linkage, which drives PR targeting and contributor confusion.
3. **Tags.** Tag the cut point `upstream-fork-point` (and
   `upstream/v3.x.y`) so ports can be diffed against it later.
4. **Branching and releases.**
   - `main` is trunk.
   - Release branches are `release/1.x`.
   - Releases follow SemVer from **1.0.0**. The `based-on` line in
     `--libidentify` records the upstream version.
5. **Ownership.**
   - A CODEOWNERS file with two owners for the concurrency-critical paths:
     `internal/catch_run_context*`, the scheduler, the sequencer, the
     demultiplexer and the fatal handler.
   - Required reviews as stated in [03 Staffing](03-roadmap.md#staffing).

## 5. Licence and notices

Catch2 is under the **Boost Software License 1.0** (`LICENSE.txt`), which
permits modification and redistribution. The licence text and copyright notices
must stay in all source copies. Object-code-only distributions are exempt.

- **Existing files.** Keep the `// Copyright Catch2 Authors` / BSL-1.0 preamble.
  `tools/scripts/checkLicense.py` requires its exact text.
- **Modified and new files.** Add the organisation's own copyright line *in
  addition to* the existing one, and keep `SPDX-License-Identifier: BSL-1.0`
  unless legal decides otherwise. Mixing licences inside one file is not
  recommended.
- **Provenance.** Add a `NOTICE` file stating that the project is derived from
  Catch2 (with the URL and fork-point commit), and an `UPSTREAM.md` ledger of
  upstream changes ported after the cut.
- **Trademarks.** Do not use the Catch2 name or artwork as the project's
  identity. "Based on Catch2" as a factual statement is fine.
- **This is not legal advice.** Have the organisation's OSS or legal office
  confirm it in Phase 1 (P1.3).

## 6. Relationship with upstream after the cut

| Activity | Cadence | Owner |
|---|---|---|
| Read upstream release notes; triage each item into *port*, *ignore* or *superseded* | Each upstream release (~every 1–2 months historically) | Tech lead |
| Port security and crash fixes | Within one release cycle | Any engineer |
| Port compiler and platform workarounds, matcher/generator/stringification fixes | Batched quarterly | Any engineer |
| Features touching `RunContext`, reporters, capture or fatal handling | Case by case. Expect a re-implementation, not a cherry-pick | Tech lead decides |
| Record every port in `UPSTREAM.md` (upstream SHA → local commit) | Always | Porter |

**Expected portability:**
- Matchers, generators, `StringMaker`s, Clara, TextFlow, the JSON/XML writers
  and the CMake helpers stay largely portable.
- `RunContext`, the reporter bases and the session diverge quickly after
  Phase 2.

**Upstreaming in the other direction.** Self-contained fixes found here, such
as RP-8, ST-6 and ST-1, are worth offering upstream as courtesy PRs. They do not
depend on the parallel design.

## 7. Detachment checklist (Phase 1 exit)

- [ ] D1 (name, namespace strategy, macro prefix) and D11 (output identifiers) decided and recorded.
- [ ] Rename script committed and re-runnable; CI green on the full matrix.
- [ ] `<proj>::catch2_compat` target and translation header; consumer smoke project builds unmodified.
- [ ] Inline ABI namespace in place; a link test with upstream Catch2 in the same binary shows no duplicate-symbol or ODR diagnostics.
- [ ] `--libidentify`, banner, XML/JSON `framework` field updated; tests updated.
- [ ] `NOTICE`, `UPSTREAM.md`, licence checker updated; legal sign-off.
- [ ] README, SECURITY, CoC contact, templates, funding and badges replaced.
- [ ] Package published to internal registries (CMake config, pkg-config, Conan/vcpkg/Bazel as applicable).
- [ ] `upstream-fork-point` tag pushed; upstream-watch process staffed.
