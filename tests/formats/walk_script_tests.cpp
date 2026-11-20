// ============================================================================
// walk_script_tests.cpp -- the hardware harness's scripted walk
// (game/walk_script.h): what a script parses to, which keys it holds on which
// frame, and every refusal.
//
// WHAT IS PINNED HERE.
//   1. THE GRAMMAR. `keys@first-end` segments separated by ';', keys joined by
//      '+', the end EXCLUSIVE (so abutting segments do not overlap), spaces
//      tolerated, one trailing ';' tolerated.
//   2. THE FRAME ANSWER. walkKeysAt ORs overlapping segments and holds nothing
//      outside every range; applyWalk OWNS the input (clears what the poll set,
//      and the look); walkSegmentStartingAt names a segment's first frame.
//   3. THE REFUSALS. Empty text, an empty segment, an unknown action, Escape, a
//      missing '@' or '-', a non-decimal or out-of-range frame, an end not after
//      the first, and more than kMaxSegments -- each leaves the output empty and
//      names the segment, so a WALK build cannot boot on half a script.
// ============================================================================

#include "test_support.h"

#include <string.h>

#include <string>

#include "game/walk_script.h"

using mdk::WalkScript;

namespace {

uint32_t bit(mdk::Key key) {
    return 1u << (uint32_t)key;
}

bool refuses(const char* text, const char* errorMentions) {
    WalkScript script;
    script.count = 7;   // must be emptied by a refusal
    char error[256];
    const bool ok = mdk::parseWalkScript(text, &script, error, sizeof(error));
    if (ok || script.count != 0 || error[0] == '\0') {
        return false;
    }
    return errorMentions == nullptr || strstr(error, errorMentions) != nullptr;
}

} // namespace

MDK_TEST(walk_script_parses_the_documented_example) {
    WalkScript script;
    char error[256];
    MDK_CHECK(mdk::parseWalkScript("forward@60-600;turnleft@600-640;forward@640-900", &script,
                                   error, sizeof(error)));
    MDK_CHECK_EQ(script.count, 3);
    MDK_CHECK_EQ(script.segments[0].first, 60u);
    MDK_CHECK_EQ(script.segments[0].end, 600u);
    MDK_CHECK_EQ(script.segments[0].keys, bit(mdk::kKeyForward));
    MDK_CHECK_EQ(script.segments[1].keys, bit(mdk::kKeyTurnLeft));
    MDK_CHECK_EQ(script.segments[2].first, 640u);
    MDK_CHECK_EQ(script.segments[2].end, 900u);
    MDK_CHECK_STR_EQ(std::string(error), std::string());
}

MDK_TEST(walk_script_end_is_exclusive_and_segments_abut) {
    WalkScript script;
    MDK_CHECK(mdk::parseWalkScript("forward@60-600;turnleft@600-640", &script, nullptr, 0));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 59), 0u);
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 60), bit(mdk::kKeyForward));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 599), bit(mdk::kKeyForward));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 600), bit(mdk::kKeyTurnLeft));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 639), bit(mdk::kKeyTurnLeft));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 640), 0u);
    MDK_CHECK_EQ(mdk::walkSegmentStartingAt(script, 60), 0);
    MDK_CHECK_EQ(mdk::walkSegmentStartingAt(script, 600), 1);
    MDK_CHECK_EQ(mdk::walkSegmentStartingAt(script, 61), -1);
}

MDK_TEST(walk_script_plus_joins_keys_and_overlaps_or) {
    WalkScript script;
    MDK_CHECK(mdk::parseWalkScript(" forward + turbo @ 10 - 20 ; strafeleft@15-30;", &script,
                                   nullptr, 0));
    MDK_CHECK_EQ(script.count, 2);
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 12), bit(mdk::kKeyForward) | bit(mdk::kKeyTurbo));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 15),
                 bit(mdk::kKeyForward) | bit(mdk::kKeyTurbo) | bit(mdk::kKeyStrafeLeft));
    MDK_CHECK_EQ(mdk::walkKeysAt(script, 25), bit(mdk::kKeyStrafeLeft));
    char keys[64];
    mdk::formatWalkKeys(script.segments[0].keys, keys, sizeof(keys));
    MDK_CHECK_STR_EQ(std::string(keys), std::string("forward+turbo"));
}

