#include "../src/VolumePolicy.h"
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
    puts("PASS: relative gain, independent players, no cumulative attenuation, mute, restore, enable toggle, invalid saved gain, and fade transitions");
    return 0;
}
