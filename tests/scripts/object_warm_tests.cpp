// ============================================================================
// object_warm_tests.cpp -- see object_warm_tests.h.
// ============================================================================

#include "object_warm_tests.h"

#include <stdio.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "formats/arena.h"
#include "formats/cmi.h"
#include "formats/mdk_data.h"
#include "formats/model.h"
#include "game/level_data.h"
#include "game/object_view.h"
#include "render/material_resolver.h"

namespace mdk {
namespace test {

namespace {

int failures = 0;

#define WARM_CHECK(cond)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                      \
            failures++;                                                           \
        }                                                                         \
    } while (0)

typedef std::pair<const formats::Texture*, const formats::Palette*> Key;

// What the warm handed over: the keys, and how many calls carried the wrong
// palette (anything but the look's).
struct Recorded {
    std::set<Key> keys;
    const formats::Palette* palette = nullptr;
    int wrongPalette = 0;
    int calls = 0;
};

void record(void* user, const Surface& surface, const formats::Palette* palette) {
    Recorded* recorded = static_cast<Recorded*>(user);
    recorded->calls++;
    if (palette != recorded->palette) {
        recorded->wrongPalette++;
    }
    if (surface.texture != nullptr) {
        recorded->keys.insert(Key(surface.texture, palette));
    }
}

} // namespace

int runObjectWarmTests(const formats::MdkData& data) {
    failures = 0;
    LevelData level;
    WARM_CHECK(LevelData::load(&level, &data, 7));
    std::vector<uint8_t> bytes;
    formats::Cmi cmi;
    const bool cmiRead = data.read("TRAVERSE/LEVEL7/LEVEL7.CMI", &bytes) &&
                         formats::Cmi::parse(&cmi, std::move(bytes));
    WARM_CHECK(cmiRead);
    if (!cmiRead || level.arenas().empty()) {
        return failures;
    }
    const MaterialResolver resolver(&level.dti(), Shading::Lit);
    int batchesChecked = 0;
    int arenasWithOwnModels = 0;
    // A few arenas: the level's first ones (each look's archives differ).
    for (size_t a = 0; a < level.arenas().size() && a < 4; a++) {
        const formats::Arena* arena = level.arenas()[a];
        ObjectLook look;
        look.palette = &level.paletteOf(arena);
        look.archives = level.archivesOf(arena);

        Recorded recorded;
        recorded.palette = look.palette;
        size_t cursor = 0;
        const size_t total = (size_t)arena->modelCount() + cmi.modelOffsets().size();
        size_t calls = 0;
        bool done = false;
        while (!done && calls < total + 1) {
            done = warmObjectStep(*arena, &cmi, look, resolver, record, &recorded, &cursor);
            calls++;
        }
        WARM_CHECK(done);
        WARM_CHECK(calls == total);           // one model a call
        WARM_CHECK(recorded.wrongPalette == 0);
        const int callsAtEnd = recorded.calls;
        WARM_CHECK(warmObjectStep(*arena, &cmi, look, resolver, record, &recorded, &cursor));
        WARM_CHECK(recorded.calls == callsAtEnd);   // a finished arena hands nothing over
        if (arena->modelCount() > 0) {
            arenasWithOwnModels++;
        }

        // The object pass's own layouts: every textured batch's key was handed
        // over. The arena's models, then the global ones (as the scripts find them).
        ObjectView view(resolver);
        for (int32_t m = 0; m < arena->modelCount(); m++) {
            const formats::Model* model = arena->model(arena->modelName(m));
            const ObjectLayout* layout = model ? view.layoutOf(model, look) : nullptr;
            for (size_t b = 0; layout && b < layout->batches.size(); b++) {
                const Surface& surface = layout->batches[b].surface;
                if (surface.texture != nullptr) {
                    WARM_CHECK(recorded.keys.count(Key(surface.texture, look.palette)) == 1);
                    batchesChecked++;
                }
            }
        }
        std::vector<formats::Model> globals(cmi.modelOffsets().size());
        for (size_t g = 0; g < cmi.modelOffsets().size(); g++) {
            if (!cmi.model(cmi.modelOffsets()[g].first, &globals[g])) {
                continue;
            }
            const ObjectLayout* layout = view.layoutOf(&globals[g], look);
            for (size_t b = 0; layout && b < layout->batches.size(); b++) {
                const Surface& surface = layout->batches[b].surface;
                if (surface.texture != nullptr) {
                    WARM_CHECK(recorded.keys.count(Key(surface.texture, look.palette)) == 1);
                    batchesChecked++;
                }
            }
        }
        view.clear();
    }
    // Not vacuous: real batches were compared, own models included.
    WARM_CHECK(batchesChecked > 0);
    WARM_CHECK(arenasWithOwnModels > 0);
    return failures;
}

} // namespace test
} // namespace mdk
