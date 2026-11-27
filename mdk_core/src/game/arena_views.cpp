// ============================================================================
// arena_views.cpp -- see arena_views.h.
// ============================================================================

#include "game/arena_views.h"

#include <stddef.h>

#include <map>

namespace mdk {

namespace {

// A DTI record that joins two arenas (level_data.cpp kConnection).
const uint32_t kConnection = 6;

} // namespace

ArenaViews::~ArenaViews() {
    for (size_t i = 0; i < views_.size(); i++) {
        delete views_[i].view;
    }
}

void ArenaViews::start(MdkRenderer renderer, LevelData* level, SurfaceCache* surfaces,
                       Shading shading) {
    clear();
    renderer_ = renderer;
    level_ = level;
    surfaces_ = surfaces;
    shading_ = shading;
    connections_.clear();
    failed_.clear();
    syncs_ = 0;
    nextSweep_ = kOrphanSweepSyncs;
    lastBuilt_.clear();
    stats_ = Stats();
    if (!level_) {
        return;
    }
    // The connections: a type-6 record in each of the two arenas, sharing an id.
    std::map<int32_t, std::vector<std::string>> byId;
    const std::vector<formats::Dti::ArenaEntry>& entries = level_->dti().arenas();
    for (size_t a = 0; a < entries.size(); a++) {
        for (size_t r = 0; r < entries[a].records.size(); r++) {
            if (entries[a].records[r].type == kConnection) {
                byId[entries[a].records[r].id].push_back(entries[a].name);
            }
        }
    }
    for (std::map<int32_t, std::vector<std::string>>::const_iterator it = byId.begin();
         it != byId.end(); ++it) {
        if (it->second.size() == 2) {
            connections_.push_back(std::make_pair(it->second[0], it->second[1]));
        }
    }
}

bool ArenaViews::keeps(const std::string& name, const std::string& current,
                       const std::string& second, const std::vector<std::string>& drawn,
                       const std::vector<std::string>& hints) const {
    for (size_t i = 0; i < hints.size(); i++) {
        if (hints[i] == name) {
            return true;
        }
    }
    return keeps(name, current, second, drawn);
}

bool ArenaViews::keeps(const std::string& name, const std::string& current,
                       const std::string& second, const std::vector<std::string>& drawn) const {
    // The loaded PAIR and whatever the frame draws (the original's residency,
    // 0x419ac0). Full-level residency was tried 2026-11-22 and the Old 3DS
    // refused it twice: a level's meshes and textures exhaust the 40 MiB main
    // heap at load (OOM at 38.8 MiB) and its linear pool holds a third of a
    // level's ETC1 (762 of 1686 makes fail). What the level now keeps in RAM
    // instead is its whole MTO (game/level_data.cpp), so nothing reads the SD
    // card during play; the pair's mesh and textures are pre-built -- hundreds
    // of frames before a door, with the pump (pumpSurfaces) spreading the
    // makes -- and a departing arena's view is freed here.
    if (name == current || name == second) {
        return true;
    }
    for (size_t i = 0; i < drawn.size(); i++) {
        if (drawn[i] == name) {
            return true;
        }
    }
    // A corridor connected to the pair is kept, as the original kept the
    // corridors of its resident arena: it is where the next door leads, and
    // keeping it is what stops a door that opens and closes from rebuilding
    // its corridor every time.
    if (!level_->mto().has(name) && (connected(name, current) || connected(name, second))) {
        return true;
    }
    return false;
}

bool ArenaViews::connected(const std::string& a, const std::string& b) const {
    if (a.empty() || b.empty()) {
        return false;
    }
    for (size_t i = 0; i < connections_.size(); i++) {
        if ((connections_[i].first == a && connections_[i].second == b) ||
            (connections_[i].first == b && connections_[i].second == a)) {
            return true;
        }
    }
    return false;
}

bool ArenaViews::waiting(const std::string& name) const {
    for (size_t i = 0; i < failed_.size(); i++) {
        if (failed_[i].name == name) {
            return failed_[i].kind == LevelViewBuildFailure::PermanentData ||
                   failed_[i].kind == LevelViewBuildFailure::SceneEmpty ||
                   syncs_ < failed_[i].retryAt;
        }
    }
    return false;
}

void ArenaViews::noteFailed(const std::string& name, LevelViewBuildFailure failure) {
    Failed* entry = nullptr;
    for (size_t i = 0; i < failed_.size(); i++) {
        if (failed_[i].name == name) {
            entry = &failed_[i];
        }
    }
    if (!entry) {
        failed_.push_back(Failed());
        entry = &failed_.back();
        entry->name = name;
    }
    entry->attempts++;
    entry->kind = failure;
    const bool parked = failure == LevelViewBuildFailure::PermanentData ||
                        failure == LevelViewBuildFailure::SceneEmpty;
    entry->retryAt = parked ? INT32_MAX
                            : syncs_ + (entry->attempts <= kQuickRetries ? 1 : kRetrySyncs);
}

void ArenaViews::forgetFailed(const std::string& name) {
    for (size_t i = 0; i < failed_.size(); i++) {
        if (failed_[i].name == name) {
            failed_.erase(failed_.begin() + (std::ptrdiff_t)i);
            return;
        }
    }
}

bool ArenaViews::tear(const std::string& arena, const std::vector<int32_t>& torn) {
    for (size_t i = 0; i < views_.size(); i++) {
        if (views_[i].name == arena) {
            return views_[i].view->tear(torn);
        }
    }
    return true;
}

bool ArenaViews::refresh(const std::string& arena) {
    // C# LevelView.Rebuild (LevelView.cs:251), the Changed subscriber: a built view is
    // rebuilt from the group materials now in the arena. An arena with no view has
    // nothing to rebuild -- its view is built from the same materials when it loads.
    // A group change is a new scene version: it clears a scene-empty or
    // transient failure, but not invalid permanent data.
    for (size_t i = 0; i < views_.size(); i++) {
        if (views_[i].name == arena) {
            const bool applied = views_[i].view->refresh();
            if (applied) {
                forgetFailed(arena);
            } else {
                noteFailed(arena, LevelViewBuildFailure::TransientAllocation);
            }
            return applied;
        }
    }
    for (size_t i = 0; i < failed_.size(); i++) {
        if (failed_[i].name == arena && failed_[i].kind != LevelViewBuildFailure::PermanentData) {
            forgetFailed(arena);
            break;
        }
    }
    return true;
}

const LevelView* ArenaViews::find(const std::string& name) const {
    for (size_t i = 0; i < views_.size(); i++) {
        if (views_[i].name == name) {
            return views_[i].view;
        }
    }
    return nullptr;
}

LevelView* ArenaViews::find(const std::string& name) {
    for (size_t i = 0; i < views_.size(); i++) {
        if (views_[i].name == name) {
            return views_[i].view;
        }
    }
    return nullptr;
}

bool ArenaViews::build(const std::string& name) {
    const formats::Arena* arena = level_->arenaNamed(name);
    if (!arena) {
        return false;
    }
    LevelView* view = new LevelView();
    view->setShading(shading_);
    if (!view->loadBegin(renderer_, level_, arena, surfaces_)) {
        // An arena with no geometry, or no GPU memory for its mesh right now:
        // tried again later (see the header), not in every frame and not never.
        const LevelViewBuildFailure failure = view->buildFailure();
        view->unload();
        delete view;
        noteFailed(name, failure);
        stats_.failed++;
        return false;
    }
    Entry entry;
    entry.name = name;
    entry.view = view;
    entry.hasBox = view->bounds(&entry.boxMin, &entry.boxMax);
    views_.push_back(entry);
    stats_.built++;
    lastBuilt_ = name;
    return true;
}

bool ArenaViews::paletteInUse(const formats::Palette& palette, size_t exceptIndex) {
    for (size_t i = 0; i < views_.size(); i++) {
        if (i == exceptIndex) {
            continue;
        }
        const formats::Arena* other = level_->arenaNamed(views_[i].name);
        if (other && &level_->paletteOf(other) == &palette) {
            return true;
        }
    }
    return false;
}

void ArenaViews::release(size_t index) {
    const Entry entry = views_[index];
    entry.view->unload();
    // Everything drawn in this arena's colours: its own textures, the level's
    // through its palette, its objects'. The cache makes them again on the next
    // draw that asks, so nothing has to know they went.
    if (const formats::Arena* arena = level_->arenaNamed(entry.name)) {
        // A corridor borrows its arena's palette OBJECT (LevelData::paletteOf),
        // so the palette can outlive the arena that made it. Evicting it under a
        // resident borrower would drop that view's textures mid-play and the draw
        // would re-expand them: measured 134 ms two frames after the drop
        // (Old 3DS, 2026-11-23, frame 827).
        formats::Palette* palette = &level_->paletteOf(arena);
        if (!paletteInUse(*palette, index)) {
            surfaces_->releasePalette(palette);
        }
    }
    delete entry.view;
    views_.erase(views_.begin() + (std::ptrdiff_t)index);
    stats_.released++;
}

int32_t ArenaViews::sync(const std::string& current, const std::string& second) {
    std::vector<std::string> drawn;
    drawn.push_back(current);
    return sync(current, second, drawn);
}

int32_t ArenaViews::sync(const std::string& current, const std::string& second,
                         const std::vector<std::string>& drawn) {
    const std::vector<std::string> noHints;
    return sync(current, second, drawn, noHints);
}

int32_t ArenaViews::sync(const std::string& current, const std::string& second,
                         const std::vector<std::string>& drawn,
                         const std::vector<std::string>& hints) {
    if (!renderer_ || !level_ || !surfaces_) {
        return 0;
    }
    syncs_++;
    // A failure belongs to the residency that produced it: leaving the kept
    // set resets it, so a later approach starts from a clean history.
    for (size_t i = failed_.size(); i > 0; i--) {
        if (!keeps(failed_[i - 1].name, current, second, drawn, hints)) {
            failed_.erase(failed_.begin() + (std::ptrdiff_t)(i - 1));
        }
    }
    // Free first. (On the 3DS a release settles at the next frame's start, not
    // here, which is one reason a build can fail and is retried.)
    for (size_t i = views_.size(); i > 0; i--) {
        if (!keeps(views_[i - 1].name, current, second, drawn, hints)) {
            release(i - 1);
        }
    }
    int32_t built = 0;
    // The pair, then whatever else is drawn, then the bubble's hints.
    // Hints are KEPT (their views survive) and built ahead in the bubble's
    // order, nearest first, ONE AT A TIME: the first hinted name with no view
    // yet, once every hint before it is ready. With the bubble's second hop
    // (ScriptRuntime::doorBubbleArenas) the first hint is often a connector
    // already built and the arena beyond it is the one Kurt reaches next --
    // building only hints[0] left LEVEL4's MEAT_5 to the synchronous finish
    // (257 ms on the Old 3DS, 2026-11-27) behind its 31-unit CMEAT_5. The
    // hint cap (three, in main) bounds the hinted views; each is a mesh of
    // tens to a few hundred KiB, and their textures ride the shared cache
    // budget (level 8's in-play linear minimum is ~2.4 MB: docs/WORK_LEDGER.md).
    for (size_t w = 0; w < 2 + drawn.size() + hints.size(); w++) {
        const bool isHint = w >= 2 + drawn.size();
        const std::string& name =
            w == 0 ? current
                   : (w == 1 ? second
                             : (w < 2 + drawn.size() ? drawn[w - 2] : hints[w - 2 - drawn.size()]));
        if (name.empty()) {
            continue;
        }
        if (const LevelView* view = find(name)) {
            if (isHint && !view->ready()) {
                break;   // a hint still building: the next hint waits for it
            }
            continue;
        }
        if (waiting(name)) {
            continue;
        }
        if (build(name)) {
            built++;
            // One NEW build started per sync: the second pair member
            // starts next frame, so two arenas' builds (and their depth
            // phases) never stack into one frame or one memory peak.
            break;
        }
    }
    // THE GUARANTEE: a view the frame DRAWS is finished here, in the view
    // phase (outside the frame, where GPU resources may be made), never
    // left half-built for draw() to skip. The bubble exists to make this
    // rare; when it fires, the cost is the old synchronous build, once.
    // A drawn arena with no view at all is built here too, subject to
    // the same retry schedule as the build loop (a persistently failing
    // arena backs off like any other; the pump retries it): the
    // guarantee's force is finishing a build in flight, not retrying a
    // failed one every frame.
    int32_t guaranteeStepsLeft = 8192;   // aggregate across all drawn views
    for (size_t d = 0; d < drawn.size(); d++) {
        if (drawn[d].empty()) {
            continue;
        }
        LevelView* view = find(drawn[d]);
        if (view == nullptr && !waiting(drawn[d])) {
            if (build(drawn[d])) {
                built++;
                view = find(drawn[d]);
            }
        }
        if (view != nullptr && !view->ready() && !view->buildFailed() && !view->notBuilding()) {
            // Driven on state, not buildStep's return (the header states
            // it: true exactly at Ready), and capped far past any real
            // build (the largest arena is ~50 slices: depth units, 1200-
            // triangle loop steps, Mesh, Large): every buildStep call
            // advances a cursor or changes state by construction, so the
            // cap is a belt against a future no-progress step, and the
            // pump keeps stepping past it on later syncs regardless.
            // The full finish in one sync is the point (the old
            // synchronous build, once, on a bubble miss); counted only
            // when the view actually reaches Ready.
            const int32_t kGuaranteeMaxSteps = 4096;
            int32_t steps = 0;
            while (!view->ready() && !view->buildFailed() && !view->notBuilding() &&
                   steps < kGuaranteeMaxSteps && guaranteeStepsLeft > 0) {
                view->buildStep();
                steps++;
                guaranteeStepsLeft--;
            }
            stats_.buildSteps += steps;
            if (view->ready()) {
                stats_.syncFinishes++;
            }
        }
    }
    // One build step a sync, for ONE view still building (the door pump:
    // an incoming arena's view is made over the approach frames, not in
    // one sync -- and one build in flight at a time, so two arenas'
    // intermediate vertex buffers never peak together). A build that
    // fails here takes the same retry path as a load that failed outright.
    // Oldest first: the arena the player is standing in finishes before
    // the one being walked to (a reverse sweep would starve it behind
    // every newer build).
    for (size_t i = 0; i < views_.size(); i++) {
        LevelView* view = views_[i].view;
        if (view->ready()) {
            continue;
        }
        view->buildStep();
        stats_.buildSteps++;
        if (view->buildFailed() || view->notBuilding()) {
            const std::string name = views_[i].name;
            const LevelViewBuildFailure failure = view->buildFailure();
            release(i);
            noteFailed(name, failure);
            stats_.failed++;
            i--;   // the next view shifted into this slot; step it next sync
            continue;
        }
        break;   // one build step a sync (the cap above)
    }
    // A refresh recorded while a view was building, or one whose mesh
    // creation failed, is retried here under the same transient backoff.
    for (size_t i = 0; i < views_.size(); i++) {
        LevelView* view = views_[i].view;
        if (!view->ready() || !view->pendingRefresh() || waiting(views_[i].name)) {
            continue;
        }
        if (view->retryPendingRefresh()) {
            forgetFailed(views_[i].name);
        } else {
            noteFailed(views_[i].name, LevelViewBuildFailure::TransientAllocation);
        }
    }
    // Publication is the only success that clears failure history: a start
    // that later failed at the mesh must not reset its backoff. A Ready view
    // with a refresh still pending is NOT a success -- the retry loop above
    // has just recorded its failure for this sync, and erasing it here would
    // restart the ladder at attempt 1, retrying a full scene rebuild every
    // sync forever (measured on the ladder's shape 2026-11-24: attempts never
    // reached kQuickRetries, so kRetrySyncs was unreachable). The retry
    // loop's own forgetFailed on success is what clears the history.
    for (size_t i = 0; i < views_.size(); i++) {
        if (views_[i].view->ready() && !views_[i].view->pendingRefresh()) {
            forgetFailed(views_[i].name);
        }
    }
    // WHICH VIEWS RE-ASK (LevelView::pumpSurfaces): the pair and the drawn ones
    // only. A packed original evicted there is about to be drawn again; one of a
    // hint-only or corridor view is not, and re-asking those would fill the
    // linear pool with textures nobody is looking at.
    for (size_t i = 0; i < views_.size(); i++) {
        const std::string& name = views_[i].name;
        bool nearPlayer = name == current || name == second;
        for (size_t d = 0; d < drawn.size() && !nearPlayer; d++) {
            nearPlayer = drawn[d] == name;
        }
        views_[i].reask = nearPlayer;
    }
    // Every kOrphanSweepSyncs, counted forward (no remainder: the ARM11 has no
    // integer divide, AGENTS.md R11).
    if (syncs_ >= nextSweep_) {
        nextSweep_ = syncs_ + kOrphanSweepSyncs;
        sweepOrphans(current, second, drawn);
    }
    return built;
}


void ArenaViews::sweepOrphans(const std::string& current, const std::string& second,
                              const std::vector<std::string>& drawn) {
    const std::vector<const formats::Arena*>& arenas = level_->arenas();
    for (size_t i = 0; i < arenas.size(); i++) {
        const std::string& name = arenas[i]->name();
        if (find(name) || keeps(name, current, second, drawn)) {
            continue;
        }
        const formats::Palette* palette = &level_->paletteOf(arenas[i]);
        // Same guard as release(): a resident corridor can be drawing through
        // this arena's palette object even though the arena itself is gone.
        if (surfaces_->hasPalette(palette) && !paletteInUse(*palette, views_.size())) {
            surfaces_->releasePalette(palette);
            stats_.orphansReleased++;
        }
    }
}

void ArenaViews::pumpSurfaces(int32_t budget) {
    // The frame's whole budget goes to the views that still have surfaces to
    // make: a finished view costs one comparison and spends nothing. The start
    // rotates so no unfinished view is starved by the ones before it.
    const size_t n = views_.size();
    if (n == 0 || budget <= 0) {
        return;
    }
    if (pumpCursor_ >= n) {
        pumpCursor_ = 0;
    }
    for (int32_t k = 0; k < (int32_t)n && budget > 0; k++) {
        // Wrap by subtraction, not `%`: both terms are < n, so one subtraction
        // is the exact wrap (and this ARM11 has no divide instruction).
        size_t i = pumpCursor_ + (size_t)k;
        if (i >= n) {
            i -= n;
        }
        // The frame's budget is an upper bound on the makes a frame can do:
        // each view spends exactly the batches it pumped, so a view finishing
        // its tail cannot hand the budget to the next view as well.
        budget -= views_[i].view->pumpSurfaces(budget, views_[i].reask);
    }
    pumpCursor_ += 1;
    if (pumpCursor_ >= n) {
        pumpCursor_ = 0;
    }
}

std::string ArenaViews::draw(const std::vector<std::string>& drawn,
                             const std::string& cameraArena, const Vec3& eye, float eyeMargin) {
    // THE CAMERA'S ARENA OVER THE OTHER ONE (see the header). With two arenas
    // drawn: the camera's arena's box, grown to take in the eye; the box around
    // the other arena's batches that cut it. When there are any, the other arena
    // is drawn `back` and the camera arena's batches that can reach them `cover`.
    const Entry* cover = nullptr;
    const Entry* back = nullptr;
    if (drawn.size() == 2 && drawn[0] != drawn[1]) {
        for (size_t i = 0; i < views_.size(); i++) {
            if (views_[i].name != drawn[0] && views_[i].name != drawn[1]) {
                continue;
            }
            if (views_[i].name == cameraArena) {
                cover = &views_[i];
            } else {
                back = &views_[i];
            }
        }
    }
    Vec3 reachMin{};
    Vec3 reachMax{};
    Vec3 backMin{};
    Vec3 backMax{};
    bool layered = false;
    const float margin = eyeMargin > kLayerEyeMargin ? eyeMargin : kLayerEyeMargin;
    if (cover && back && cover->hasBox) {
        boxWithEye(cover->boxMin, cover->boxMax, eye, margin, &reachMin, &reachMax);
        layered = back->view->batchesCutting(reachMin, reachMax, &backMin, &backMax);
    }
    for (size_t d = 0; d < drawn.size(); d++) {
        for (size_t i = 0; i < views_.size(); i++) {
            if (views_[i].name == drawn[d]) {
                // Normally ready (sync's guarantee finished it, or the
                // bubble built it during the approach); a view that is
                // still building -- or failed and is waiting out its
                // retry -- draws nothing this frame.
                if (layered && &views_[i] == back) {
                    views_[i].view->draw(ArenaLayer::Back, backMin, backMax, eye, margin);
                } else if (layered && &views_[i] == cover) {
                    views_[i].view->draw(ArenaLayer::Cover, backMin, backMax, eye, margin);
                } else {
                    views_[i].view->draw(ArenaLayer::Plain, backMin, backMax, eye, margin);
                }
                break;
            }
        }
    }
    return layered ? back->name : std::string();
}

void ArenaViews::clear() {
    if (renderer_ && level_ && surfaces_) {
        for (size_t i = views_.size(); i > 0; i--) {
            release(i - 1);
        }
    }
    for (size_t i = 0; i < views_.size(); i++) {
        delete views_[i].view;
    }
    views_.clear();
}

} // namespace mdk
