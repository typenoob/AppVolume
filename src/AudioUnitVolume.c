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
//
// A stream description read off a property is only one of several candidate layouts for the buffer that arrives:
// an app that feeds int16 often ends up handing the hardware float32, and a format flagged non-interleaved does
// not say how many buffers the render actually filled. Because multiplying every sample is correct both for an
// interleaved run and for one buffer per channel, a render is matched by its total byte count against the speaker
// candidates rather than against one assumed layout. Exactly one candidate has to fit; two that fit would be a
// coin toss about the sample type, and a wrong type is noise, so that case is refused like a mismatch.

#define QAV_SPEAKER_BUS 0
#define QAV_MIC_BUS 1
#define QAV_LAST_BUS_BUCKET (QAV_AUDIO_UNIT_BUS_BUCKETS - 1)
#define QAV_UNIT_SLOTS 32
#define QAV_FORMAT_REPROBES 8

typedef struct {
    uint32_t bytesPerFrame;   // bytes of one frame across every channel present in the render list
    uint32_t bitsPerChannel;
    bool isFloat;
} QAVFormatCandidate;
typedef struct {
    _Atomic(void *) unit;
    _Atomic uint32_t candidateBytesPerFrame[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uint32_t candidateBits[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uintptr_t candidateIsFloat[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uint32_t candidateCount;
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
static _Atomic uint64_t QAVUnitFramesPerCandidate[QAV_AUDIO_UNIT_CANDIDATES];
static _Atomic uint32_t QAVUnitRenderFrames;
static _Atomic uint32_t QAVUnitRenderBytes;
static _Atomic uint32_t QAVUnitRenderBuffers;
static _Atomic int32_t QAVUnitLastError;
static _Atomic int32_t QAVUnitLastBus;
static _Atomic int32_t QAVUnitSpeakerFormatStatus;
static _Atomic int32_t QAVUnitMicFormatStatus;
static _Atomic uint32_t QAVUnitSpeakerChannels;
static _Atomic uint32_t QAVUnitSpeakerBytesPerFrame;
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
    if (!atomic_load_explicit(&slot->candidateCount, memory_order_acquire) || !ioData) return noErr;
    if (inBusNumber != QAV_SPEAKER_BUS) {
        // Any other bus is a path the app owns the level of, such as the microphone on element 1.
        atomic_fetch_add_explicit(&QAVUnitSkippedFrames, inNumberFrames, memory_order_relaxed);
        return noErr;
    }
    float gain = atomic_load_explicit(&QAVUnitGain, memory_order_relaxed);
    if (!atomic_load_explicit(&QAVUnitEnabled, memory_order_relaxed) || gain == 1.0f) return noErr;
    if (*actionFlags & kAudioUnitRenderAction_PostRenderError) return noErr;

    UInt32 total = 0, filled = 0;
    bool broken = false;
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        AudioBuffer *entry = &ioData->mBuffers[buffer];
        if (!entry->mDataByteSize) continue;
        if (!entry->mData) { broken = true; break; }
        total += entry->mDataByteSize;
        filled++;
    }
    atomic_store_explicit(&QAVUnitRenderFrames, inNumberFrames, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitRenderBytes, total, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitRenderBuffers, filled, memory_order_relaxed);
    if (broken || !filled) {
        atomic_fetch_add_explicit(&QAVUnitGeometryMismatches, 1, memory_order_relaxed);
        return noErr;
    }
    uint32_t candidates = atomic_load_explicit(&slot->candidateCount, memory_order_relaxed);
    int matched = -1;
    uint32_t matchCount = 0;
    for (uint32_t index = 0; index < candidates; index++) {
        uint32_t bytesPerFrame = atomic_load_explicit(&slot->candidateBytesPerFrame[index], memory_order_relaxed);
        if ((uint64_t)bytesPerFrame * inNumberFrames != total) continue;
        uint32_t bytesPerSample = atomic_load_explicit(&slot->candidateBits[index], memory_order_relaxed) / 8;
        // One buffer means the run is taken as interleaved; several buffers are only credible when each holds
        // exactly one channel's worth of this candidate's sample size. That rejects a mono float render that
        // happens to be as long as a declared stereo int16 frame.
        bool credible = true;
        for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers && credible; buffer++) {
            UInt32 size = ioData->mBuffers[buffer].mDataByteSize;
            if (!size) continue;
            if (size % bytesPerSample) credible = false;
            if (filled > 1 && size != (UInt32)((uint64_t)bytesPerSample * inNumberFrames)) credible = false;
        }
        if (!credible) continue;
        matched = (int)index;
        matchCount++;
    }
    // Exactly one candidate has to fit: one is a fact, two would be a coin toss about the sample type, and none is
    // a guess. Reading a sample stream as the wrong type turns audio into noise, so both other cases refuse.
    if (matchCount != 1) {
        atomic_fetch_add_explicit(&QAVUnitGeometryMismatches, 1, memory_order_relaxed);
        return noErr;
    }
    uint32_t bits = atomic_load_explicit(&slot->candidateBits[matched], memory_order_relaxed);
    bool isFloat = atomic_load_explicit(&slot->candidateIsFloat[matched], memory_order_relaxed) != 0;
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        AudioBuffer *entry = &ioData->mBuffers[buffer];
        QAVApplySampleGain(entry->mData, entry->mDataByteSize, bits, isFloat, gain);
    }
    atomic_fetch_add_explicit(&slot->scaledFrames, inNumberFrames, memory_order_relaxed);
    atomic_fetch_add_explicit(&QAVUnitFramesPerCandidate[matched], inNumberFrames, memory_order_relaxed);
    return noErr;
}

