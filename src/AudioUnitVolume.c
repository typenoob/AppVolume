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
// Element 1 is the output side of RemoteIO and element 0 is the microphone; only non-zero bus numbers are
// scaled, so recording keeps the app's own levels.

#define QAV_UNIT_SLOTS 32

typedef struct {
    _Atomic(void *) unit;
    _Atomic uint32_t bitsPerChannel;
    _Atomic uintptr_t isFloat;
    _Atomic uint64_t scaledFrames;
    bool claimed;
} QAVUnitSlot;

static pthread_mutex_t QAVUnitLock = PTHREAD_MUTEX_INITIALIZER;
static QAVUnitSlot QAVUnitSlots[QAV_UNIT_SLOTS];
static _Atomic float QAVUnitGain = 1.0f;
static _Atomic bool QAVUnitEnabled = true;
static _Atomic uint64_t QAVUnitAttached;
static _Atomic uint64_t QAVUnitDetached;
static _Atomic uint64_t QAVUnitInputFrames;
static _Atomic uint64_t QAVUnitFormatFailures;
static _Atomic int32_t QAVUnitLastError;
static _Atomic int32_t QAVUnitLastBus;
static _Atomic uint32_t QAVUnitLastBits;
static _Atomic uint32_t QAVUnitLastChannels;
static _Atomic int32_t QAVUnitLastIsFloat;

// The iOS SDK types the notify proc as AURenderCallback, so match that shape exactly.
static OSStatus QAVGainRenderNotify(void *inRefCon, AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
    (void)inTimeStamp;
    if (!(*actionFlags & kAudioUnitRenderAction_PostRender)) return noErr;
    QAVUnitSlot *slot = (QAVUnitSlot *)inRefCon;
    if (!slot || !atomic_load_explicit(&slot->unit, memory_order_acquire)) return noErr;
    atomic_store_explicit(&QAVUnitLastBus, (int32_t)inBusNumber, memory_order_relaxed);
    if (inBusNumber == 0) {
        // Microphone side of the same unit: leave the app's recording levels alone.
        atomic_fetch_add_explicit(&QAVUnitInputFrames, inNumberFrames, memory_order_relaxed);
        return noErr;
    }
    uint32_t bits = atomic_load_explicit(&slot->bitsPerChannel, memory_order_acquire);
    if (!bits || !ioData) return noErr;
    float gain = atomic_load_explicit(&QAVUnitGain, memory_order_relaxed);
    if (!atomic_load_explicit(&QAVUnitEnabled, memory_order_relaxed) || gain == 1.0f) return noErr;
    bool isFloat = atomic_load_explicit(&slot->isFloat, memory_order_relaxed) != 0;
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        QAVApplySampleGain(ioData->mBuffers[buffer].mData, ioData->mBuffers[buffer].mDataByteSize, bits, isFloat, gain);
    }
    atomic_fetch_add_explicit(&slot->scaledFrames, inNumberFrames, memory_order_relaxed);
    return noErr;
}

static bool QAVFormatIsScalable(const AudioStreamBasicDescription *format) {
    if (format->mFormatID != kAudioFormatLinearPCM) return false;
    if (format->mBitsPerChannel != 16 && format->mBitsPerChannel != 32) return false;
    if (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 64) return false;
    if ((format->mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0 && format->mBitsPerChannel != 32) return false;
    uint32_t bytesPerSample = format->mBitsPerChannel / 8;
    bool nonInterleaved = (format->mFormatFlags & kLinearPCMFormatFlagIsNonInterleaved) != 0;
    uint32_t expected = nonInterleaved ? bytesPerSample : bytesPerSample * format->mChannelsPerFrame;
    // Scaling walks a packed run of samples; anything padded or aliased is refused rather than guessed at.
    return format->mBytesPerFrame == expected;
}

// RemoteIO reports the speaker feed as the input scope of element 1 and the microphone as the output scope of element 0.
static bool QAVCopyScalableFormat(AudioUnit unit, AudioStreamBasicDescription *format) {
    static const struct { AudioUnitScope scope; AudioUnitElement element; } probes[] = {
        { kAudioUnitScope_Input, 1 },
        { kAudioUnitScope_Output, 0 },
        { kAudioUnitScope_Output, 1 },
        { kAudioUnitScope_Input, 0 }
    };
    for (size_t index = 0; index < sizeof(probes) / sizeof(probes[0]); index++) {
        UInt32 size = sizeof(*format);
        OSStatus status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, probes[index].scope, probes[index].element, format, &size);
        if (status == noErr && size == sizeof(*format) && QAVFormatIsScalable(format)) return true;
    }
    return false;
}

