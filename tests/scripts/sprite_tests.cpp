// ============================================================================
// sprite_tests.cpp — see sprite_tests.h.
//
// WHAT IS PINNED HERE.
//
//   1. THE ACCOUNTING. A resident frame costs what the backend says it
//      costs, the total is the sum, and a released frame gives its bytes
//      back.
//   2. THE BOUND. With no budget nothing is ever over budget; with a
//      budget the total over it is over budget, and the total under it is
//      not.
//   3. THE VICTIM. The frame that has gone unused the longest is the one
//      that goes -- and the frame being drawn is never the victim, because
//      dropping it would drop the picture that is about to be drawn.
//   4. THE ANIMATION. Replacing an animation releases every frame of it, so
//      its GPU copies do not leak -- in any order the frames were added.
//
// The numbers are the port's own: the C# has no bound, and the 3DS's is
// named by its backend (renderer_c3d.cpp). The picture the policy produces
// is pinned by the frame golden, not here.
// ============================================================================

#include "sprite_tests.h"

#include <stdio.h>

#include "game/kurt_sprite.h"

using namespace mdk;

namespace mdk {
namespace {

int failures = 0;

#define SPRITE_CHECK(cond)                                                       \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                           \
        }                                                                         \
    } while (0)

void testTheAccounting() {
    SpriteResidency residency;
    SPRITE_CHECK(!residency.bounded());
    SPRITE_CHECK(residency.budget() == 0);
    SPRITE_CHECK(residency.residentBytes() == 0);
    SPRITE_CHECK(residency.residentCount() == 0);

    // With no budget nothing is ever over budget: the C#'s own behaviour.
    SPRITE_CHECK(!residency.overBudget());

    SpriteResidency::Entry entry;
    entry.animation = 0;
    entry.frame = 3;
    entry.bytes = 131072;   // a 95x168 frame as the 3DS's padded RGBA8
    entry.lastUsed = 1;
    residency.add(entry);
    SPRITE_CHECK(residency.residentBytes() == 131072);
    SPRITE_CHECK(residency.residentCount() == 1);
    SPRITE_CHECK(!residency.overBudget());

    // The same frame twice is two frames' worth: the caller adds a frame
    // once, and the accounting is the caller's business.
    SpriteResidency::Entry second = entry;
    second.frame = 4;
    residency.add(second);
    SPRITE_CHECK(residency.residentBytes() == 262144);
    SPRITE_CHECK(residency.residentCount() == 2);

    // A release gives the bytes back.
    SPRITE_CHECK(residency.remove(0, 3));
    SPRITE_CHECK(residency.residentBytes() == 131072);
    SPRITE_CHECK(residency.residentCount() == 1);
    // Releasing it twice is not a release.
    SPRITE_CHECK(!residency.remove(0, 3));
    SPRITE_CHECK(residency.residentBytes() == 131072);

    // A negative budget is not a bound. The backend answers 0 for "no
    // bound" and a negative number would make everything over budget.
    residency.setBudget(-1);
    SPRITE_CHECK(residency.budget() == 0);
    SPRITE_CHECK(!residency.bounded());
    SPRITE_CHECK(!residency.overBudget());

    // A new level's load() starts the cache over, even when a frame is left
    // in the accounting: stale entries must not spend a new level's budget.
    residency.add(entry);
    residency.clear();
    SPRITE_CHECK(residency.residentBytes() == 0);
    SPRITE_CHECK(residency.residentCount() == 0);
    SPRITE_CHECK(!residency.remove(0, 3));
}

void testTheBound() {
    SpriteResidency residency;
    residency.setBudget(200000);
    SPRITE_CHECK(residency.bounded());
    SPRITE_CHECK(residency.budget() == 200000);

    SpriteResidency::Entry entry;
    entry.animation = 0;
    entry.frame = 0;
    entry.bytes = 100000;
    entry.lastUsed = 1;
    residency.add(entry);
    SPRITE_CHECK(!residency.overBudget());

    SpriteResidency::Entry second = entry;
    second.frame = 1;
    second.bytes = 150000;
    residency.add(second);
    SPRITE_CHECK(residency.overBudget());
    SPRITE_CHECK(residency.residentBytes() == 250000);

    // Dropping the first frame brings the set back inside the bound.
    SPRITE_CHECK(residency.remove(0, 0));
    SPRITE_CHECK(!residency.overBudget());
    SPRITE_CHECK(residency.residentBytes() == 150000);
}

