#include "AudioUnitVolume.h"
#include "SampleGain.h"
#include "VolumePolicy.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

// Apps like Douyin drive the speaker from a RemoteIO / VoiceProcessingIO unit they create themselves, with
// their own software mixer inside. The one boundary we can scale without touching system volume is the
// AudioBufferList the unit hands to the hardware, so a render notify is attached to each app-owned unit.
//
// RemoteIO numbers its streams the other way round from an ordinary AU: AudioUnitProperties.h documents the
// hardware side as 「scope output, element 0 = output」 and 「scope input, element 1 = input」, which is why
// aurioTouch uses kOutputBus = 0 and kInputBus = 1. The render notify reports that same element in inBusNumber,
// so the speaker feed is bus 0 and every other bus is a path whose levels belong to the app.

#define QAV_SPEAKER_BUS 0
#define QAV_MIC_BUS 1
#define QAV_LAST_BUS_BUCKET (QAV_AUDIO_UNIT_BUS_BUCKETS - 1)
#define QAV_UNIT_SLOTS 32
#define QAV_FORMAT_REPROBES 8

typedef struct {
    _Atomic(void *) unit;
    _Atomic uint32_t bitsPerChannel;
    _Atomic uint32_t bytesPerBuffer;
    _Atomic uintptr_t isFloat;
    _Atomic uint64_t scaledFrames;
    bool claimed;
    uint32_t reprobes;
} QAVUnitSlot;

static pthread_mutex_t QAVUnitLock = PTHREAD_MUTEX_INITIALIZER;
static QAVUnitSlot QAVUnitSlots[QAV_UNIT_SLOTS];
static _Atomic float QAVUnitGain = 1.0f;
static _Atomic bool QAVUnitEnabled = true;
static _Atomic uint64_t QAVUnitAttached;
static _Atomic uint64_t QAVUnitDetached;
static _Atomic uint64_t QAVUnitSkippedFrames;
static _Atomic uint64_t QAVUnitGeometryMismatches;
static _Atomic uint64_t QAVUnitFormatFailures;
static _Atomic uint64_t QAVUnitFramesPerBus[QAV_AUDIO_UNIT_BUS_BUCKETS];
static _Atomic int32_t QAVUnitLastError;
static _Atomic int32_t QAVUnitLastBus;
static _Atomic int32_t QAVUnitSpeakerFormatStatus;
static _Atomic int32_t QAVUnitMicFormatStatus;
static _Atomic uint32_t QAVUnitSpeakerBits;
static _Atomic uint32_t QAVUnitSpeakerChannels;
static _Atomic int32_t QAVUnitSpeakerIsFloat;
static _Atomic uint32_t QAVUnitMicBits;
static _Atomic int32_t QAVUnitSpeakerIO;
static _Atomic int32_t QAVUnitMicIO;

