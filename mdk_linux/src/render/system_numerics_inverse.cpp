#include "render/system_numerics_inverse.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mdk {
namespace {

struct Vec4f {
    float v[4];
};

struct Vec4i {
    int v[4];
};

struct Matrix4f {
    Vec4f v0;
    Vec4f v1;
    Vec4f v2;
    Vec4f v3;
};

Vec4f shuffle(const Vec4f& a, const Vec4f& b, int control) {
    Vec4f r;
    r.v[0] = a.v[control & 3];
    r.v[1] = a.v[(control >> 2) & 3];
    r.v[2] = b.v[(control >> 4) & 3];
    r.v[3] = b.v[(control >> 6) & 3];
    return r;
}

Vec4f shuffleIndices(const Vec4f& a, const Vec4i& index) {
    Vec4f r;
    r.v[0] = a.v[index.v[0]];
    r.v[1] = a.v[index.v[1]];
    r.v[2] = a.v[index.v[2]];
    r.v[3] = a.v[index.v[3]];
    return r;
}

Vec4i indices(int a, int b, int c, int d) {
    Vec4i r;
    r.v[0] = a;
    r.v[1] = b;
    r.v[2] = c;
    r.v[3] = d;
    return r;
}

Vec4f one() {
    Vec4f r;
    r.v[0] = 1.0f;
    r.v[1] = 1.0f;
    r.v[2] = 1.0f;
    r.v[3] = 1.0f;
    return r;
}

Vec4f mul(const Vec4f& a, const Vec4f& b) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i];
    return r;
}

Vec4f add(const Vec4f& a, const Vec4f& b) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i];
    return r;
}

Vec4f sub(const Vec4f& a, const Vec4f& b) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i];
    return r;
}

Vec4f neg(const Vec4f& a) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &a.v[i], sizeof(bits));
        bits ^= 0x80000000u;
        std::memcpy(&r.v[i], &bits, sizeof(bits));
    }
    return r;
}

// .NET's SSE path lowers MultiplyAddEstimate to a fused multiply-add on this
// target. The cancellation in the 3x3 minors is exact enough that the fused
// rounding is observable, so use std::fma rather than a contracted pair.
Vec4f madd(const Vec4f& a, const Vec4f& b, const Vec4f& c) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) {
        r.v[i] = std::fma(a.v[i], b.v[i], c.v[i]);
    }
    return r;
}

Vec4f div(const Vec4f& a, float d) {
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] / d;
    return r;
}

// .NET's Vector4.Dot reduces the four products pairwise: (x + y) + (z + w).
// The volatiles prevent the compiler from reassociating that order. The old
// sequential reduction differed by 1-2 ULP under cancellation.
float dot(const Vec4f& a, const Vec4f& b) {
    volatile float p0 = a.v[0] * b.v[0];
    volatile float p1 = a.v[1] * b.v[1];
    volatile float p2 = a.v[2] * b.v[2];
    volatile float p3 = a.v[3] * b.v[3];
    volatile float s0 = p0 + p1;
    volatile float s1 = p2 + p3;
    volatile float result = s0 + s1;
    return result;
}

