// ============================================================================
// main.cpp — the test runner.
//
// A hand-rolled runner, for the same reason the harness has no framework:
// the shared code has to compile for the 3DS, and the test build must not
// drag a framework into the tree.
// ============================================================================

#include "test_support.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // The formats library reaches the filesystem through this seam, so the
    // tests install the host's implementation before anything reads a file.
    mdk::formats::setFileSystem(mdk::test::host::fileSystem());

    const std::vector<mdk::test::TestCase>& tests = mdk::test::registry();
    std::printf("running %zu tests\n", tests.size());
    for (size_t i = 0; i < tests.size(); i++) {
        std::printf("- %s\n", tests[i].name);
        tests[i].function();
    }

    const int checks = mdk::test::checkCount();
    const int failures = mdk::test::failureCount();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