// The iOS SDK types the notify proc as AURenderCallback, so match that shape exactly.
static OSStatus QAVGainRenderNotify(void *inRefCon, AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
    (void)inTimeStamp;
    if (!(*actionFlags & kAudioUnitRenderAction_PostRender)) return noErr;
    QAVUnitSlot *slot = (QAVUnitSlot *)inRefCon;
    if (!slot || !atomic_load_explicit(&slot->unit, memory_order_acquire)) return noErr;
    size_t bucket = inBusNumber > QAV_LAST_BUS_BUCKET ? QAV_LAST_BUS_BUCKET : (size_t)inBusNumber;
    atomic_fetch_add_explicit(&QAVUnitFramesPerBus[bucket], inNumberFrames, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastBus, (int32_t)inBusNumber, memory_order_relaxed);
    uint32_t bits = atomic_load_explicit(&slot->bitsPerChannel, memory_order_acquire);
    if (!bits || !ioData) return noErr;
    if (inBusNumber != QAV_SPEAKER_BUS) {
        // Any other bus is a path the app owns the level of, such as the microphone on element 1.
        atomic_fetch_add_explicit(&QAVUnitSkippedFrames, inNumberFrames, memory_order_relaxed);
        return noErr;
    }
    float gain = atomic_load_explicit(&QAVUnitGain, memory_order_relaxed);
    if (!atomic_load_explicit(&QAVUnitEnabled, memory_order_relaxed) || gain == 1.0f) return noErr;
    uint32_t bytesPerBuffer = atomic_load_explicit(&slot->bytesPerBuffer, memory_order_relaxed);
    bool isFloat = atomic_load_explicit(&slot->isFloat, memory_order_relaxed) != 0;
    // The recorded format is only an assumption about this render until the geometry agrees with it, and scaling a
    // buffer of another sample type would corrupt it, so a mismatch leaves every buffer untouched.
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        AudioBuffer *entry = &ioData->mBuffers[buffer];
        if (!entry->mDataByteSize) continue;
        if (!entry->mData || entry->mDataByteSize != (UInt32)inNumberFrames * bytesPerBuffer) {
            atomic_fetch_add_explicit(&QAVUnitGeometryMismatches, 1, memory_order_relaxed);
            return noErr;
        }
    }
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        QAVApplySampleGain(ioData->mBuffers[buffer].mData, ioData->mBuffers[buffer].mDataByteSize, bits, isFloat, gain);
    }
    atomic_fetch_add_explicit(&slot->scaledFrames, inNumberFrames, memory_order_relaxed);
    return noErr;
}

// Returns the packed byte size of one rendered buffer for this format, or 0 when the layout is not one we scale.
static uint32_t QAVScalableBytesPerBuffer(const AudioStreamBasicDescription *format) {
    if (format->mFormatID != kAudioFormatLinearPCM) return 0;
    if (format->mBitsPerChannel != 16 && format->mBitsPerChannel != 32) return 0;
    if (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 64) return 0;
    if ((format->mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0 && format->mBitsPerChannel != 32) return 0;
    uint32_t bytesPerSample = format->mBitsPerChannel / 8;
    bool nonInterleaved = (format->mFormatFlags & kLinearPCMFormatFlagIsNonInterleaved) != 0;
    uint32_t expected = nonInterleaved ? bytesPerSample : bytesPerSample * format->mChannelsPerFrame;
    // Scaling walks a packed run of samples; anything padded or aliased is refused rather than guessed at.
    return format->mBytesPerFrame == expected ? expected : 0;
}

#define QAV_FORMAT_TOO_SHORT -1

static OSStatus QAVProbeFormat(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element, AudioStreamBasicDescription *format) {
    UInt32 size = sizeof(*format);
    OSStatus status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, format, &size);
    if (status == noErr && size != sizeof(*format)) status = QAV_FORMAT_TOO_SHORT;
    return status;
}

static void QAVRecordStreamFormat(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element, _Atomic int32_t *statusSlot,
                                  _Atomic uint32_t *bitsSlot, _Atomic uint32_t *channelsSlot, _Atomic int32_t *isFloatSlot) {
    AudioStreamBasicDescription format;
    OSStatus status = QAVProbeFormat(unit, scope, element, &format);
    atomic_store_explicit(statusSlot, (int32_t)status, memory_order_relaxed);
    if (status != noErr) return;
    atomic_store_explicit(bitsSlot, format.mBitsPerChannel, memory_order_relaxed);
    if (channelsSlot) atomic_store_explicit(channelsSlot, format.mChannelsPerFrame, memory_order_relaxed);
    if (isFloatSlot) atomic_store_explicit(isFloatSlot, (format.mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0 ? 1 : 0, memory_order_relaxed);
}

// The speaker feed is described on the input scope of element 0; the hardware-side description is the fallback.
static bool QAVCopySpeakerFormat(AudioUnit unit, AudioStreamBasicDescription *format) {
    static const AudioUnitScope scopes[] = { kAudioUnitScope_Input, kAudioUnitScope_Output };
    for (size_t index = 0; index < sizeof(scopes) / sizeof(scopes[0]); index++) {
        if (QAVProbeFormat(unit, scopes[index], QAV_SPEAKER_BUS, format) == noErr && QAVScalableBytesPerBuffer(format) != 0) return true;
    }
    return false;
}

static int32_t QAVCopyIOFlag(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element) {
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    OSStatus status = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_EnableIO, scope, element, &value, &size);
    if (status != noErr || size != sizeof(value)) return (int32_t)status;
    return (int32_t)value;
}