static QAVUnitSlot *QAVSlotForUnitLocked(AudioUnit unit) {
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (atomic_load(&QAVUnitSlots[index].unit) == unit) return &QAVUnitSlots[index];
    }
    return NULL;
}

static void QAVAttachUnit(AudioUnit unit) {
    if (!unit) return;
    AudioStreamBasicDescription format;
    bool scalable = QAVCopyScalableFormat(unit, &format);
    pthread_mutex_lock(&QAVUnitLock);
    if (QAVSlotForUnitLocked(unit)) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    QAVUnitSlot *slot = NULL;
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (!QAVUnitSlots[index].claimed) { slot = &QAVUnitSlots[index]; break; }
    }
    if (!slot) {
        atomic_store_explicit(&QAVUnitLastError, -1, memory_order_relaxed);
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    if (!scalable) {
        // Unknown or packed format: leave the unit alone rather than guess at the sample layout.
        atomic_fetch_add_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
        atomic_store_explicit(&QAVUnitLastBits, format.mBitsPerChannel, memory_order_relaxed);
        atomic_store_explicit(&QAVUnitLastChannels, format.mChannelsPerFrame, memory_order_relaxed);
        atomic_store_explicit(&QAVUnitLastIsFloat, (format.mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0 ? 1 : 0, memory_order_relaxed);
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    slot->claimed = true;
    atomic_store(&slot->bitsPerChannel, format.mBitsPerChannel);
    atomic_store(&slot->isFloat, (uintptr_t)((format.mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0));
    atomic_store(&slot->scaledFrames, 0);
    OSStatus status = AudioUnitAddRenderNotify(unit, QAVGainRenderNotify, slot);
    if (status != noErr) {
        slot->claimed = false;
        atomic_store(&slot->bitsPerChannel, 0);
        atomic_store_explicit(&QAVUnitLastError, (int32_t)status, memory_order_relaxed);
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    atomic_store(&slot->unit, unit);
    atomic_fetch_add_explicit(&QAVUnitAttached, 1, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastError, 0, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastBits, format.mBitsPerChannel, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastChannels, format.mChannelsPerFrame, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitLastIsFloat, (format.mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0 ? 1 : 0, memory_order_relaxed);
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

QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void) {
    QAVAudioUnitDiagnostics result;
    uint64_t scaled = 0;
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) scaled += atomic_load(&QAVUnitSlots[index].scaledFrames);
    pthread_mutex_unlock(&QAVUnitLock);
    result.attachedUnits = atomic_load(&QAVUnitAttached);
    result.detachedUnits = atomic_load(&QAVUnitDetached);
    result.scaledFrames = scaled;
    result.inputFramesSeen = atomic_load(&QAVUnitInputFrames);
    result.formatFailures = atomic_load(&QAVUnitFormatFailures);
    result.lastError = atomic_load(&QAVUnitLastError);
    result.lastBusNumber = atomic_load(&QAVUnitLastBus);
    result.lastBitsPerChannel = atomic_load(&QAVUnitLastBits);
    result.lastChannels = atomic_load(&QAVUnitLastChannels);
    result.lastIsFloat = atomic_load(&QAVUnitLastIsFloat);
    return result;
}
