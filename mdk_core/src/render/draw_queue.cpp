// ============================================================================
// draw_queue.cpp — see draw_queue.h.
// ============================================================================

#include "render/draw_queue.h"

namespace mdk {

namespace {

// The starting capacity of each bucket, in draws. A level-3 frame queues about
// 150 batches and almost all of them are `Solid`, so `Solid` starts bigger; the
// others are the glass, mirrors and sprites of one arena. These are starting
// points, not limits: a bucket grows geometrically the first time a frame needs
// more and then never grows again, because `reset` keeps the capacity.
const int32_t kSolidReserve = 64;
const int32_t kOtherReserve = 8;

} // namespace

DrawQueue::DrawQueue() {
    for (int32_t i = 0; i < kDrawQueuePassCount; i++) {
        buckets_[i].reserve(i == (int32_t)Pass::Solid ? kSolidReserve : kOtherReserve);
        // The other arena's batches, while two can overlap: an arena's worth.
        backBuckets_[i].reserve(i == (int32_t)Pass::Solid ? kSolidReserve : kOtherReserve);
    }
}

void DrawQueue::add(const MeshDraw& draw) {
    // The pass is a `Pass` value. A value outside the enum would be a bug in
    // the caller, and the safe reading of that bug is the first bucket: the
    // draw still happens, in the opaque group, rather than vanishing.
    int32_t bucket = (int32_t)draw.material.pass;
    if (bucket < 0 || bucket >= kDrawQueuePassCount) {
        bucket = 0;
    }
    if (draw.back) {
        backBuckets_[bucket].push_back(draw);
        backTotal_++;
    } else {
        buckets_[bucket].push_back(draw);
    }
    total_++;
}

void DrawQueue::reset() {
    for (int32_t i = 0; i < kDrawQueuePassCount; i++) {
        buckets_[i].clear();
        backBuckets_[i].clear();
    }
    total_ = 0;
    backTotal_ = 0;
}

const std::vector<MeshDraw>& DrawQueue::bucket(int32_t pass) const {
    static const std::vector<MeshDraw> kEmpty;
    if (pass < 0 || pass >= kDrawQueuePassCount) {
        return kEmpty;
    }
    return buckets_[pass];
}

const std::vector<MeshDraw>& DrawQueue::backBucket(int32_t pass) const {
    static const std::vector<MeshDraw> kEmpty;
    if (pass < 0 || pass >= kDrawQueuePassCount) {
        return kEmpty;
    }
    return backBuckets_[pass];
}

} // namespace mdk