static void QAVApplyFormatToSlot(QAVUnitSlot *slot, const AudioStreamBasicDescription *format) {
    // The render thread acquires bitsPerChannel before reading the rest, so the description has to be published first.
    atomic_store(&slot->bytesPerBuffer, QAVScalableBytesPerBuffer(format));
    atomic_store(&slot->isFloat, (uintptr_t)((format->mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0));
    atomic_store_explicit(&slot->bitsPerChannel, format->mBitsPerChannel, memory_order_release);
}

static QAVUnitSlot *QAVSlotForUnitLocked(AudioUnit unit) {
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (atomic_load(&QAVUnitSlots[index].unit) == unit) return &QAVUnitSlots[index];
    }
    return NULL;
}

static QAVUnitSlot *QAVClaimSlotLocked(void) {
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (QAVUnitSlots[index].claimed) continue;
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        slot->claimed = true;
        slot->reprobes = 0;
        atomic_store(&slot->scaledFrames, 0);
        atomic_store(&slot->bitsPerChannel, 0);
        atomic_store(&slot->bytesPerBuffer, 0);
        atomic_store(&slot->isFloat, 0);
        atomic_store(&slot->unit, NULL);
        return slot;
    }
    atomic_store_explicit(&QAVUnitLastError, -1, memory_order_relaxed);
    return NULL;
}

// A unit whose format is unusable still gets a notify attached: the per-bus frame counts are what shows whether the
// app really renders its playback on the speaker bus, and QAVAudioUnitRefresh picks up a format set after start.
static void QAVAttachUnit(AudioUnit unit) {
    if (!unit) return;
    AudioStreamBasicDescription format;
    bool scalable = QAVCopySpeakerFormat(unit, &format);
    atomic_store_explicit(&QAVUnitSpeakerIO, QAVCopyIOFlag(unit, kAudioUnitScope_Output, QAV_SPEAKER_BUS), memory_order_relaxed);
    atomic_store_explicit(&QAVUnitMicIO, QAVCopyIOFlag(unit, kAudioUnitScope_Input, QAV_MIC_BUS), memory_order_relaxed);
    QAVRecordStreamFormat(unit, kAudioUnitScope_Input, QAV_SPEAKER_BUS, &QAVUnitSpeakerFormatStatus,
                          &QAVUnitSpeakerBits, &QAVUnitSpeakerChannels, &QAVUnitSpeakerIsFloat);
    QAVRecordStreamFormat(unit, kAudioUnitScope_Output, QAV_MIC_BUS, &QAVUnitMicFormatStatus, &QAVUnitMicBits, NULL, NULL);
    if (!scalable) atomic_fetch_add_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);

    pthread_mutex_lock(&QAVUnitLock);
    if (QAVSlotForUnitLocked(unit)) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    QAVUnitSlot *slot = QAVClaimSlotLocked();
    if (!slot) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    if (scalable) QAVApplyFormatToSlot(slot, &format);
    OSStatus status = AudioUnitAddRenderNotify(unit, QAVGainRenderNotify, slot);
    if (status != noErr) {
        slot->claimed = false;
        atomic_store_explicit(&QAVUnitLastError, (int32_t)status, memory_order_relaxed);
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    atomic_store(&slot->unit, unit);
    atomic_fetch_add_explicit(&QAVUnitAttached, 1, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastError, 0, memory_order_relaxed);
    pthread_mutex_unlock(&QAVUnitLock);
}