// Accepts only a packed layout: the format's per-frame size must be exactly channels x sample size, so nothing
// padded or aliased is ever walked as if it were a run of samples. The candidate itself counts every channel,
// because a render list holds all of them whether they are interleaved or one per buffer.
static bool QAVCandidateFromFormat(const AudioStreamBasicDescription *format, QAVFormatCandidate *out) {
    if (format->mFormatID != kAudioFormatLinearPCM) return false;
    if (format->mBitsPerChannel != 16 && format->mBitsPerChannel != 32) return false;
    if (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 64) return false;
    bool isFloat = (format->mFormatFlags & kLinearPCMFormatFlagIsFloat) != 0;
    if (isFloat && format->mBitsPerChannel != 32) return false;
    uint32_t bytesPerSample = format->mBitsPerChannel / 8;
    bool nonInterleaved = (format->mFormatFlags & kLinearPCMFormatFlagIsNonInterleaved) != 0;
    uint32_t packed = nonInterleaved ? bytesPerSample : bytesPerSample * format->mChannelsPerFrame;
    if (format->mBytesPerFrame != packed) return false;
    *out = (QAVFormatCandidate){
        .bytesPerFrame = bytesPerSample * format->mChannelsPerFrame,
        .bitsPerChannel = format->mBitsPerChannel,
        .isFloat = isFloat
    };
    return true;
}

#define QAV_FORMAT_TOO_SHORT -1

static OSStatus QAVProbeFormat(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element, AudioStreamBasicDescription *format) {
    UInt32 size = sizeof(*format);
    OSStatus status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, format, &size);
    if (status == noErr && size != sizeof(*format)) status = QAV_FORMAT_TOO_SHORT;
    return status;
}

// The speaker is element 0, described both as what the app feeds in (input scope) and as what the hardware sees
// (output scope). Collecting the two turns one assumed layout into a small set of candidates, and the render then
// picks whichever one its byte count fits; identical descriptions collapse into one.
static uint32_t QAVResolveCandidates(AudioUnit unit, QAVFormatCandidate *out, uint32_t capacity) {
    static const AudioUnitScope scopes[] = { kAudioUnitScope_Input, kAudioUnitScope_Output };
    AudioStreamBasicDescription feed;
    OSStatus feedStatus = QAVProbeFormat(unit, kAudioUnitScope_Input, QAV_SPEAKER_BUS, &feed);
    atomic_store_explicit(&QAVUnitSpeakerFormatStatus, (int32_t)feedStatus, memory_order_relaxed);
    if (feedStatus == noErr) {
        atomic_store_explicit(&QAVUnitSpeakerChannels, feed.mChannelsPerFrame, memory_order_relaxed);
        atomic_store_explicit(&QAVUnitSpeakerBytesPerFrame, feed.mBytesPerFrame, memory_order_relaxed);
    }
    AudioStreamBasicDescription mic;
    OSStatus micStatus = QAVProbeFormat(unit, kAudioUnitScope_Output, QAV_MIC_BUS, &mic);
    atomic_store_explicit(&QAVUnitMicFormatStatus, (int32_t)micStatus, memory_order_relaxed);
    if (micStatus == noErr) atomic_store_explicit(&QAVUnitMicBits, mic.mBitsPerChannel, memory_order_relaxed);

    uint32_t count = 0;
    for (size_t index = 0; index < sizeof(scopes) / sizeof(scopes[0]) && count < capacity; index++) {
        AudioStreamBasicDescription format;
        if (QAVProbeFormat(unit, scopes[index], QAV_SPEAKER_BUS, &format) != noErr) continue;
        QAVFormatCandidate candidate;
        if (!QAVCandidateFromFormat(&format, &candidate)) continue;
        bool known = false;
        for (uint32_t previous = 0; previous < count; previous++) {
            if (out[previous].bytesPerFrame == candidate.bytesPerFrame && out[previous].bitsPerChannel == candidate.bitsPerChannel &&
                out[previous].isFloat == candidate.isFloat) known = true;
        }
        if (!known) out[count++] = candidate;
    }
    return count;
}

static int32_t QAVCopyIOFlag(AudioUnit unit, AudioUnitScope scope, AudioUnitElement element) {
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    OSStatus status = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_EnableIO, scope, element, &value, &size);
    if (status != noErr || size != sizeof(value)) return (int32_t)status;
    return (int32_t)value;
}