void testTheVictim() {
    SpriteResidency residency;
    residency.setBudget(1000);

    SpriteResidency::Entry oldest;
    oldest.animation = 1;
    oldest.frame = 0;
    oldest.bytes = 100;
    oldest.lastUsed = 1;
    residency.add(oldest);

    SpriteResidency::Entry middle = oldest;
    middle.animation = 1;
    middle.frame = 1;
    middle.lastUsed = 5;
    residency.add(middle);

    SpriteResidency::Entry newest = oldest;
    newest.animation = 2;
    newest.frame = 7;
    newest.lastUsed = 9;
    residency.add(newest);

    // The frame that has gone unused the longest.
    SpriteResidency::Entry victim;
    SPRITE_CHECK(residency.victim(-1, -1, &victim));
    SPRITE_CHECK(victim.animation == 1 && victim.frame == 0);

    // A draw makes a frame the newest, and it stops being the victim.
    residency.touch(1, 0, 20);
    SPRITE_CHECK(residency.victim(-1, -1, &victim));
    SPRITE_CHECK(victim.animation == 1 && victim.frame == 1);

    // THE FRAME BEING DRAWN IS NEVER THE VICTIM. Dropping it would drop
    // the picture that is about to be drawn.
    SPRITE_CHECK(residency.victim(1, 1, &victim));
    SPRITE_CHECK(!(victim.animation == 1 && victim.frame == 1));
    SPRITE_CHECK(victim.animation == 2 && victim.frame == 7);

    // The only resident frame is the one being drawn: there is no victim,
    // and the cache stays over budget rather than dropping the picture.
    SpriteResidency alone;
    alone.setBudget(1);
    SpriteResidency::Entry only;
    only.animation = 0;
    only.frame = 0;
    only.bytes = 10;
    only.lastUsed = 1;
    alone.add(only);
    SPRITE_CHECK(!alone.victim(0, 0, &victim));
    SPRITE_CHECK(alone.overBudget());
}

void testReplacingAnAnimation() {
    SpriteResidency residency;
    residency.setBudget(1000000);

    SpriteResidency::Entry first;
    first.animation = 0;
    first.frame = 0;
    first.bytes = 100;
    first.lastUsed = 1;
    residency.add(first);

    SpriteResidency::Entry second = first;
    second.frame = 1;
    second.lastUsed = 2;
    residency.add(second);

    SpriteResidency::Entry other = first;
    other.animation = 1;
    other.frame = 0;
    other.lastUsed = 3;
    residency.add(other);

    // THE ANIMATION GOES, AND ONLY THE ANIMATION.
    residency.removeAnimation(0);
    SPRITE_CHECK(residency.residentCount() == 1);
    SPRITE_CHECK(residency.residentBytes() == 100);
    SPRITE_CHECK(residency.remove(1, 0));
    SPRITE_CHECK(residency.residentCount() == 0);
    SPRITE_CHECK(residency.residentBytes() == 0);

    // THE ORDER THE FRAMES WERE ADDED DOES NOT MATTER. Removing an entry
    // moves the last one into its hole, so an order in which the moved entry
    // is itself a match has to be re-examined rather than stepped over.
    SpriteResidency interleaved;
    interleaved.setBudget(1000000);
    SpriteResidency::Entry mixed;
    mixed.bytes = 100;
    mixed.lastUsed = 1;
    mixed.animation = 1;
    mixed.frame = 0;
    interleaved.add(mixed);
    mixed.animation = 0;
    mixed.frame = 0;
    interleaved.add(mixed);
    mixed.animation = 1;
    mixed.frame = 1;
    interleaved.add(mixed);
    mixed.animation = 0;
    mixed.frame = 1;
    interleaved.add(mixed);
    mixed.frame = 2;
    interleaved.add(mixed);
    SPRITE_CHECK(interleaved.residentCount() == 5);
    interleaved.removeAnimation(0);
    SPRITE_CHECK(interleaved.residentCount() == 2);
    SPRITE_CHECK(interleaved.residentBytes() == 200);
    SPRITE_CHECK(interleaved.remove(1, 0));
    SPRITE_CHECK(interleaved.remove(1, 1));
    SPRITE_CHECK(interleaved.residentCount() == 0);
}

void testTheClock() {
    SpriteResidency residency;
    const uint32_t first = residency.tick();
    const uint32_t second = residency.tick();
    SPRITE_CHECK(first == 1);
    SPRITE_CHECK(second == first + 1);
}

} // namespace

namespace test {

int runSpriteResidencyTests() {
    testTheAccounting();
    testTheBound();
    testTheVictim();
    testReplacingAnAnimation();
    testTheClock();
    return failures;
}

} // namespace test
} // namespace mdk
