#ifndef QAV_AUDIO_UNIT_VOLUME_H
#define QAV_AUDIO_UNIT_VOLUME_H

#include <AudioToolbox/AudioToolbox.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t attachedUnits;
    uint64_t detachedUnits;
    uint64_t scaledFrames;
    uint64_t inputFramesSeen;
    uint64_t formatFailures;
    int32_t lastError;
    int32_t lastBusNumber;
    uint32_t lastBitsPerChannel;
    uint32_t lastChannels;
    int32_t lastIsFloat;
} QAVAudioUnitDiagnostics;

// Bound into the app's own images by the shared scan in AudioQueueVolume.c.
OSStatus QAVStartOutputUnit(AudioUnit unit);
OSStatus QAVStopOutputUnit(AudioUnit unit);
OSStatus QAVDisposeComponentInstance(AudioComponentInstance instance);

// Called from the shared app-image scan; the gain is read lock-free by the render thread.
void QAVAudioUnitSetGain(float gain, bool enabled);
QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void);

#endif