// The candidate arrays are written before the count, which the render thread acquires before reading them.
static void QAVPublishCandidates(QAVUnitSlot *slot, const QAVFormatCandidate *candidates, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        atomic_store(&slot->candidateBytesPerFrame[index], candidates[index].bytesPerFrame);
        atomic_store(&slot->candidateBits[index], candidates[index].bitsPerChannel);
        atomic_store(&slot->candidateIsFloat[index], (uintptr_t)(candidates[index].isFloat ? 1 : 0));
    }
    atomic_store_explicit(&slot->candidateCount, count, memory_order_release);
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
        atomic_store(&slot->candidateCount, 0);
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
    QAVFormatCandidate candidates[QAV_AUDIO_UNIT_CANDIDATES];
    uint32_t count = QAVResolveCandidates(unit, candidates, QAV_AUDIO_UNIT_CANDIDATES);
    atomic_store_explicit(&QAVUnitSpeakerIO, QAVCopyIOFlag(unit, kAudioUnitScope_Output, QAV_SPEAKER_BUS), memory_order_relaxed);
    atomic_store_explicit(&QAVUnitMicIO, QAVCopyIOFlag(unit, kAudioUnitScope_Input, QAV_MIC_BUS), memory_order_relaxed);
    if (!count) atomic_fetch_add_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);

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
    QAVPublishCandidates(slot, candidates, count);
    OSStatus status = AudioUnitAddRenderNotify(unit, QAVGainRenderNotify, slot);
    if (status != noErr) {
        slot->claimed = false;
        atomic_store(&slot->candidateCount, 0);
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
    atomic_store(&slot->candidateCount, 0);
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

// An app that sets its stream format after AudioOutputUnitStart leaves the slot with no candidate at attach time.
// Retrying off the realtime thread is what lets such a unit start being scaled without restarting the app.
void QAVAudioUnitRefresh(void) {
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        AudioUnit unit = atomic_load(&slot->unit);
        if (!slot->claimed || !unit || atomic_load(&slot->candidateCount) || slot->reprobes >= QAV_FORMAT_REPROBES) continue;
        slot->reprobes++;
        QAVFormatCandidate candidates[QAV_AUDIO_UNIT_CANDIDATES];
        uint32_t count = QAVResolveCandidates(unit, candidates, QAV_AUDIO_UNIT_CANDIDATES);
        if (!count) continue;
        QAVPublishCandidates(slot, candidates, count);
        atomic_fetch_sub_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&QAVUnitLock);
}

QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void) {
    QAVAudioUnitDiagnostics result;
    memset(&result, 0, sizeof(result));
    uint64_t scaled = 0;
    QAVUnitSlot *display = NULL;
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        scaled += atomic_load(&slot->scaledFrames);
        if (!slot->claimed || !atomic_load(&slot->unit)) continue;
        uint32_t count = atomic_load(&slot->candidateCount);
        if (!count) continue;
        if (!display || count > atomic_load(&display->candidateCount)) display = slot;
    }
    if (display) {
        result.candidateCount = (uint32_t)atomic_load(&display->candidateCount);
        for (uint32_t index = 0; index < result.candidateCount; index++) {
            result.candidateBytesPerFrame[index] = atomic_load(&display->candidateBytesPerFrame[index]);
            result.candidateBits[index] = atomic_load(&display->candidateBits[index]);
            result.candidateIsFloat[index] = atomic_load(&display->candidateIsFloat[index]) != 0;
        }
    }
    pthread_mutex_unlock(&QAVUnitLock);
    result.attachedUnits = atomic_load(&QAVUnitAttached);
    result.detachedUnits = atomic_load(&QAVUnitDetached);
    result.scaledFrames = scaled;
    result.skippedFrames = atomic_load(&QAVUnitSkippedFrames);
    result.geometryMismatches = atomic_load(&QAVUnitGeometryMismatches);
    result.formatFailures = atomic_load(&QAVUnitFormatFailures);
    for (size_t index = 0; index < QAV_AUDIO_UNIT_BUS_BUCKETS; index++) result.framesPerBus[index] = atomic_load(&QAVUnitFramesPerBus[index]);
    for (size_t index = 0; index < QAV_AUDIO_UNIT_CANDIDATES; index++) result.framesPerCandidate[index] = atomic_load(&QAVUnitFramesPerCandidate[index]);
    result.lastError = atomic_load(&QAVUnitLastError);
    result.lastBusNumber = atomic_load(&QAVUnitLastBus);
    result.speakerFormatStatus = atomic_load(&QAVUnitSpeakerFormatStatus);
    result.micFormatStatus = atomic_load(&QAVUnitMicFormatStatus);
    result.speakerChannels = atomic_load(&QAVUnitSpeakerChannels);
    result.speakerBytesPerFrame = atomic_load(&QAVUnitSpeakerBytesPerFrame);
    result.micBitsPerChannel = atomic_load(&QAVUnitMicBits);
    result.speakerIO = atomic_load(&QAVUnitSpeakerIO);
    result.micIO = atomic_load(&QAVUnitMicIO);
    result.renderFrames = atomic_load(&QAVUnitRenderFrames);
    result.renderBytes = atomic_load(&QAVUnitRenderBytes);
    result.renderBuffers = atomic_load(&QAVUnitRenderBuffers);
    return result;
}