MDK_TEST(walk_script_every_walkable_name_round_trips) {
    for (int32_t k = 0; k < mdk::kKeyCount; k++) {
        if (k == mdk::kKeyEscape) {
            continue;
        }
        const std::string text = std::string(mdk::walkKeyName((mdk::Key)k)) + "@0-1";
        WalkScript script;
        MDK_CHECK(mdk::parseWalkScript(text.c_str(), &script, nullptr, 0));
        MDK_CHECK_EQ(script.count, 1);
        MDK_CHECK_EQ(script.segments[0].keys, 1u << (uint32_t)k);
    }
    MDK_CHECK_STR_EQ(std::string(mdk::walkKeyName(mdk::kKeyEscape)), std::string("?"));
}

MDK_TEST(walk_script_apply_owns_the_input) {
    WalkScript script;
    MDK_CHECK(mdk::parseWalkScript("forward@0-10", &script, nullptr, 0));
    mdk::Input input;
    input.down[mdk::kKeyFire] = true;   // what a poll might have set
    input.mouseX = 12.0f;
    input.mouseY = -3.0f;
    mdk::applyWalk(script, 5, &input);
    MDK_CHECK(input.isDown(mdk::kKeyForward));
    MDK_CHECK(!input.isDown(mdk::kKeyFire));
    MDK_CHECK_EQ(input.mouseX, 0.0f);
    MDK_CHECK_EQ(input.mouseY, 0.0f);
    mdk::applyWalk(script, 10, &input);
    MDK_CHECK(!input.isDown(mdk::kKeyForward));
}

MDK_TEST(walk_script_refusals_leave_nothing_and_name_the_segment) {
    MDK_CHECK(refuses(nullptr, "empty"));
    MDK_CHECK(refuses("", "empty"));
    MDK_CHECK(refuses(";", "empty"));
    MDK_CHECK(refuses("forward@0-10;;back@10-20", "segment 2"));
    MDK_CHECK(refuses("forwards@0-10", "unknown action"));
    MDK_CHECK(refuses("escape@0-10", "unknown action"));
    MDK_CHECK(refuses("forward+@0-10", "unknown action"));
    MDK_CHECK(refuses("forward 0-10", "no '@'"));
    MDK_CHECK(refuses("forward@10", "no '-'"));
    MDK_CHECK(refuses("forward@a-10", "decimal"));
    MDK_CHECK(refuses("forward@-10", "decimal"));
    MDK_CHECK(refuses("forward@1-2x", "decimal"));
    MDK_CHECK(refuses("forward@0-99999999999", "decimal"));
    MDK_CHECK(refuses("forward@0-16777217", "decimal"));
    MDK_CHECK(refuses("forward@10-10", "after the first"));
    MDK_CHECK(refuses("forward@20-10", "after the first"));
    MDK_CHECK(refuses("forward@0-1;back@x-2", "segment 2"));
    std::string many;
    for (int32_t i = 0; i < WalkScript::kMaxSegments; i++) {
        many += "jump@" + std::to_string(i) + "-" + std::to_string(i + 1) + ";";
    }
    WalkScript script;
    MDK_CHECK(mdk::parseWalkScript(many.c_str(), &script, nullptr, 0));
    MDK_CHECK_EQ(script.count, WalkScript::kMaxSegments);
    many += "jump@99-100";
    MDK_CHECK(refuses(many.c_str(), "more than 32"));
    // The largest frame is accepted.
    MDK_CHECK(mdk::parseWalkScript("back@0-16777216", &script, nullptr, 0));
}
