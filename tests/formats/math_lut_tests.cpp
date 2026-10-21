// ============================================================================
// math_lut_tests.cpp — regression tests for the shared trig LUT (R6).
//
// WHY THIS FILE EXISTS. The π/2 defect in fastAtan2 (the ax<ay branch
// subtracted from kTwoPi*0.5 instead of kTwoPi*0.25) survived 155k format
// checks because NOTHING in this suite touched the LUT. A hot-path function
// with a documented accuracy contract needs a test that asserts the
// contract, not just a harness that happens to call it.
//
// The expected values are libm, evaluated at test time. That is deliberate:
// the LUT's contract IS "close to libm", and the host build has libm. The
// tolerances are the measured worst-case errors with margin, from the
// 2026-10-21 grid sweep recorded in math_lut.h.
// ============================================================================

#include "test_support.h"

#include "util/math_lut.h"

#include <cmath>

namespace {

double worstSinErr(double lo, double hi, int samples) {
    double worst = 0.0;
    for (int i = 0; i < samples; i++) {
        double t = (double)i / (double)(samples - 1);
        float x = (float)(lo + (hi - lo) * t);
        double e = std::fabs((double)mdk::fastSin(x) - std::sin(x));
        if (e > worst) worst = e;
    }
    return worst;
}

double worstCosErr(double lo, double hi, int samples) {
    double worst = 0.0;
    for (int i = 0; i < samples; i++) {
        double t = (double)i / (double)(samples - 1);
        float x = (float)(lo + (hi - lo) * t);
        double e = std::fabs((double)mdk::fastCos(x) - std::cos(x));
        if (e > worst) worst = e;
    }
    return worst;
}

double worstAtan2Err(float extent, int steps) {
    double worst = 0.0;
    for (int iy = -steps; iy <= steps; iy++) {
        for (int ix = -steps; ix <= steps; ix++) {
            float y = (float)iy * extent / (float)steps;
            float x = (float)ix * extent / (float)steps;
            if (x == 0.0f && y == 0.0f) continue;
            double e = std::fabs((double)mdk::fastAtan2(y, x) - std::atan2((double)y, (double)x));
            if (e > worst) worst = e;
        }
    }
    return worst;
}

} // namespace

MDK_TEST(MathLutInitIsIdempotent) {
    mdk::mathLutInit();
    mdk::mathLutInit();
    MDK_CHECK(std::fabs(mdk::fastSin(0.0f)) < 1e-6f);
    MDK_CHECK(std::fabs(mdk::fastCos(0.0f) - 1.0f) < 1e-6f);
}

MDK_TEST(FastSinMatchesLibm) {
    mdk::mathLutInit();
    const double kTwoPi = 6.283185307179586;
    // The header's table, with margin: measured 4.76e-06 on [0, 2*PI).
    MDK_CHECK(worstSinErr(0.0, kTwoPi - 1e-6, 200000) < 1e-05);
    MDK_CHECK(worstSinErr(-100.0, 100.0, 200000) < 1e-04);
    MDK_CHECK(worstSinErr(-1000.0, 1000.0, 200000) < 2e-04);
}

MDK_TEST(FastCosMatchesLibm) {
    mdk::mathLutInit();
    const double kTwoPi = 6.283185307179586;
    MDK_CHECK(worstCosErr(0.0, kTwoPi - 1e-6, 200000) < 1e-05);
    MDK_CHECK(worstCosErr(-100.0, 100.0, 200000) < 1e-04);
}

MDK_TEST(FastAtan2MatchesLibm) {
    mdk::mathLutInit();
    // The regression: the ax<ay quadrant (|x| < |y|) was wrong by PI/2.
    MDK_CHECK(std::fabs(mdk::fastAtan2(1.0f, 0.001f) - 1.569796) < 1e-04);
    MDK_CHECK(std::fabs(mdk::fastAtan2(-1.0f, 0.001f) + 1.569796) < 1e-04);
    MDK_CHECK(std::fabs(mdk::fastAtan2(3.0f, 1.0f) - 1.249046) < 1e-04);
    // Measured 3.58e-07 on the [-2,2] grid; the tolerance is the margin.
    MDK_CHECK(worstAtan2Err(2.0f, 400) < 1e-05);
    MDK_CHECK(worstAtan2Err(1000.0f, 200) < 1e-05);
}
