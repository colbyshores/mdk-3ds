// ============================================================================
// updraft_wind.cpp — package 8 (docs/PORT_GAPS.md): the scripts runtime's half of
// Kurt in the fans' updrafts and in the wind zones.
//
//   fan_create (142) ──► Fans ──► Fans.Update: a still fire spark 1 tick in 8 (FanSpark)
//                          │                      └─► Debris, lifted by Fans.Query(mask 8)
//                          └─► Kurt.UpdateUpdraft ──► KurtUpdraft: his arena's fans,
//                                                     else the second arena's (mask 1)
//   wind_zone  (224) ──► WindZone: Kurt in a type-9 box ──► on a floor or sliding:
//                          StartSlide + yaw + SlideAccel(wind); in the air: -128 u/s^2;
//                          off: StopSlide
//
// Ported operation for operation from src/Mdk.Game/Scripts/ScriptRuntime.cs (FanSpark
// :2222, KurtUpdraft :2234, WindZone :2302) and ScriptVm.cs:1166 (opcode 224, in
// script_vm.cpp). Kurt's own half is game/player.cpp (Kurt.Updraft.cs, Kurt.Slide.cs).
// The goldens that pin it are tests/scripts/golden/L*_wind_*.txt, printed by the C#
// probe tests/scripts/wind_probe and by `tick1 dump wind`.
//
// Its own file, not script_runtime.cpp, so the many packages editing that file merge
// around three binding lines instead of these functions.
// ============================================================================

#include <math.h>

#include "game/player.h"
#include "scripts/script_runtime.h"

namespace mdk {
namespace scripts {

namespace {

// ScriptRuntime.DtiWind (ScriptRuntime.cs:79): the DTI records the wind zones are.
const uint32_t kDtiWind = 9;
// FanSpark (ScriptRuntime.cs:2222-2226): SparkColours[Spark.Fire] (0x30, 0x10, the same
// pair items.cpp's kFireSparkColour/kFireSparkRange are) and SparkSize (half a unit).
const int32_t kFanSparkColour = 0x30;
const int32_t kFanSparkRange = 0x10;
const float kFanSparkSize = 0.5f;

// ScriptMath.Contains over Box(record.Position, record.BoxEnd) (ScriptMath.cs:81):
// inclusive on every face.
bool recordContains(const formats::Dti::Record& record, const Vec3& p) {
    return p.x >= record.position[0] && p.y >= record.position[1] &&
           p.z >= record.position[2] && p.x <= record.boxEnd[0] && p.y <= record.boxEnd[1] &&
           p.z <= record.boxEnd[2];
}

} // namespace

void ScriptRuntime::fanSpark(const std::string& arena, const Vec3& point) {
    // C# FanSpark (ScriptRuntime.cs:2222-2226, 0x414230): one fire spark, half a unit
    // big, Launch.Still -- it stands exactly at the point for the updraft to lift.
    debris_.spark(arena, point, 1, kFanSparkSize, kFanSparkColour, kFanSparkRange, 1.0f,
                  Debris::Launch::Still);
}

void ScriptRuntime::fanSparkTrampoline(void* ctx, const std::string& arena, const Vec3& point) {
    static_cast<ScriptRuntime*>(ctx)->fanSpark(arena, point);
}

float ScriptRuntime::kurtUpdraft(const Vec3& feet, float vz, float dt) {
    // C# KurtUpdraft (ScriptRuntime.cs:2234-2243): his arena's fans, else the second
    // arena's -- whether or not the second arena is active, as the C# tests only its
    // name. E.g. LEVEL3's CHMO_6 shaft fan lifts him on through the floor doorway into
    // HMO_8 (golden/L3_wind_shaft.txt).
    const float lifted = fans.query(currentArena_, feet, vz, Fans::kMaskKurt, dt);
    if (!isnan(lifted) || secondArena_.empty()) {
        return lifted;
    }
    return fans.query(secondArena_, feet, vz, Fans::kMaskKurt, dt);
}

float ScriptRuntime::kurtUpdraftTrampoline(void* ctx, float vz, float dt) {
    // ScriptRuntime.cs:345: kurt.Updraft = (vz, dt) => KurtUpdraft(kurt.Feet, vz, dt) --
    // Kurt's LIVE feet at the moment he asks, not the tick's KurtPosition snapshot.
    ScriptRuntime* self = static_cast<ScriptRuntime*>(ctx);
    return self->kurtUpdraft(self->player_->feet, vz, dt);
}

/* @vfp-exempt R6 — the wind's heading: cosf/sinf are the oracle's ScriptMath.FromAngle
 * (MathF.Cos/Sin) and the conversion is .NET's left-associative DegreesToRadians, as
 * script_runtime.cpp's fromAngle; the LUT would move the slide golden/L6_wind_*.txt pins.
 * Once per wind_zone call, not a loop body. measured n/a (corpus empty).
 * Verified 2026-11-20. */
void ScriptRuntime::windZone(const std::string& arena, Wind wind, float yaw, float speed) {
    // C# WindZone (ScriptRuntime.cs:2302-2328, wind_zone 0x4579e0), in its order. The C#
    // cannot run without a Kurt (its constructor takes one); a runtime built without a
    // Player has no one to blow.
    if (player_ == nullptr) {
        return;
    }
    if (wind == Wind::Off) {
        player_->stopSlide();
        return;
    }

    const std::vector<formats::Dti::Record>* records = arenaRecords(arena);
    if (records == nullptr) {
        return;
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        // KurtPosition: the tick's snapshot of his feet (RunTick's first line).
        if (record.type != kDtiWind || !recordContains(record, kurtPosition_)) {
            continue;
        }

        if (player_->sliding() || player_->onFloor) {
            player_->startSlide();
            player_->yaw = yaw;
            // FromAngle(yaw) * speed: (cos, sin) then each times the speed.
            /* @vfp-exempt R6 — ScriptMath.FromAngle's MathF.Cos/Sin; see windZone's pin.
             * measured n/a (corpus empty). Verified 2026-11-20. */
            const float radians = yaw * 3.14159265358979323846f / 180.0f;
            player_->slideAccel(Vec2{cosf(radians) * speed, sinf(radians) * speed}, kTick);
        } else {
            player_->blow(kTick);
        }
        return;
    }
}

} // namespace scripts
} // namespace mdk
