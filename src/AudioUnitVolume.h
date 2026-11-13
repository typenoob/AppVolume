#ifndef QAV_AUDIO_UNIT_VOLUME_H
#define QAV_AUDIO_UNIT_VOLUME_H

#include <AudioToolbox/AudioToolbox.h>
#include <stdbool.h>
#include <stdint.h>

#define QAV_AUDIO_UNIT_BUS_BUCKETS 4
#define QAV_AUDIO_UNIT_CANDIDATES 2

typedef struct {
    uint64_t attachedUnits;
    uint64_t detachedUnits;
    uint64_t scaledFrames;
    uint64_t skippedFrames;
    uint64_t geometryMismatches;
    uint64_t formatFailures;
    uint64_t framesPerBus[QAV_AUDIO_UNIT_BUS_BUCKETS];
    uint64_t framesPerCandidate[QAV_AUDIO_UNIT_CANDIDATES];
    int32_t lastError;
    int32_t lastBusNumber;
    int32_t speakerFormatStatus;
    int32_t micFormatStatus;
    uint32_t candidateCount;
    uint32_t candidateBytesPerFrame[QAV_AUDIO_UNIT_CANDIDATES];
    uint32_t candidateBits[QAV_AUDIO_UNIT_CANDIDATES];
    bool candidateIsFloat[QAV_AUDIO_UNIT_CANDIDATES];
    uint32_t speakerChannels;
    uint32_t speakerBytesPerFrame;
    uint32_t micBitsPerChannel;
    int32_t speakerIO;
    int32_t micIO;
    uint32_t renderFrames;
    uint32_t renderBytes;
    uint32_t renderBuffers;
} QAVAudioUnitDiagnostics;

// Bound into the app's own images by the shared scan in AudioQueueVolume.c.
OSStatus QAVStartOutputUnit(AudioUnit unit);
OSStatus QAVStopOutputUnit(AudioUnit unit);
OSStatus QAVDisposeComponentInstance(AudioComponentInstance instance);

// Called from the shared app-image scan; the gain is read lock-free by the render thread.
void QAVAudioUnitSetGain(float gain, bool enabled);
QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void);

// Units whose format was only set after AudioOutputUnitStart stay inert; re-probing them from the panel thread
// is what turns 「格式不支持」 into real scaling without touching the render thread.
void QAVAudioUnitRefresh(void);

#endif
