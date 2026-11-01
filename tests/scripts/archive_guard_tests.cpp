// archive_guard_tests.cpp — see archive_guard_tests.h.
//
// WHY THIS FILE EXISTS. When TRAVERSE/TRAVSPRT.BNI cannot be read the Linux
// frontend runs the level WITHOUT scripts and says so, and the golden harness
// refuses to run at all (exit 1) rather than hand the runtime a null archive.
// Both of those decisions rest on one mechanical fact: a short archive fails
// Bni::parse. Nothing pinned that fact, so the guard it protects could be
// broken without anything going red — the classic "a defence against an
// unreachable condition passes as proof of itself" shape.
//
// These cases build their own bytes, so they run with no game data present.
// That matters: the refusal itself can only be observed against a real data
// tree, so a test that needed the tree would SKIP on a machine without it, and
// a check that skips on missing input is a pass generator, not a gate.
//
// The archive layout the reader actually enforces (formats/fti.cpp,
// ArchiveIndex::read, with nameLength = 12 for a BNI):
//
//   offset 0   u32   the archive's own size (the reader does not use it)
//   offset 4   u32   entry count
//   offset 8   count * (12-byte name + u32 payload offset)
//
// WHICH GUARD FIRES, MEASURED, NOT ASSUMED. Three separate rejections stand
// between a broken file and a successful parse, and they OVERLAP:
//
//   * the pointer overload's `!bytes` — an empty vector's data() is null, so
//     this is what rejects a 0-byte file;
//   * `bytes.size() < 8`;
//   * `count * (nameLength + 4) > size - 8`, the count-fit guard.
//
// For a 1-, 4- or 7-byte file the second and third BOTH reject it, each on its
// own: deleting either one leaves these cases green, and only deleting the two
// of them together makes them fail. That was measured here rather than assumed,
// because the first reversion attempt deleted `size < 8`, printed PASS, and
// looked exactly like a dead test. So these cases pin the BEHAVIOUR the refusal
// depends on ("a short file is not an archive"), not any one guard, and a
// one-guard reversion is NOT a valid falsification of them — see the batch
// narrative in docs/PROGRESS_LOG.md before concluding a case here is inert.

#include "archive_guard_tests.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "formats/bni.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        failures++;
        std::printf("  FAIL archive_guard: %s\n", what);
    }
}

void putU32(std::vector<uint8_t>* bytes, size_t at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        (*bytes)[at + (size_t)i] = (uint8_t)((value >> (8 * i)) & 0xFFu);
    }
}

// A well-formed one-entry archive, so the guard below can be shown to accept
// something. The entry's stored payload offset is the whole file, so its
// computed size is zero: the entry EXISTS, which is all these cases need.
std::vector<uint8_t> makeArchive(const char* name) {
    std::vector<uint8_t> bytes(24, 0);
    putU32(&bytes, 0, 24);
    putU32(&bytes, 4, 1);
    memcpy(bytes.data() + 8, name, strlen(name));
    putU32(&bytes, 20, 20);
    return bytes;
}

// The behaviour the missing-archive path rests on: a file too short to be an
// archive does not parse. 4 is the exact size used by the committed evidence
// capture (docs/evidence/2026-11-01-L7-sprites-missing-scene.png), where the
// stub makes the data marker exist, the read succeed, and the parse fail — the
// only way to reach the refusal without deleting the file, which would take the
// whole data tree down with it. See the file header for which guard rejects
// which of these sizes.
void testTruncatedArchiveIsNotAnArchive() {
    const int kBeforeGuard[] = {0, 1, 4, 7};
    for (size_t i = 0; i < sizeof(kBeforeGuard) / sizeof(kBeforeGuard[0]); i++) {
        const int size = kBeforeGuard[i];
        std::vector<uint8_t> bytes((size_t)size, 0);
        mdk::formats::Bni bni;
        char label[128];
        snprintf(label, sizeof(label),
                 "a %d-byte archive must not parse (the 4-byte stub is the evidence case)", size);
        check(!bni.parse(&bni, bytes.data(), (int32_t)size), label);
    }
}

// The other side of the same guard, pinned so nobody "tightens" it into a
// behaviour change: the guard is about TRUNCATION, not about CONTENT. A
// zero-filled archive is long enough, its entry count is 0, and it therefore
// parses as a legal EMPTY archive. That is the oracle's behaviour too and it
// is deliberate — the C# can produce an archive with no such entry, and
// GetAnimation returning null for a missing entry is faithful, so a content
// check here would add a branch the oracle does not have.
void testZeroFilledArchiveParsesButIsEmpty() {
    std::vector<uint8_t> bytes(4096, 0);
    mdk::formats::Bni bni;
    check(bni.parse(&bni, bytes.data(), (int32_t)bytes.size()),
          "a zero-filled archive parses as an empty archive");
    check(!bni.has("H150_R"), "the empty archive has no H150_R");
    check(!bni.has("H150_I"), "the empty archive has no H150_I");
}

// Non-vacuity: the same call that rejects the stub accepts a real one-entry
// archive. Without this, "the stub is rejected" would also be satisfied by a
// parse that rejects everything.
void testWellFormedArchiveParses() {
    std::vector<uint8_t> bytes = makeArchive("H150_R");
    mdk::formats::Bni bni;
    check(bni.parse(&bni, bytes.data(), (int32_t)bytes.size()),
          "a well-formed one-entry archive parses");
    check(bni.has("H150_R"), "the entry is found by name");
    check(!bni.has("H150_I"), "a name that is not there is not found");
}

// An entry count that cannot fit in the file is a broken file, not an archive.
// This is the second rejection, and it is the one a corrupt-but-long file hits.
void testImpossibleEntryCountIsRejected() {
    std::vector<uint8_t> bytes(24, 0);
    putU32(&bytes, 0, 24);
    putU32(&bytes, 4, 0xFFFF);  // 65535 entries of 16 bytes cannot fit in 16 bytes
    mdk::formats::Bni bni;
    check(!bni.parse(&bni, bytes.data(), (int32_t)bytes.size()),
          "an entry count that does not fit is rejected");
}

}  // namespace

namespace mdk {
namespace test {

int runArchiveGuardTests() {
    testTruncatedArchiveIsNotAnArchive();
    testZeroFilledArchiveParsesButIsEmpty();
    testWellFormedArchiveParses();
    testImpossibleEntryCountIsRejected();
    return failures;
}

}  // namespace test
}  // namespace mdk