// A close translation of .NET 10's Matrix4x4.Invert SSE/FMA path. The `tN`
// names preserve the IL's locals and operation grouping; do not algebraically
// simplify or reorder them, because the sky matrix is sensitive to the fused
// versus separate rounding of MultiplyAddEstimate and to the exact shuffle
// grouping. Vector4.Dot's pairwise reduction is reproduced explicitly above.
// This is not a claim that every generated instruction is identical: it is
// bit-exact on the realistic camera matrices checked here, while the original
// sequential dot was not on high-cancellation inputs. The one dead branch (a
// singular determinant producing NaN) is reported through the return value.
bool invertBody(const Matrix4f& input, Matrix4f& result) {
    const Vec4f t0 = input.v0;
    const Vec4f t1 = input.v1;
    const Vec4f t2 = input.v2;
    const Vec4f t3 = input.v3;
    const Vec4f t4 = shuffle(t0, t1, 68);
    const Vec4f t5 = shuffle(t0, t1, 238);
    const Vec4f t7 = shuffle(t2, t3, 68);
    const Vec4f t9 = shuffle(t2, t3, 238);
    const Vec4f t11 = shuffle(t4, t7, 136);
    const Vec4f t13 = shuffle(t4, t7, 221);
    const Vec4f t15 = shuffle(t5, t9, 136);
    const Vec4f t17 = shuffle(t5, t9, 221);
    const Vec4i t19 = indices(0, 0, 1, 1);
    const Vec4f t20 = shuffleIndices(t15, t19);
    const Vec4i t22 = indices(2, 3, 2, 3);
    const Vec4f t23 = shuffleIndices(t17, t22);
    const Vec4f t26 = shuffleIndices(t11, t19);
    const Vec4f t29 = shuffleIndices(t13, t22);
    const Vec4f t31 = shuffle(t15, t11, 136);
    const Vec4f t33 = shuffle(t17, t13, 221);
    const Vec4f t35 = mul(t20, t23);
    const Vec4f t37 = mul(t26, t29);
    const Vec4f t39 = mul(t31, t33);
    const Vec4f t42 = shuffleIndices(t15, t22);
    const Vec4f t45 = shuffleIndices(t17, t19);
    const Vec4f t48 = shuffleIndices(t11, t22);
    const Vec4f t51 = shuffleIndices(t13, t19);
    const Vec4f t53 = shuffle(t15, t11, 221);
    const Vec4f t55 = shuffle(t17, t13, 136);
    const Vec4f t57 = neg(t42);
    const Vec4f t58 = madd(t57, t45, t35);
    const Vec4f t60 = neg(t48);
    const Vec4f t61 = madd(t60, t51, t37);
    const Vec4f t63 = neg(t53);
    const Vec4f t64 = madd(t63, t55, t39);
    const Vec4f t66 = shuffle(t58, t64, 93);
    const Vec4i t68 = indices(1, 2, 0, 1);
    const Vec4f t69 = shuffleIndices(t13, t68);
    const Vec4f t71 = shuffle(t66, t58, 50);
    const Vec4i t73 = indices(2, 0, 1, 0);
    const Vec4f t74 = shuffleIndices(t11, t73);
    const Vec4f t76 = shuffle(t66, t58, 153);
    const Vec4f t78 = shuffle(t61, t64, 253);
    const Vec4f t81 = shuffleIndices(t17, t68);
    const Vec4f t83 = shuffle(t78, t61, 50);
    const Vec4f t86 = shuffleIndices(t15, t73);
    const Vec4f t88 = shuffle(t78, t61, 153);
    const Vec4f t90 = mul(t69, t71);
    const Vec4f t92 = mul(t74, t76);
    const Vec4f t94 = mul(t81, t83);
    const Vec4f t96 = mul(t86, t88);
    const Vec4f t98 = shuffle(t58, t64, 4);
    const Vec4i t100 = indices(2, 3, 1, 2);
    const Vec4f t101 = shuffleIndices(t13, t100);
    const Vec4f t103 = shuffle(t58, t98, 147);
    const Vec4i t105 = indices(3, 2, 3, 1);
    const Vec4f t106 = shuffleIndices(t11, t105);
    const Vec4f t108 = shuffle(t58, t98, 38);
    const Vec4f t110 = shuffle(t61, t64, 164);
    const Vec4f t113 = shuffleIndices(t17, t100);
    const Vec4f t115 = shuffle(t61, t110, 147);
    const Vec4f t118 = shuffleIndices(t15, t105);
    const Vec4f t120 = shuffle(t61, t110, 38);
    const Vec4f t122 = neg(t101);
    const Vec4f t123 = madd(t122, t103, t90);
    const Vec4f t125 = neg(t106);
    const Vec4f t126 = madd(t125, t108, t92);
    const Vec4f t128 = neg(t113);
    const Vec4f t129 = madd(t128, t115, t94);
    const Vec4f t131 = neg(t118);
    const Vec4f t132 = madd(t131, t120, t96);
    const Vec4i t134 = indices(3, 0, 3, 0);
    const Vec4f t135 = shuffleIndices(t13, t134);
    const Vec4f t137 = shuffle(t58, t64, 74);
    const Vec4i t139 = indices(0, 3, 2, 0);
    const Vec4f t140 = shuffleIndices(t137, t139);
    const Vec4i t142 = indices(1, 3, 0, 2);
    const Vec4f t143 = shuffleIndices(t11, t142);
    const Vec4f t145 = shuffle(t58, t64, 76);
    const Vec4i t147 = indices(3, 0, 1, 2);
    const Vec4f t148 = shuffleIndices(t145, t147);
    const Vec4f t151 = shuffleIndices(t17, t134);
    const Vec4f t153 = shuffle(t61, t64, 234);
    const Vec4f t156 = shuffleIndices(t153, t139);
    const Vec4f t159 = shuffleIndices(t15, t142);
    const Vec4f t161 = shuffle(t61, t64, 236);
    const Vec4f t164 = shuffleIndices(t161, t147);
    const Vec4f t166 = mul(t135, t140);
    const Vec4f t168 = mul(t143, t148);
    const Vec4f t170 = mul(t151, t156);
    const Vec4f t172 = mul(t159, t164);
    const Vec4f t174 = sub(t123, t166);
    const Vec4f t176 = add(t123, t166);
    const Vec4f t178 = add(t126, t168);
    const Vec4f t180 = sub(t126, t168);
    const Vec4f t182 = sub(t129, t170);
    const Vec4f t184 = add(t129, t170);
    const Vec4f t186 = add(t132, t172);
    const Vec4f t188 = sub(t132, t172);
    const Vec4f t190 = shuffle(t176, t174, 216);
    const Vec4f t192 = shuffle(t180, t178, 216);
    const Vec4f t194 = shuffle(t184, t182, 216);
    const Vec4f t196 = shuffle(t188, t186, 216);
    const Vec4i t198 = indices(0, 2, 1, 3);
    const Vec4f t199 = shuffleIndices(t190, t198);
    const Vec4f t202 = shuffleIndices(t192, t198);
    const Vec4f t205 = shuffleIndices(t194, t198);
    const Vec4f t208 = shuffleIndices(t196, t198);
    const float determinant = dot(t199, t11);
    if (std::fabs(determinant) < 1e-45f) {
        return false;
    }
    const Vec4f inverseDet = div(one(), determinant);
    result.v0 = mul(t199, inverseDet);
    result.v1 = mul(t202, inverseDet);
    result.v2 = mul(t205, inverseDet);
    result.v3 = mul(t208, inverseDet);
    return true;
}

}  // namespace

bool invertSystemNumericsMatrix4(const float in[16], float out[16]) {
    Matrix4f input;
    Matrix4f result;
    input.v0 = {{in[0], in[1], in[2], in[3]}};
    input.v1 = {{in[4], in[5], in[6], in[7]}};
    input.v2 = {{in[8], in[9], in[10], in[11]}};
    input.v3 = {{in[12], in[13], in[14], in[15]}};
    if (!invertBody(input, result)) {
        return false;
    }
    out[0] = result.v0.v[0];  out[1] = result.v0.v[1];
    out[2] = result.v0.v[2];  out[3] = result.v0.v[3];
    out[4] = result.v1.v[0];  out[5] = result.v1.v[1];
    out[6] = result.v1.v[2];  out[7] = result.v1.v[3];
    out[8] = result.v2.v[0];  out[9] = result.v2.v[1];
    out[10] = result.v2.v[2]; out[11] = result.v2.v[3];
    out[12] = result.v3.v[0]; out[13] = result.v3.v[1];
    out[14] = result.v3.v[2]; out[15] = result.v3.v[3];
    return true;
}

}  // namespace mdk
