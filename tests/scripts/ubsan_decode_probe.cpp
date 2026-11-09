// ============================================================================
// ubsan_decode_probe.cpp — the 8-bit PCM decodes, exercised for the compiler to
// put under -fsanitize=undefined. See ubsan_decode_test.sh for why a script
// builds this at all instead of adding it to vm_tests.
//
// WHAT IT IS FOR. Batch 95 replaced `(x - 128) << 8` with `(x - 128) * 256` in
// two decodes, because left-shifting a negative signed value is undefined in
// C++17 and half of the byte domain is negative. The replacement is
// VALUE-IDENTICAL — on this host and on the target the emitted instruction is the
// same `lsl #8` — which is precisely why a value test cannot pin it: reverting
// the multiply to a shift changes no output and no golden, and only a sanitizer
// says anything about it. Batch 95 proved that with an out-of-tree UBSan run and
// committed nothing, so the proof could not be reproduced by anyone (the loop has
// now been told repeatedly that an asserted provenance is not a provenance). This
// file is that run, in the tree.
//
// WHY IT IS ITS OWN BINARY. vm_tests cannot be built with the sanitizer without
// rebuilding every core object with it, and a check that takes ten minutes gets
// skipped. The two decodes and their reader are three translation units.
//
// WHY IT ALSO ASSERTS. A probe whose only output is "the sanitizer stayed quiet"
// passes when it decoded nothing, so every path here counts its own assertions
// and the exit code is the failure count. The MVE half is the committed test
// (mve_audio_tests.cpp), reused rather than duplicated.
// ============================================================================

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/sound.h"
#include "mve_audio_tests.h"

using namespace mdk;

namespace {

int failures = 0;

void expect(const char* what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL: %s was %lld, the oracle has %lld\n", what, got, want);
        failures++;
    }
}

// The oracle's int16 for one byte, in the defined form (audio/AudioDevice.cs:
// (data[i] - ByteMiddle) * ByteScale, and the C#'s Mve.cs (b - 128) << 8).
int16_t oracleStore(uint8_t b) { return (int16_t)(((int32_t)b - 128) * 256); }

// Sound::soundFromPcm over the whole byte domain, one byte per decode so a
// report names the byte. This is the path whose shift batch 95 removed first.
void sweepSoundFromPcm() {
    int stored = 0;
    for (int b = 0; b < 256; b++) {
        const uint8_t byte = (uint8_t)b;
        audio::Sound sound;
        audio::soundFromPcm(&byte, 1, 1, 22050, 8, false, &sound);
        if (sound.samples.size() != 1) {
            printf("  FAIL: byte %d decoded to %d samples, expected 1\n", b,
                   (int)sound.samples.size());
            failures++;
            continue;
        }
        stored++;
        const int16_t want = oracleStore((uint8_t)b);
        if (sound.samples[0] != want) {
            printf("  FAIL: byte %d stored %d, the oracle has %d\n", b,
                   (int)sound.samples[0], (int)want);
            failures++;
        }
    }
    expect("every byte of the domain reached the store", stored, 256);
}

// And the 16-bit path of the same function, so the sanitizer has walked both
// branches of the decode it is here for.
void sweepSoundFromPcm16() {
    const int16_t shorts[] = {-32768, -1, 0, 1, 512, 32767};
    uint8_t bytes[sizeof(shorts)];
    for (size_t i = 0; i < sizeof(shorts) / sizeof(shorts[0]); i++) {
        memcpy(bytes + i * 2, &shorts[i], 2);
    }
    audio::Sound sound;
    audio::soundFromPcm(bytes, (int32_t)sizeof(bytes), 1, 22050, 16, false, &sound);
    expect("the 16-bit path decoded one sample per pair",
           (long long)sound.samples.size(), (long long)(sizeof(shorts) / sizeof(shorts[0])));
    for (size_t i = 0; i < sizeof(shorts) / sizeof(shorts[0]) && i < sound.samples.size(); i++) {
        if (sound.samples[i] != shorts[i]) {
            printf("  FAIL: 16-bit sample %zu stored %d, the oracle has %d\n", i,
                   (int)sound.samples[i], (int)shorts[i]);
            failures++;
        }
    }
}

}  // namespace

int main() {
    sweepSoundFromPcm();
    sweepSoundFromPcm16();
    failures += mdk::test::runMveAudioTests();
    if (failures == 0) {
        printf("probe ran both 8-bit decodes and both 16-bit paths\n");
        return 0;
    }
    printf("%d probe check(s) FAILED\n", failures);
    return 1;
}
