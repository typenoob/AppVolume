#include "../src/VolumePolicy.h"
#include "../src/SampleGain.h"
#include <assert.h>
#include <stdio.h>

static void expect(float actual, float expected) {
    assert(fabsf(actual - expected) < 0.00001f);
}

int main(void) {
    QAVVolumePolicy music = {.requestedVolume = 0.6f};
    QAVVolumePolicy effects = {.requestedVolume = 0.2f};
    expect(QAVEffectiveVolume(music, 0.5f, true), 0.3f);
    expect(QAVEffectiveVolume(effects, 0.5f, true), 0.1f);
    for (int i = 0; i < 1000; i++) {
        expect(QAVEffectiveVolume(music, 0.5f, true), 0.3f);
    }
    expect(music.requestedVolume, 0.6f);
    expect(QAVEffectiveVolume(music, 0.0f, true), 0.0f);
    expect(QAVEffectiveVolume(music, 1.0f, true), 0.6f);
    expect(QAVEffectiveVolume(music, 0.0f, false), 0.6f);
    music.requestedVolume = 0.8f;
    expect(QAVEffectiveVolume(music, 0.25f, true), 0.2f);
    expect(QAVEffectiveVolume(music, 0.25f, false), 0.8f);
    expect(QAVEffectiveVolume(music, 0.25f, true), 0.2f);
    music.requestedVolume = 0.0f;
    expect(QAVEffectiveVolume(music, 1.0f, true), 0.0f);
    expect(QAVNormalizeGain(-2.0f), 0.0f);
    expect(QAVNormalizeGain(20.0f), 1.0f);
    expect(QAVNormalizeGain(NAN), 1.0f);
    expect(QAVNormalizeGain(INFINITY), 1.0f);
    expect(QAVNormalizeGain(-INFINITY), 1.0f);
    expect(QAVScaleVolume(NAN, 0.0f, true), 0.0f);
    expect(QAVScaleVolume(INFINITY, 0.0f, true), 0.0f);
    QAVVolumePolicy fade = QAVBeginFade((QAVVolumePolicy){.requestedVolume = 0.8f}, 0.2f, 4.0, 10.0);
    expect(QAVRequestedVolumeAt(fade, 9.0), 0.8f);
    expect(QAVRequestedVolumeAt(fade, 10.0), 0.8f);
    expect(QAVRequestedVolumeAt(fade, 12.0), 0.5f);
    expect(QAVRequestedVolumeAt(fade, 14.0), 0.2f);
    expect(QAVRequestedVolumeAt(fade, 20.0), 0.2f);
    expect(QAVRemainingFade(fade, 12.0), 2.0f);
    expect(QAVRemainingFade(fade, 20.0), 0.0f);
    expect(QAVScaleVolume(QAVRequestedVolumeAt(fade, 12.0), 0.5f, true), 0.25f);
    expect(QAVScaleVolume(QAVRequestedVolumeAt(fade, 12.0), 0.5f, false), 0.5f);
    fade = QAVBeginFade(fade, 1.0f, 2.0, 12.0);
    expect(QAVRequestedVolumeAt(fade, 12.0), 0.5f);
    expect(QAVRequestedVolumeAt(fade, 13.0), 0.75f);
    expect(QAVRequestedVolumeAt(fade, 14.0), 1.0f);
    fade = QAVBeginFade(fade, 0.4f, 0.0, 13.0);
    expect(QAVRequestedVolumeAt(fade, 13.0), 0.4f);
    fade = QAVBeginFade(fade, 0.6f, -1.0, 13.0);
    expect(QAVRequestedVolumeAt(fade, 13.0), 0.6f);
    float pcm[] = {1.0f, -0.5f, 0.25f, 0.0f};
    QAVApplySampleGain(pcm, sizeof(pcm), 32, true, 0.5f);
    expect(pcm[0], 0.5f);
    expect(pcm[1], -0.25f);
    expect(pcm[2], 0.125f);
    expect(pcm[3], 0.0f);
    int16_t s16[] = {32767, -32768, 1000, 0};
    QAVApplySampleGain(s16, sizeof(s16), 16, false, 0.5f);
    assert(s16[0] == 16384 && s16[1] == -16384 && s16[2] == 500 && s16[3] == 0);
    int32_t s32[] = {1073741823, -1073741824, 65536, 1};
    QAVApplySampleGain(s32, sizeof(s32), 32, false, 0.5f);
    assert(s32[0] == 536870911 && s32[1] == -536870912 && s32[2] == 32768 && s32[3] == 0);
    int16_t passthrough[] = {12345, -9876};
    QAVApplySampleGain(passthrough, sizeof(passthrough), 24, false, 0.5f);
    assert(passthrough[0] == 12345 && passthrough[1] == -9876);
    float unchanged[] = {0.75f, -0.25f};
    QAVApplySampleGain(unchanged, sizeof(unchanged), 32, true, 1.0f);
    expect(unchanged[0], 0.75f);
    expect(unchanged[1], -0.25f);
    QAVApplySampleGain(NULL, 64, 32, true, 0.5f);
    QAVApplySampleGain(s16, 0, 16, false, 0.5f);
    puts("PASS: relative gain, independent players, no cumulative attenuation, mute, restore, enable toggle, invalid saved gain, fade transitions, and sample buffer scaling");
    return 0;
}
