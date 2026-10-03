// Repro for planning/parallel-execution/evidence/README.md
//
// Build against the unmodified amalgamated sources (extras/) with
// ThreadSanitizer; see README.md in this directory for the exact commands.

#include "catch_amalgamated.hpp"
#include <thread>
#include <vector>
#include <string>

// Assertions, messages and stringification from user-spawned threads
// inside a single test case. This exercises the same shared objects that
// concurrently running test cases would touch on every assertion.
TEST_CASE("assertions from threads") {
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([t] {
            for (int i = 0; i < 200; ++i) {
                INFO("thread " << t << " iter " << i);
                CHECK(i + t >= 0);
                CHECK(std::to_string(i) != "x");
            }
        });
    }
    for (auto& th : ts) th.join();
}
