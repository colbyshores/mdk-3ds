// ============================================================================
// fans.cpp -- see fans.h.
// ============================================================================

#include "scripts/fans.h"

#include <cmath>
#include <limits>
#include <stdio.h>

#include "formats/dti.h"

namespace mdk {
namespace scripts {

namespace {

const uint32_t kFanHotspot = 7;
const float kBoxDrop = 0.5f;
const int32_t kTypeTimed = 6;
const int32_t kEnabled = 1;
const int32_t kSparkChance = 8;
const float kSparkLift = 0.25f;
const float kTop = 5.0f;
const float kTopFade = 0.2f;
const float kWobbleZone = 2.0f;
const float kWobbleLimit = 2.0f;
const float kWobbleStep = 0.1f;
const float kLiftAcceleration = 64.0f;

} // namespace

void Fans::create(const std::string& arena, int32_t hotspot, const std::string& name,
                  int32_t param, int32_t type, float strength) {
    const formats::Dti::Record* record = nullptr;
    if (dti_) {
        const std::vector<formats::Dti::ArenaEntry>& arenas = dti_->arenas();
        for (size_t i = 0; i < arenas.size() && !record; i++) {
            if (arenas[i].name != arena) {
                continue;
            }
            for (size_t r = 0; r < arenas[i].records.size(); r++) {
                if (arenas[i].records[r].type == kFanHotspot && arenas[i].records[r].id == hotspot) {
                    record = &arenas[i].records[r];
                    break;
                }
            }
        }
    }
    if (!record) {
        std::fprintf(stderr, "Cannot find fan hotspot id %d for %s\n", (int)hotspot,
                     name.c_str());
        return;
    }

    Fan fan;
    fan.name = name;
    fan.arena = arena;
    fan.param = param;
    fan.type = type;
    fan.strength = strength;
    fan.boxStart = Vec3{record->position[0], record->position[1], record->position[2] - kBoxDrop};
    fan.boxEnd = Vec3{record->boxEnd[0], record->boxEnd[1], record->boxEnd[2]};
    if (type == kTypeTimed) {
        fan.strength = (record->boxEnd[2] - record->position[2]) / (strength - kBoxDrop);
    }
    fans_.push_back(fan);
}

void Fans::remove(const std::string& arena, const std::string& name) {
    for (size_t i = 0; i < fans_.size(); i++) {
        if (fans_[i].arena == arena && fans_[i].name == name) {
            fans_.erase(fans_.begin() + (long)i);
            return;
        }
    }
}

void Fans::enable(const std::string& arena, const std::string& name, Power power) {
    for (size_t i = 0; i < fans_.size(); i++) {
        if (fans_[i].arena == arena && fans_[i].name == name) {
            fans_[i].mask = power == Power::On ? (fans_[i].mask | kEnabled)
                                               : (fans_[i].mask & ~kEnabled);
        }
    }
}

void Fans::update() {
    if (!rng_) {
        return;
    }
    for (size_t i = 0; i < fans_.size(); i++) {
        const Fan& fan = fans_[i];
        const bool live = isLive_ ? isLive_(isLiveUser_, fan.arena) : true;
        if (!live || rng_->next(kSparkChance) != 0) {
            continue;
        }
        if (spark_) {
            const float x = fan.boxStart.x +
                            (float)rng_->nextDouble() * (fan.boxEnd.x - fan.boxStart.x);
            const float y = fan.boxStart.y +
                            (float)rng_->nextDouble() * (fan.boxEnd.y - fan.boxStart.y);
            spark_(sparkUser_, fan.arena, Vec3{x, y, fan.boxStart.z + kSparkLift});
        }
    }
}

bool Fans::holds(const Fan& fan, const Vec3& point) {
    return point.x >= fan.boxStart.x && point.x <= fan.boxEnd.x && point.y >= fan.boxStart.y &&
           point.y <= fan.boxEnd.y && point.z >= fan.boxStart.z &&
           point.z <= fan.boxEnd.z + kTop;
}

float Fans::query(const std::string& arena, const Vec3& point, float vz, int32_t mask, float dt) {
    bool found = false;
    for (size_t i = 0; i < fans_.size(); i++) {
        Fan& fan = fans_[i];
        if (fan.arena != arena || (fan.mask & mask) == 0 || !holds(fan, point)) {
            continue;
        }
        found = true;
        vz = lift(fan, point.z, vz, dt);
    }
    return found ? vz : std::numeric_limits<float>::quiet_NaN();
}

float Fans::lift(Fan& fan, float z, float vz, float dt) {
    const float height =
        fan.param == 0 ? (z - fan.boxStart.z) / (fan.boxEnd.z - fan.boxStart.z) : 1.0f;
    float factor;
    if (fan.type == 1) {
        factor = 1.0f - height * height;
    } else if (fan.type == 2) {
        factor = (1.0f - height) * (1.0f - height);
    } else if (fan.type == 3) {
        factor = 1.0f - height;
    } else if (fan.type == 4) {
        factor = 1.0f - height * height * height;
    } else if (fan.type == 5) {
        factor = std::pow(1.0f - height, 3.0f);
    } else if (fan.type == kTypeTimed && fan.boxEnd.z - z < kTop) {
        factor = 1.0f - (z - (fan.boxEnd.z - kTop)) * kTopFade;
    } else {
        factor = 1.0f;
    }

    float target = fan.strength * factor;
    if (fan.type == kTypeTimed) {
        if (fan.boxEnd.z - z < kWobbleZone) {
            target += wobble();
        }
        // A faster upward speed is halved towards the target.
        if (target < vz) {
            vz = (vz + target) * 0.5f;
        }
    } else if (factor < 0.0f) {
        target = -kLiftAcceleration;
    }

    return vz < target ? std::min(vz + (target + kLiftAcceleration) * dt, target) : vz;
}

float Fans::wobble() {
    wobble_ += wobbleUp_ ? kWobbleStep : -kWobbleStep;
    if (!wobbleUp_ && wobble_ < -kWobbleLimit) {
        wobbleUp_ = true;
    } else if (wobbleUp_ && wobble_ > kWobbleLimit) {
        wobbleUp_ = false;
    }
    return wobble_;
}

bool Fans::fan(const std::string& arena, const std::string& name, int32_t* maskOut,
               float* strengthOut) const {
    for (size_t i = 0; i < fans_.size(); i++) {
        if (fans_[i].arena == arena && fans_[i].name == name) {
            if (maskOut) {
                *maskOut = fans_[i].mask;
            }
            if (strengthOut) {
                *strengthOut = fans_[i].strength;
            }
            return true;
        }
    }
    return false;
}

} // namespace scripts
} // namespace mdk
