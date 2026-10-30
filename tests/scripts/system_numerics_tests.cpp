// system_numerics_tests.cpp — see system_numerics_tests.h.
//
// The sky's clip-to-direction inverse is sensitive to the order of the final
// floating-point additions. This pins the C++ implementation to the exact
// output of the .NET 10 oracle on a matrix whose determinant cancels.

#include "system_numerics_tests.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "render/system_numerics_inverse.h"

namespace {

int failures = 0;

uint32_t floatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void testCancellationUsesTheDotnetsPairwiseReduction() {
    // The third invertible matrix from .NET 10.0.402's Random(12345), with
    // values uniformly distributed over [-1, 1]. The expected inverse is the
    // exact System.Numerics.Matrix4x4.Invert output from the same runtime.
    // The old sequential dot reduction differs from these bits; the oracle's
    // pairwise reduction matches all sixteen.
    const uint32_t inputBits[16] = {
        0x3D0FAAFC, 0xBCEF5ABC, 0xBEFD2091, 0x3F0AA386,
        0xBF7EADF9, 0xBD03CABE, 0xBDD52A18, 0xBCDDD516,
        0x3ED8A7BE, 0xBDC7B077, 0xBEEB919C, 0xBEDE2AA1,
        0x3E8A7CB8, 0xBED2258C, 0xBE8598FC, 0x3F349290,
    };
    const uint32_t expectedBits[16] = {
        0x3D0A4562, 0xBF6DD2F3, 0x3E1E8513, 0x3D09D31A,
        0x400FD5EB, 0xBF6DEC12, 0xBF64DBF6, 0xC013E9E3,
        0xBF9E20EE, 0xBEB77C11, 0xBF75C851, 0x3EB02FBA,
        0x3F567AD2, 0xBEA25EAC, 0xBF6F4A19, 0x3E3FC447,
    };

    float input[16];
    for (int i = 0; i < 16; i++) {
        memcpy(&input[i], &inputBits[i], sizeof(input[i]));
    }

    float output[16];
    if (!mdk::invertSystemNumericsMatrix4(input, output)) {
        printf("  FAIL: the oracle's invertible matrix was rejected\n");
        failures++;
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (floatBits(output[i]) != expectedBits[i]) {
            printf("  FAIL: inverse element %d was %08X, wanted %08X\n", i,
                   floatBits(output[i]), expectedBits[i]);
            failures++;
            return;
        }
    }
}

}  // namespace

namespace mdk {
namespace test {

int runSystemNumericsTests() {
    testCancellationUsesTheDotnetsPairwiseReduction();
    return failures;
}

}  // namespace test
}  // namespace mdk
