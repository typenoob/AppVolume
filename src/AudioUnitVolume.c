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

typedef struct {
    uint32_t bytesPerFrame;   // bytes of one frame across every channel present in the render list
    uint32_t bitsPerChannel;
    bool isFloat;
} QAVFormatCandidate;
// Everything the panel reads about a unit's format lives in that unit's slot: a global would describe whichever
// probe ran last, and readings taken from two different units can look contradictory in exactly the way that
// sends a fix in the wrong direction.
typedef struct {
    _Atomic(void *) unit;
    _Atomic uint32_t candidateBytesPerFrame[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uint32_t candidateBits[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uintptr_t candidateIsFloat[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uint64_t candidateFrames[QAV_AUDIO_UNIT_CANDIDATES];
    _Atomic uint32_t candidateCount;
    _Atomic uint64_t scaledFrames;
    _Atomic int32_t feedStatus;
    _Atomic int32_t hardwareStatus;
    _Atomic int32_t micStatus;
    _Atomic uint32_t micBits;
    _Atomic uint32_t speakerChannels;
    _Atomic uint32_t speakerBytesPerFrame;
    // The description this slot was refused, kept verbatim: fourcc plus layout numbers is what tells a 24-bit
    // packed stream from a padded one from a plain 「format not set yet」 probe error.
    _Atomic uint32_t rejectedFormatID;
    _Atomic uint32_t rejectedFlags;
    _Atomic uint32_t rejectedBits;
    _Atomic uint32_t rejectedChannels;
    _Atomic uint32_t rejectedBytesPerFrame;
    _Atomic float rejectedSampleRate;
    _Atomic uint32_t renderFrames;
    _Atomic uint32_t renderBytes;
    _Atomic uint32_t renderBuffers;
    _Atomic uint64_t speakerRenders;
    _Atomic int32_t speakerIO;
    _Atomic int32_t micIO;
    // The component this slot was claimed for, when it came from AudioComponentInstanceNew: two live output units
    // in one app (playback and capture) otherwise read as the same anonymous row.
    _Atomic uint32_t componentType;
    _Atomic uint32_t componentSubType;
    bool claimed;
    uint64_t claimId;      // changes with every claim, so a re-probe in flight cannot publish onto a recycled slot
    bool counted;          // whether this slot's unreadable format is inside the 不认识格式 gauge
    uint32_t reprobes;
} QAVUnitSlot;

static pthread_mutex_t QAVUnitLock = PTHREAD_MUTEX_INITIALIZER;
// AudioToolbox reads never happen while QAVUnitLock is held: this mutex serialises re-probes on its own, so our
// mutex can never be held at the same time as AudioToolbox's internal one and the two cannot deadlock.
static pthread_mutex_t QAVFormatLock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t QAVNextClaimId;
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
static _Atomic uint32_t QAVUnitFormatEvents;
static _Atomic uint32_t QAVUnitCreatedInstances;
static _Atomic uint32_t QAVUnitCreatedType;
static _Atomic uint32_t QAVUnitCreatedSubType;

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
    if (inBusNumber != QAV_SPEAKER_BUS) {
        // Any other bus is a path the app owns the level of, such as the microphone on element 1.
        if (atomic_load_explicit(&slot->candidateCount, memory_order_acquire))
            atomic_fetch_add_explicit(&QAVUnitSkippedFrames, inNumberFrames, memory_order_relaxed);
        return noErr;
    }
    // The speaker geometry is measured even for a slot with no format and even while the gain is idle: 「this unit
    // never renders bus 0」 and 「it renders bus 0 in a layout we refused」 are different answers, and only the
    // render thread can tell them apart. Walking the buffer list touches pointers, not samples.
    if (!ioData) return noErr;
    UInt32 total = 0, filled = 0;
    bool broken = false;
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; buffer++) {
        AudioBuffer *entry = &ioData->mBuffers[buffer];
        if (!entry->mDataByteSize) continue;
        if (!entry->mData) { broken = true; break; }
        total += entry->mDataByteSize;
        filled++;
    }
    atomic_store_explicit(&slot->renderFrames, inNumberFrames, memory_order_relaxed);
    atomic_store_explicit(&slot->renderBytes, total, memory_order_relaxed);
    atomic_store_explicit(&slot->renderBuffers, filled, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->speakerRenders, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&slot->candidateCount, memory_order_acquire)) return noErr;
    if (broken || !filled) {
        atomic_fetch_add_explicit(&QAVUnitGeometryMismatches, 1, memory_order_relaxed);
        return noErr;
    }
    float gain = atomic_load_explicit(&QAVUnitGain, memory_order_relaxed);
    if (!atomic_load_explicit(&QAVUnitEnabled, memory_order_relaxed) || gain == 1.0f) return noErr;
    if (*actionFlags & kAudioUnitRenderAction_PostRenderError) return noErr;

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
    atomic_fetch_add_explicit(&slot->candidateFrames[matched], inNumberFrames, memory_order_relaxed);
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
// picks whichever one its byte count fits; identical descriptions collapse into one. What was probed and what was
// refused is written into the slot, so the panel tells one unit's story instead of a blend of whichever units
// happened to be probed last.
static uint32_t QAVResolveCandidates(AudioUnit unit, QAVUnitSlot *slot, QAVFormatCandidate *out, uint32_t capacity) {
    AudioStreamBasicDescription feed, hardware, mic;
    OSStatus feedStatus = QAVProbeFormat(unit, kAudioUnitScope_Input, QAV_SPEAKER_BUS, &feed);
    OSStatus hardwareStatus = QAVProbeFormat(unit, kAudioUnitScope_Output, QAV_SPEAKER_BUS, &hardware);
    OSStatus micStatus = QAVProbeFormat(unit, kAudioUnitScope_Output, QAV_MIC_BUS, &mic);
    atomic_store_explicit(&slot->feedStatus, (int32_t)feedStatus, memory_order_relaxed);
    atomic_store_explicit(&slot->hardwareStatus, (int32_t)hardwareStatus, memory_order_relaxed);
    atomic_store_explicit(&slot->micStatus, (int32_t)micStatus, memory_order_relaxed);
    atomic_store_explicit(&slot->micBits, micStatus == noErr ? mic.mBitsPerChannel : 0, memory_order_relaxed);
    atomic_store_explicit(&slot->speakerChannels, feedStatus == noErr ? feed.mChannelsPerFrame : 0, memory_order_relaxed);
    atomic_store_explicit(&slot->speakerBytesPerFrame, feedStatus == noErr ? feed.mBytesPerFrame : 0, memory_order_relaxed);
    atomic_store_explicit(&slot->rejectedFormatID, 0, memory_order_relaxed);

    const AudioStreamBasicDescription *descriptions[] = {
        feedStatus == noErr ? &feed : NULL,
        hardwareStatus == noErr ? &hardware : NULL
    };
    uint32_t count = 0;
    for (size_t index = 0; index < sizeof(descriptions) / sizeof(descriptions[0]) && count < capacity; index++) {
        if (!descriptions[index]) continue;
        QAVFormatCandidate candidate;
        if (QAVCandidateFromFormat(descriptions[index], &candidate)) {
            bool known = false;
            for (uint32_t previous = 0; previous < count; previous++) {
                if (out[previous].bytesPerFrame == candidate.bytesPerFrame && out[previous].bitsPerChannel == candidate.bitsPerChannel &&
                    out[previous].isFloat == candidate.isFloat) known = true;
            }
            if (!known) out[count++] = candidate;
            continue;
        }
        const AudioStreamBasicDescription *refused = descriptions[index];
        atomic_store_explicit(&slot->rejectedFormatID, (uint32_t)refused->mFormatID, memory_order_relaxed);
        atomic_store_explicit(&slot->rejectedFlags, refused->mFormatFlags, memory_order_relaxed);
        atomic_store_explicit(&slot->rejectedBits, refused->mBitsPerChannel, memory_order_relaxed);
        atomic_store_explicit(&slot->rejectedChannels, refused->mChannelsPerFrame, memory_order_relaxed);
        atomic_store_explicit(&slot->rejectedBytesPerFrame, refused->mBytesPerFrame, memory_order_relaxed);
        atomic_store_explicit(&slot->rejectedSampleRate, (float)refused->mSampleRate, memory_order_relaxed);
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

// 不认识格式 is a gauge of the units that have no usable format right now, so it rises when a probe comes up empty
// and falls when a later probe describes one. slot->counted keeps the two transitions from double counting.
static void QAVTrackFormatGauge(QAVUnitSlot *slot, uint32_t count) {
    if (!count && !slot->counted) {
        slot->counted = true;
        atomic_fetch_add_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
    }
    if (count && slot->counted) {
        slot->counted = false;
        atomic_fetch_sub_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
    }
}

static QAVUnitSlot *QAVSlotForUnitLocked(AudioUnit unit) {
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (atomic_load(&QAVUnitSlots[index].unit) == unit) return &QAVUnitSlots[index];
    }
    return NULL;
}

static QAVUnitSlot *QAVClaimSlotLocked(uint32_t componentType, uint32_t componentSubType) {
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        if (QAVUnitSlots[index].claimed) continue;
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        slot->claimed = true;
        slot->counted = false;
        slot->reprobes = 0;
        slot->claimId = atomic_fetch_add_explicit(&QAVNextClaimId, 1, memory_order_relaxed) + 1;
        atomic_store(&slot->scaledFrames, 0);
        atomic_store(&slot->candidateCount, 0);
        atomic_store(&slot->speakerRenders, 0);
        atomic_store(&slot->renderFrames, 0);
        atomic_store(&slot->renderBytes, 0);
        atomic_store(&slot->renderBuffers, 0);
        for (uint32_t candidate = 0; candidate < QAV_AUDIO_UNIT_CANDIDATES; candidate++) atomic_store(&slot->candidateFrames[candidate], 0);
        atomic_store(&slot->componentType, componentType);
        atomic_store(&slot->componentSubType, componentSubType);
        atomic_store(&slot->unit, NULL);
        return slot;
    }
    atomic_store_explicit(&QAVUnitLastError, -1, memory_order_relaxed);
    return NULL;
}

static void QAVReleaseSlotLocked(QAVUnitSlot *slot) {
    if (slot->counted) {
        slot->counted = false;
        atomic_fetch_sub_explicit(&QAVUnitFormatFailures, 1, memory_order_relaxed);
    }
    atomic_store(&slot->candidateCount, 0);
    slot->claimed = false;
}

// Probing runs with no lock held, and its results only land on the slot this claim still describes: a unit stopped
// and re-claimed while a probe was in flight gets its own reading rather than a stale one.
static void QAVProbeIntoSlot(AudioUnit unit, QAVUnitSlot *slot, uint64_t claim) {
    QAVFormatCandidate candidates[QAV_AUDIO_UNIT_CANDIDATES];
    pthread_mutex_lock(&QAVFormatLock);
    uint32_t count = QAVResolveCandidates(unit, slot, candidates, QAV_AUDIO_UNIT_CANDIDATES);
    int32_t speakerIO = QAVCopyIOFlag(unit, kAudioUnitScope_Output, QAV_SPEAKER_BUS);
    int32_t micIO = QAVCopyIOFlag(unit, kAudioUnitScope_Input, QAV_MIC_BUS);
    pthread_mutex_unlock(&QAVFormatLock);

    pthread_mutex_lock(&QAVUnitLock);
    if (slot->claimed && slot->claimId == claim) {
        atomic_store(&slot->speakerIO, speakerIO);
        atomic_store(&slot->micIO, micIO);
        QAVTrackFormatGauge(slot, count);
        QAVPublishCandidates(slot, candidates, count);
    }
    pthread_mutex_unlock(&QAVUnitLock);
}

// A unit whose format is unusable still gets a notify attached: the per-bus frame counts are what shows whether the
// app really renders its playback on the speaker bus, and a later probe can still fill in the format.
static void QAVAttachUnit(AudioUnit unit, uint32_t componentType, uint32_t componentSubType) {
    if (!unit) return;
    pthread_mutex_lock(&QAVUnitLock);
    if (QAVSlotForUnitLocked(unit)) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    QAVUnitSlot *slot = QAVClaimSlotLocked(componentType, componentSubType);
    uint64_t claim = slot ? slot->claimId : 0;
    pthread_mutex_unlock(&QAVUnitLock);
    if (!slot) return;
    QAVProbeIntoSlot(unit, slot, claim);

    pthread_mutex_lock(&QAVUnitLock);
    if (!slot->claimed || slot->claimId != claim) {
        pthread_mutex_unlock(&QAVUnitLock);
        return;
    }
    OSStatus status = AudioUnitAddRenderNotify(unit, QAVGainRenderNotify, slot);
    if (status != noErr) {
        QAVReleaseSlotLocked(slot);
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
    QAVReleaseSlotLocked(slot);
    atomic_fetch_add_explicit(&QAVUnitDetached, 1, memory_order_relaxed);
    pthread_mutex_unlock(&QAVUnitLock);
}

OSStatus QAVStartOutputUnit(AudioUnit unit) {
    QAVAttachUnit(unit, 0, 0);
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

// An RTC-style unit is commonly started before it is configured, so a bounded poll is only a guess about timing.
// Re-probing when the app itself describes the stream makes the wait event-driven: the notify stops being inert the
// moment a usable format exists, whatever the moment turns out to be.
static void QAVRefreshUnit(AudioUnit unit) {
    if (!unit) return;
    pthread_mutex_lock(&QAVUnitLock);
    QAVUnitSlot *slot = QAVSlotForUnitLocked(unit);
    uint64_t claim = slot ? slot->claimId : 0;
    pthread_mutex_unlock(&QAVUnitLock);
    if (!slot) return;
    QAVProbeIntoSlot(unit, slot, claim);
    pthread_mutex_lock(&QAVUnitLock);
    if (slot->claimed && slot->claimId == claim) slot->reprobes = 0;
    pthread_mutex_unlock(&QAVUnitLock);
}

OSStatus QAVSetUnitProperty(AudioUnit unit, AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                            const void *inData, UInt32 inDataSize) {
    OSStatus status = AudioUnitSetProperty(unit, inID, inScope, inElement, inData, inDataSize);
    // EnableIO is refreshed too: a unit that starts output-only and enables input later changes what its
    // speaker-side format resolves to, and the panel has to show the state as of now rather than as of start.
    if (status == noErr && (inID == kAudioUnitProperty_StreamFormat || inID == kAudioOutputUnitProperty_EnableIO)) {
        atomic_fetch_add_explicit(&QAVUnitFormatEvents, 1, memory_order_relaxed);
        QAVRefreshUnit(unit);
    }
    return status;
}

OSStatus QAVInitializeUnit(AudioUnit unit) {
    OSStatus status = AudioUnitInitialize(unit);
    if (status == noErr) QAVRefreshUnit(unit);
    return status;
}

// Tracking an output unit from the moment it is created, rather than only from its start call, is what covers the
// units whose start never reaches a bound symbol: a live room creates a RemoteIO inside the bundle and drives it
// through a path our rebind does not see, so waiting for the start hook leaves the render unscaled forever. Only
// kAudioUnitType_Output claims a slot, because the 32-slot pool must not fill with decoders and converters.
OSStatus QAVNewComponentInstance(AudioComponent inComponent, AudioComponentInstance *outInstance) {
    OSStatus status = AudioComponentInstanceNew(inComponent, outInstance);
    if (status != noErr || !outInstance) return status;
    atomic_fetch_add_explicit(&QAVUnitCreatedInstances, 1, memory_order_relaxed);
    AudioComponentDescription description;
    if (AudioComponentGetDescription(inComponent, &description) != noErr) return status;
    atomic_store_explicit(&QAVUnitCreatedType, (uint32_t)description.componentType, memory_order_relaxed);
    atomic_store_explicit(&QAVUnitCreatedSubType, (uint32_t)description.componentSubType, memory_order_relaxed);
    if (description.componentType == kAudioUnitType_Output)
        QAVAttachUnit((AudioUnit)*outInstance, (uint32_t)description.componentType, (uint32_t)description.componentSubType);
    return status;
}

void QAVAudioUnitSetGain(float gain, bool enabled) {
    atomic_store_explicit(&QAVUnitGain, QAVNormalizeGain(gain), memory_order_relaxed);
    atomic_store_explicit(&QAVUnitEnabled, enabled, memory_order_relaxed);
}

// An app that sets its stream format after AudioOutputUnitStart leaves the slot with no candidate at attach time.
// Retrying off the realtime thread is what lets such a unit start being scaled without restarting the app.
void QAVAudioUnitRefresh(void) {
    typedef struct { QAVUnitSlot *slot; AudioUnit unit; uint64_t claim; } QAVPendingProbe;
    QAVPendingProbe pending[QAV_UNIT_SLOTS];
    size_t queued = 0;
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        AudioUnit unit = atomic_load(&slot->unit);
        if (!slot->claimed || !unit || atomic_load(&slot->candidateCount) || slot->reprobes >= QAV_AUDIO_UNIT_REPROBES) continue;
        slot->reprobes++;
        pending[queued++] = (QAVPendingProbe){ .slot = slot, .unit = unit, .claim = slot->claimId };
    }
    pthread_mutex_unlock(&QAVUnitLock);
    for (size_t index = 0; index < queued; index++) QAVProbeIntoSlot(pending[index].unit, pending[index].slot, pending[index].claim);
}

static void QAVFillRow(QAVAudioUnitRow *row, QAVUnitSlot *slot) {
    row->componentType = (uint32_t)atomic_load_explicit(&slot->componentType, memory_order_relaxed);
    row->componentSubType = (uint32_t)atomic_load_explicit(&slot->componentSubType, memory_order_relaxed);
    row->speakerIO = atomic_load_explicit(&slot->speakerIO, memory_order_relaxed);
    row->micIO = atomic_load_explicit(&slot->micIO, memory_order_relaxed);
    row->feedStatus = atomic_load_explicit(&slot->feedStatus, memory_order_relaxed);
    row->hardwareStatus = atomic_load_explicit(&slot->hardwareStatus, memory_order_relaxed);
    row->micFormatStatus = atomic_load_explicit(&slot->micStatus, memory_order_relaxed);
    row->micBitsPerChannel = atomic_load_explicit(&slot->micBits, memory_order_relaxed);
    row->speakerChannels = atomic_load_explicit(&slot->speakerChannels, memory_order_relaxed);
    row->speakerBytesPerFrame = atomic_load_explicit(&slot->speakerBytesPerFrame, memory_order_relaxed);
    row->candidateCount = atomic_load_explicit(&slot->candidateCount, memory_order_relaxed);
    for (uint32_t index = 0; index < row->candidateCount && index < QAV_AUDIO_UNIT_CANDIDATES; index++) {
        row->candidateBytesPerFrame[index] = atomic_load_explicit(&slot->candidateBytesPerFrame[index], memory_order_relaxed);
        row->candidateBits[index] = atomic_load_explicit(&slot->candidateBits[index], memory_order_relaxed);
        row->candidateIsFloat[index] = atomic_load_explicit(&slot->candidateIsFloat[index], memory_order_relaxed) != 0;
        row->framesPerCandidate[index] = atomic_load_explicit(&slot->candidateFrames[index], memory_order_relaxed);
    }
    row->speakerRenders = atomic_load_explicit(&slot->speakerRenders, memory_order_relaxed);
    row->scaledFrames = atomic_load_explicit(&slot->scaledFrames, memory_order_relaxed);
    row->renderFrames = atomic_load_explicit(&slot->renderFrames, memory_order_relaxed);
    row->renderBytes = atomic_load_explicit(&slot->renderBytes, memory_order_relaxed);
    row->renderBuffers = atomic_load_explicit(&slot->renderBuffers, memory_order_relaxed);
    row->reprobes = slot->reprobes;
    row->rejectedFormatID = atomic_load_explicit(&slot->rejectedFormatID, memory_order_relaxed);
    row->rejectedFlags = atomic_load_explicit(&slot->rejectedFlags, memory_order_relaxed);
    row->rejectedBits = atomic_load_explicit(&slot->rejectedBits, memory_order_relaxed);
    row->rejectedChannels = atomic_load_explicit(&slot->rejectedChannels, memory_order_relaxed);
    row->rejectedBytesPerFrame = atomic_load_explicit(&slot->rejectedBytesPerFrame, memory_order_relaxed);
    row->rejectedSampleRate = atomic_load_explicit(&slot->rejectedSampleRate, memory_order_relaxed);
}

// One app can hold several output units at once — Douyin's live room has a speaker half and a microphone half, and
// their readings mean opposite things — so every live unit gets its own row instead of one blended summary. The
// units that matter lead: a resolved format first, then whatever actually renders the speaker bus.
QAVAudioUnitDiagnostics QAVAudioUnitGetDiagnostics(void) {
    QAVAudioUnitDiagnostics result;
    memset(&result, 0, sizeof(result));
    uint64_t scaled = 0;
    uint32_t liveSlots = 0;
    QAVUnitSlot *ranked[QAV_UNIT_SLOTS];
    uint64_t scores[QAV_UNIT_SLOTS];
    size_t count = 0;
    pthread_mutex_lock(&QAVUnitLock);
    for (size_t index = 0; index < QAV_UNIT_SLOTS; index++) {
        QAVUnitSlot *slot = &QAVUnitSlots[index];
        scaled += atomic_load(&slot->scaledFrames);
        if (!slot->claimed || !atomic_load(&slot->unit)) continue;
        liveSlots++;
        uint64_t renders = atomic_load(&slot->speakerRenders);
        uint64_t resolved = atomic_load(&slot->candidateCount) ? (1ull << 63) : 0;
        ranked[count] = slot;
        scores[count] = resolved | (renders & ~(1ull << 63));
        count++;
    }
    for (size_t index = 1; index < count; index++) {
        QAVUnitSlot *moving = ranked[index];
        uint64_t score = scores[index];
        size_t target = index;
        while (target && scores[target - 1] < score) {
            ranked[target] = ranked[target - 1];
            scores[target] = scores[target - 1];
            target--;
        }
        ranked[target] = moving;
        scores[target] = score;
    }
    result.liveSlots = liveSlots;
    for (size_t index = 0; index < count && index < QAV_AUDIO_UNIT_ROWS; index++) {
        result.rowCount++;
        QAVFillRow(&result.rows[index], ranked[index]);
    }
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
    result.formatEvents = atomic_load(&QAVUnitFormatEvents);
    result.createdInstances = atomic_load(&QAVUnitCreatedInstances);
    result.createdType = atomic_load(&QAVUnitCreatedType);
    result.createdSubType = atomic_load(&QAVUnitCreatedSubType);
    return result;
}
