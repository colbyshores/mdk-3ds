#pragma once

// ============================================================================
// test_support.h — the C++ test harness's assertions and host file system.
//
// WHY A HAND-ROLLED HARNESS AND NOT CATCH2: the shared code has to compile
// for the 3DS, and a test framework is a dependency the shipping build must
// not carry. The same source has to build and run on the host, so the
// harness is a header and a couple of lines of each test file.
//
// THE FILE SYSTEM. The formats library reaches the filesystem through the
// FileSystem seam (formats/file_system.h), so the tests install a host
// implementation. That is the same seam the Linux and 3DS backends fill in,
// so the tests exercise the real path, not a bypass.
// ============================================================================

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "formats/file_system.h"

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

namespace mdk {
namespace test {

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline int& checkCount() {
    static int count = 0;
    return count;
}

inline void reportFailure(const char* file, int line, const std::string& message) {
    failureCount()++;
    std::printf("  FAIL %s:%d  %s\n", file, line, message.c_str());
}

inline void reportCheck() {
    checkCount()++;
}

#define MDK_CHECK(condition)                                                    \
    do {                                                                        \
        mdk::test::reportCheck();                                               \
        if (!(condition)) {                                                     \
            mdk::test::reportFailure(__FILE__, __LINE__, "expected: " #condition); \
        }                                                                       \
    } while (0)

#define MDK_CHECK_EQ(actual, expected)                                          \
    do {                                                                        \
        mdk::test::reportCheck();                                               \
        if (!((actual) == (expected))) {                                         \
            char buffer[512];                                                   \
            snprintf(buffer, sizeof(buffer), "%s == %s  (got %lld, want %lld)",  \
                     #actual, #expected, (long long)(actual), (long long)(expected)); \
            mdk::test::reportFailure(__FILE__, __LINE__, buffer);               \
        }                                                                       \
    } while (0)

#define MDK_CHECK_STR_EQ(actual, expected)                                      \
    do {                                                                        \
        mdk::test::reportCheck();                                               \
        const std::string mdk_a = (actual);                                     \
        const std::string mdk_b = (expected);                                   \
        if (mdk_a != mdk_b) {                                                   \
            char buffer[1024];                                                  \
            snprintf(buffer, sizeof(buffer), "%s == %s  (got \"%s\", want \"%s\")", \
                     #actual, #expected, mdk_a.c_str(), mdk_b.c_str());          \
            mdk::test::reportFailure(__FILE__, __LINE__, buffer);               \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// The host file system
// ---------------------------------------------------------------------------

namespace host {

// List a directory's entries. The names are the entries' own names.
// opendir, like the production POSIX seam: listing a FILE fails, which is
// what the mod scanner's directory probe relies on (an `ls` of a file
// succeeds and would call every file a directory).
inline bool listDirectory(void* user, const char* path, std::vector<std::string>* outNames) {
    (void)user;
    outNames->clear();
    DIR* dir = opendir(path);
    if (!dir) {
        return false;
    }
    while (struct dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        outNames->push_back(name);
    }
    closedir(dir);
    return true;
}

inline bool readFile(void* user, const char* path, std::vector<uint8_t>* outBytes) {
    (void)user;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return false;
    }
    outBytes->resize((size_t)size);
    const size_t read = (size > 0) ? fread(outBytes->data(), 1, (size_t)size, file) : 0;
    fclose(file);
    outBytes->resize(read);
    return true;
}

// Read `length` bytes at `offset` of a file: exactly that many, or false (a
// short read is a failure, AGENTS.md P5). The arena stream reads one arena's
// block this way, so a level's MTO is never resident whole.
inline bool readRange(void* user, const char* path, int32_t offset, int32_t length,
                        std::vector<uint8_t>* outBytes) {
    (void)user;
    if (offset < 0 || length < 0) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    outBytes->resize((size_t)length);
    const size_t read = length > 0 ? fread(outBytes->data(), 1, (size_t)length, file) : 0;
    fclose(file);
    if (read != (size_t)length) {
        outBytes->clear();
        return false;
    }
    return true;
}

inline bool fileExists(void* user, const char* path) {
    (void)user;
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

inline const char* programDirectory(void* user) {
    (void)user;
    return ".";
}

inline const char* getEnvironment(void* user, const char* name) {
    (void)user;
    return getenv(name);
}

// The harness's file system, ready to install.
inline const mdk::formats::FileSystem* fileSystem() {
    static mdk::formats::FileSystem instance;
    instance.listDirectory = listDirectory;
    instance.readFile = readFile;
    instance.readRange = readRange;
    instance.fileExists = fileExists;
    instance.programDirectory = programDirectory;
    instance.getEnvironment = getEnvironment;
    instance.user = nullptr;
    return &instance;
}

} // namespace host
} // namespace test
} // namespace mdk

// ---------------------------------------------------------------------------
// The test registry
//
// Each test file declares its cases with MDK_TEST, and main.cpp runs them.
// A hand-rolled registry beats a framework here for the same reason the
// harness has no framework at all.
// ---------------------------------------------------------------------------

namespace mdk {
namespace test {

typedef void (*TestFunction)();

struct TestCase {
    const char* name;
    TestFunction function;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct TestRegistrar {
    TestRegistrar(const char* name, TestFunction function) {
        TestCase testCase;
        testCase.name = name;
        testCase.function = function;
        registry().push_back(testCase);
    }
};

} // namespace test
} // namespace mdk

#define MDK_TEST(name)                                                          \
    static void name();                                                         \
    static mdk::test::TestRegistrar mdk_registrar_##name(#name, name);          \
    static void name()
