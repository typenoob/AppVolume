#ifndef QAV_AUDIO_UNIT_VOLUME_H
#define QAV_AUDIO_UNIT_VOLUME_H

#include <AudioToolbox/AudioToolbox.h>
#include <stdbool.h>
#include <stdint.h>

#define QAV_AUDIO_UNIT_BUS_BUCKETS 4
#define QAV_AUDIO_UNIT_CANDIDATES 2
#define QAV_AUDIO_UNIT_ROWS 4
// How many times the panel thread re-probes one unit that still has no usable format before leaving it alone.
#define QAV_AUDIO_UNIT_REPROBES 8

// One row per live unit. An app with both a playback and a capture stream has two output units whose readings
// mean opposite things, so the panel has to say which unit each number belongs to instead of picking one.
typedef struct {
    uint32_t componentType;   // fourcc as reported by AudioComponentGetDescription, 0 when attached at start
    uint32_t componentSubType;
    int32_t speakerIO;        // EnableIO on scope output, element 0: 1 = playback, 0 = disabled, <0 = read failed
    int32_t micIO;            // EnableIO on scope input, element 1
    int32_t feedStatus;       // StreamFormat on (input scope, speaker element)
    int32_t hardwareStatus;   // StreamFormat on (output scope, speaker element)
    int32_t micFormatStatus;
    uint32_t micBitsPerChannel;
    uint32_t speakerChannels;
    uint32_t speakerBytesPerFrame;
    uint32_t candidateCount;
    uint32_t candidateBytesPerFrame[QAV_AUDIO_UNIT_CANDIDATES];
    uint32_t candidateBits[QAV_AUDIO_UNIT_CANDIDATES];
    bool candidateIsFloat[QAV_AUDIO_UNIT_CANDIDATES];
    uint64_t framesPerCandidate[QAV_AUDIO_UNIT_CANDIDATES];
    uint64_t speakerRenders;  // PostRender calls seen on the speaker bus, whether or not a format was resolved
    uint64_t scaledFrames;
    uint32_t renderFrames;
    uint32_t renderBytes;
    uint32_t renderBuffers;
    uint32_t reprobes;
    uint32_t rejectedFormatID;
    uint32_t rejectedFlags;
    uint32_t rejectedBits;
    uint32_t rejectedChannels;
    uint32_t rejectedBytesPerFrame;
    float rejectedSampleRate;
} QAVAudioUnitRow;

typedef struct {
    uint64_t attachedUnits;
    uint64_t detachedUnits;
    uint64_t scaledFrames;
    uint64_t skippedFrames;
    uint64_t geometryMismatches;
    uint64_t formatFailures;
    uint64_t framesPerBus[QAV_AUDIO_UNIT_BUS_BUCKETS];
    int32_t lastError;
    int32_t lastBusNumber;
    uint32_t liveSlots;
    uint32_t rowCount;
    QAVAudioUnitRow rows[QAV_AUDIO_UNIT_ROWS];
    uint64_t formatEvents;
    uint64_t createdInstances;
    uint32_t createdType;
    uint32_t createdSubType;
} QAVAudioUnitDiagnostics;

// Bound into the app's own images by the shared scan in AudioQueueVolume.c.
OSStatus QAVStartOutputUnit(AudioUnit unit);
OSStatus QAVStopOutputUnit(AudioUnit unit);
OSStatus QAVDisposeComponentInstance(AudioComponentInstance instance);
OSStatus QAVNewComponentInstance(AudioComponent inComponent, AudioComponentInstance *outInstance);
OSStatus QAVSetUnitProperty(AudioUnit unit, AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                            const void *inData, UInt32 inDataSize);
OSStatus QAVInitializeUnit(AudioUnit unit);

// Called from the shared app-image scan; the gain is read lock-free by the render thread.
void QAVAudioUnitSetGain(float gain, bool enabled);
QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void);

// Units whose format was only set after AudioOutputUnitStart stay inert; re-probing them from the panel thread
// is what turns 「格式不支持」 into real scaling without touching the render thread.
void QAVAudioUnitRefresh(void);

#endif