static void QAVDetachUnit(AudioUnit unit) {
    if (!unit) return;
    pthread_mutex_lock(&QAVUnitLock);
    QAVUnitSlot *slot = QAVSlotForUnitLocked(unit);
    if (!slot) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    // Ordering matters: a render already in flight must scale nothing rather than use a recycled slot.
    atomic_store(&slot->unit, NULL);
    atomic_store(&slot->bitsPerChannel, 0);
    atomic_store(&slot->bytesPerBuffer, 0);
    pthread_mutex_unlock(&QAVUnitLock);
    AudioUnitRemoveRenderNotify(unit, QAVGainRenderNotify, slot);
    pthread_mutex_lock(&QAVUnitLock);
    slot->claimed = false;
    atomic_fetch_add_explicit(&QAVUnitDetached, 1, memory_order_relaxed);
    pthread_mutex_unlock(&QAVUnitLock);
}

OSStatus QAVStartOutputUnit(AudioUnit unit) {
    QAVAttachUnit(unit);
    return AudioOutputUnitStart(unit);
}

OSStatus QAVStopOutputUnit(AudioUnit unit) {
    QAVDetachUnit(unit);
    return AudioOutputUnitStop(unit);
}

OSStatus QAVDisposeComponentInstance(AudioComponentInstance instance) {
    QAVDetachUnit((AudioUnit)instance);
    return AudioComponentInstanceDispose(instance);
}

void QAVAudioUnitSetGain(float gain, bool enabled) {
    atomic_store_explicit(&QAVUnitGain, QAVNormalizeGain(gain), memory_order_relaxed);
    atomic_store_explicit(&QAVUnitEnabled, enabled, memory_order_relaxed);
}

void QAVAudioUnitRefresh(void) {
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        AudioUnit unit = atomic_load(&slot->unit);
        if (!slot->claimed || !unit || atomic_load(&slot->bitsPerChannel) || slot->reprobes >= QAV_FORMAT_REPROBES) continue;
        slot->reprobes++;
        AudioStreamBasicDescription format;
        if (!QAVCopySpeakerFormat(unit, &format)) continue;
        QAVApplyFormatToSlot(slot, &format);
        atomic_fetch_sub_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&QAVUnitLock);
}

QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void) {
    QAVAudioUnitDiagnostics result;
    uint64_t scaled = 0;
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) scaled += atomic_load(&QAVUnitSlots[index].scaledFrames);
    pthread_mutex_unlock(&QAVUnitLock);
    result.attachedUnits = atomic_load(&QAVUnitAttached);
    result.detachedUnits = atomic_load(&QAVUnitDetached);
    result.scaledFrames = scaled;
    result.skippedFrames = atomic_load(&QAVUnitSkippedFrames);
    result.geometryMismatches = atomic_load(&QAVUnitGeometryMismatches);
    result.formatFailures = atomic_load(&QAVUnitFormatFailures);
    for (size_t index = 0; index < QAV_AUDIO_UNIT_BUS_BUCKETS; index++) result.framesPerBus[index] = atomic_load(&QAVUnitFramesPerBus[index]);
    result.lastError = atomic_load(&QAVUnitLastError);
    result.lastBusNumber = atomic_load(&QAVUnitLastBus);
    result.speakerFormatStatus = atomic_load(&QAVUnitSpeakerFormatStatus);
    result.micFormatStatus = atomic_load(&QAVUnitMicFormatStatus);
    result.speakerBitsPerChannel = atomic_load(&QAVUnitSpeakerBits);
    result.speakerChannels = atomic_load(&QAVUnitSpeakerChannels);
    result.speakerIsFloat = atomic_load(&QAVUnitSpeakerIsFloat);
    result.micBitsPerChannel = atomic_load(&QAVUnitMicBits);
    result.speakerIO = atomic_load(&QAVUnitSpeakerIO);
    result.micIO = atomic_load(&QAVUnitMicIO);
    return result;
}
