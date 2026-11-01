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

// How many times makeArchive() has refused a fixture. Kept apart from `failures`
// on purpose: the refusal is a statement about the TEST, and testOverlongFixtureNameIsRefused
// below asks for one, so counting it as a check failure would make a passing test
// turn the suite red. A refusal outside that test still goes red, because the
// caller gets no bytes and its own checks fail — and the NOTE line says why.
int fixtureRefusals = 0;

// The pinning test asks for a refusal, so it does not need one shouted: a NOTE
// printed on every green run teaches the reader to ignore NOTE lines, which is
// the reflex this file exists to avoid. Real callers leave this true.
bool reportFixtureRefusals = true;

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

// The BNI name field, in bytes: Bni.cs:14 passes 12 to ArchiveIndex, and
// bni.cpp:19 is the port's copy of that constant. The fixture below is exactly
// 8 + kNameFieldBytes + 4 = 24 bytes.
const size_t kNameFieldBytes = 12;

// A well-formed one-entry archive, so the guard below can be shown to accept
// something. The entry's stored payload offset is the whole file, so its
// computed size is zero: the entry EXISTS, which is all these cases need.
//
// IT REFUSES A NAME THAT DOES NOT FIT (review-11 F2). The name is copied before
// the payload offset is written, so a 13-character name does not corrupt the
// offset — the offset write lands on top of the name's last byte and the fixture
// silently becomes a TWELVE-character one. The assertions then fail as if the
// READER had truncated the name, which is a red gate blaming the wrong file. The
// helper is test code, so an overlong name is a broken fixture rather than a
// parse result: it is reported by name, counted in fixtureRefusals, and no bytes
// are returned. assert() is not available here (AGENTS.md §10) and would be the
// wrong tool anyway, because a fixture bug must be reported the way every other
// check in this file is. A name of exactly kNameFieldBytes is legal and is the
// point of testFullNameFillsTheField: the field has no NUL in that case, which is
// what makes the width load-bearing.
std::vector<uint8_t> makeArchive(const char* name) {
    const size_t length = strlen(name);
    if (length > kNameFieldBytes) {
        if (reportFixtureRefusals) {
            std::printf("  NOTE archive_guard: makeArchive(\"%s\") was given a %u-byte name, but the BNI "
                        "name field is %u bytes and the payload offset is written over the byte after it; "
                        "this fixture would pin a layout its assertions do not describe. Fix the fixture, "
                        "not the reader.\n",
                        name, (unsigned)length, (unsigned)kNameFieldBytes);
        }
        fixtureRefusals++;
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> bytes(24, 0);
    putU32(&bytes, 0, 24);
    putU32(&bytes, 4, 1);
    memcpy(bytes.data() + 8, name, length);
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
//
// IT PINS THE LAYOUT, NOT JUST THE LOOKUP. Asserting only `has()` left two real
// reader regressions invisible, because this fixture is built so a WRONG layout
// still finds the entry: the name is 6 bytes (so an 8-byte name field reads it
// too) and the stored payload offset puts the entry exactly at EOF (so its size
// is zero either way). The two assertions on the entry's offset and size are
// what make the case depend on the record layout:
//
//   * dropping the oracle's `4 +` (Fti.cs:66, fti.cpp:72) reports offset 20;
//   * reading the name field as 8 instead of 12 moves the offset field four
//     bytes early, so it reports offset 4 and size 20.
//
// Both reversions leave `has()` green and both fail the offset and size lines.
//
// WHAT THIS CASE DOES NOT PIN (review-10 F4): its name assertion cannot detect a
// width error, because a 6-byte name is also what an 8-byte field reads. The
// width pin is testFullNameFillsTheField below, not the line here. Measured, not
// reasoned — see the falsification table in docs/PROGRESS_LOG.md.
void testWellFormedArchiveParses() {
    std::vector<uint8_t> bytes = makeArchive("H150_R");
    mdk::formats::Bni bni;
    check(bni.parse(&bni, bytes.data(), (int32_t)bytes.size()),
          "a well-formed one-entry archive parses");
    check(bni.has("H150_R"), "the entry is found by name");
    check(!bni.has("H150_I"), "a name that is not there is not found");
    check(bni.entries().size() == 1, "exactly one entry was read");
    if (bni.entries().size() == 1) {
        check(bni.entries()[0].first == "H150_R",
              "the name stops at its NUL inside the field (NOT the width pin; see below)");
        check(bni.entries()[0].second.offset == 24,
              "the entry's offset is the stored offset plus four (the oracle's 4 +)");
        check(bni.entries()[0].second.size == 0,
              "the entry's size is the file end minus its offset");
    }
}

// THE NAME-FIELD WIDTH PIN (review-10 F4). The case above cannot pin it: a
// 6-byte name inside a 12-byte field is also what an 8-byte field reads, so
// reading the field as 8 left its name assertion green and left only the offset
// and size lines on duty. A name with no NUL anywhere in the field is the only
// fixture where the width is load-bearing: with nameLength = 8 the reader stops
// eight bytes early, so the name comes back truncated AND the offset field is
// read from the middle of the name. Both directions fail here.
//
// A full-width name is oracle behaviour, not a convenience: the C#'s
// BinReader.Name(length) hands the whole field to Bin.Ascii (BinReader.cs:52-56
// and :75-80), which takes the entire span when it finds no 0 byte. So "the name
// is the field" is what the oracle does, and this fixture is the case that says
// so. The string itself is a plausible sprite name and carries no meaning: it is
// the width that is pinned, not the spelling.
void testFullNameFillsTheField() {
    std::vector<uint8_t> bytes = makeArchive("H150_R_FRAME");  // exactly 12 chars
    mdk::formats::Bni bni;
    check(bni.parse(&bni, bytes.data(), (int32_t)bytes.size()),
          "a one-entry archive whose name fills the 12-byte field parses");
    check(bni.has("H150_R_FRAME"), "the whole 12-byte field is the name");
    check(!bni.has("H150_R_F"), "the name is not truncated to eight bytes");
    check(bni.entries().size() == 1, "exactly one entry was read");
    if (bni.entries().size() == 1) {
        check(bni.entries()[0].first == "H150_R_FRAME",
              "the name is the whole 12-byte field, with no NUL inside it");
        check(bni.entries()[0].second.offset == 24,
              "the offset field follows the 12-byte name (an 8-byte field reads it mid-name)");
        check(bni.entries()[0].second.size == 0,
              "the entry's size is the file end minus its offset");
    }
}

// THE FIXTURE HELPER'S OWN BOUND, pinned (review-11 F2). A guard nobody can make
// red is not a guard, and this one protects the two cases above, so it is checked
// the same way the reader's guards are: by an input that must be refused. The
// fixture is one character over the field, which is the smallest case of the
// hazard, and both halves are asserted — that no bytes come back, and that the
// refusal was recorded. Without the second half, deleting the bound would leave
// this case green while a future overlong fixture silently became a shorter name.
void testOverlongFixtureNameIsRefused() {
    const int before = fixtureRefusals;
    reportFixtureRefusals = false;
    std::vector<uint8_t> bytes = makeArchive("H150_R_FRAMES");  // 13 chars, one over the field
    reportFixtureRefusals = true;
    check(bytes.empty(), "an overlong fixture name yields no fixture");
    check(fixtureRefusals == before + 1, "and the refusal is recorded against the fixture");
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
    testFullNameFillsTheField();
    testOverlongFixtureNameIsRefused();
    testImpossibleEntryCountIsRejected();
    return failures;
}

}  // namespace test
}  // namespace mdk
