#include "AudioQueueVolume.h"
#include "AudioUnitVolume.h"
#include "VolumePolicy.h"
#include <AudioToolbox/AudioQueue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef QAV_AUDIOQUEUE_TEST
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include "../vendor/fishhook/fishhook.h"
#endif

typedef struct QAVQueue {
    AudioQueueRef queue;
    float requestedVolume;
    uint64_t revision;
    uint64_t appliedGeneration;
    unsigned references;
    struct QAVQueue *next;
} QAVQueue;

static pthread_mutex_t QAVQueueLock = PTHREAD_MUTEX_INITIALIZER;
static QAVQueue *QAVQueues;
static float QAVQueueGain = 1.0f;
static bool QAVQueueEnabled = true;
static uint64_t QAVQueueGeneration = 1;
static QAVAudioQueueDiagnostics QAVQueueDiagnostics;

static QAVQueue *QAVAcquireQueue(AudioQueueRef queue) {
    pthread_mutex_lock(&QAVQueueLock);
    QAVQueue *state = QAVQueues;
    while (state && state->queue != queue) state = state->next;
    if (state) state->references++;
    pthread_mutex_unlock(&QAVQueueLock);
    return state;
}

static void QAVReleaseQueue(QAVQueue *state) {
    if (!state) return;
    pthread_mutex_lock(&QAVQueueLock);
    bool destroy = --state->references == 0;
    pthread_mutex_unlock(&QAVQueueLock);
    if (destroy) free(state);
}

static void QAVRecordQueueError(OSStatus status) {
    pthread_mutex_lock(&QAVQueueLock);
    QAVQueueDiagnostics.lastError = status;
    pthread_mutex_unlock(&QAVQueueLock);
}

static void QAVApplyQueue(AudioQueueRef queue) {
    QAVQueue *state = QAVAcquireQueue(queue);
    if (!state) return;
    pthread_mutex_lock(&QAVQueueLock);
    bool dirty = state->appliedGeneration != QAVQueueGeneration;
    uint64_t generation = QAVQueueGeneration;
    uint64_t revision = state->revision;
    float effective = QAVScaleVolume(state->requestedVolume, QAVQueueGain, QAVQueueEnabled);
    pthread_mutex_unlock(&QAVQueueLock);
    if (dirty) {
        OSStatus status = AudioQueueSetParameter(queue, kAudioQueueParam_Volume, effective);
        pthread_mutex_lock(&QAVQueueLock);
        bool current = revision == state->revision && generation == QAVQueueGeneration;
        state->appliedGeneration = status == noErr && current ? generation : 0;
        QAVQueueDiagnostics.lastError = status;
        if (status == noErr) QAVQueueDiagnostics.appliedChanges++;
        pthread_mutex_unlock(&QAVQueueLock);
    }
    QAVReleaseQueue(state);
}

static void QAVRegisterQueue(AudioQueueRef queue) {
    AudioQueueParameterValue base = 1.0f;
    OSStatus status = AudioQueueGetParameter(queue, kAudioQueueParam_Volume, &base);
    if (status != noErr) {
        QAVRecordQueueError(status);
        return;
    }
    QAVQueue *state = calloc(1, sizeof(*state));
    if (!state) {
        QAVRecordQueueError(-108);
        return;
    }
    state->queue = queue;
    state->requestedVolume = base;
    state->references = 1;
    pthread_mutex_lock(&QAVQueueLock);
    state->next = QAVQueues;
    QAVQueues = state;
    QAVQueueDiagnostics.activeQueues++;
    QAVQueueDiagnostics.createdQueues++;
    pthread_mutex_unlock(&QAVQueueLock);
    QAVApplyQueue(queue);
}

static OSStatus QAVNewOutput(const AudioStreamBasicDescription *format, AudioQueueOutputCallback callback,
                             void *userData, CFRunLoopRef runLoop, CFStringRef runLoopMode,
                             UInt32 flags, AudioQueueRef *outQueue) {
    OSStatus status = AudioQueueNewOutput(format, callback, userData, runLoop, runLoopMode, flags, outQueue);
    if (status == noErr && outQueue && *outQueue) QAVRegisterQueue(*outQueue);
    return status;
}

static OSStatus QAVNewOutputWithDispatch(AudioQueueRef *outQueue, const AudioStreamBasicDescription *format,
                                         UInt32 flags, dispatch_queue_t callbackQueue,
                                         AudioQueueOutputCallbackBlock callback) {
    OSStatus status = AudioQueueNewOutputWithDispatchQueue(outQueue, format, flags, callbackQueue, callback);
    if (status == noErr && outQueue && *outQueue) QAVRegisterQueue(*outQueue);
    return status;
}

static OSStatus QAVSetQueueParameter(AudioQueueRef queue, AudioQueueParameterID parameter, AudioQueueParameterValue value) {
    if (parameter != kAudioQueueParam_Volume) return AudioQueueSetParameter(queue, parameter, value);
    QAVQueue *state = QAVAcquireQueue(queue);
    if (!state) return AudioQueueSetParameter(queue, parameter, value);
    if (!isfinite(value) || value < 0.0f || value > 1.0f) {
        QAVReleaseQueue(state);
        return -50;
    }
    pthread_mutex_lock(&QAVQueueLock);
    float previous = state->requestedVolume;
    state->requestedVolume = value;
    uint64_t revision = ++state->revision;
    uint64_t generation = QAVQueueGeneration;
    float effective = QAVScaleVolume(value, QAVQueueGain, QAVQueueEnabled);
    state->appliedGeneration = 0;
    pthread_mutex_unlock(&QAVQueueLock);
    OSStatus status = AudioQueueSetParameter(queue, parameter, effective);
    pthread_mutex_lock(&QAVQueueLock);
    bool current = revision == state->revision && generation == QAVQueueGeneration;
    if (status != noErr && revision == state->revision) {
        state->requestedVolume = previous;
        state->revision++;
    }
    state->appliedGeneration = status == noErr && current ? generation : 0;
    QAVQueueDiagnostics.lastError = status;
    if (status == noErr) QAVQueueDiagnostics.appliedChanges++;
    pthread_mutex_unlock(&QAVQueueLock);
    QAVReleaseQueue(state);
    return status;
}

static OSStatus QAVGetQueueParameter(AudioQueueRef queue, AudioQueueParameterID parameter, AudioQueueParameterValue *value) {
    if (parameter != kAudioQueueParam_Volume || !value) return AudioQueueGetParameter(queue, parameter, value);
    QAVQueue *state = QAVAcquireQueue(queue);
    if (!state) return AudioQueueGetParameter(queue, parameter, value);
    pthread_mutex_lock(&QAVQueueLock);
    *value = state->requestedVolume;
    pthread_mutex_unlock(&QAVQueueLock);
    QAVReleaseQueue(state);
    return noErr;
}

static OSStatus QAVStartQueue(AudioQueueRef queue, const AudioTimeStamp *startTime) {
    QAVApplyQueue(queue);
    return AudioQueueStart(queue, startTime);
}

static OSStatus QAVEnqueueBuffer(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 packetCount,
                                const AudioStreamPacketDescription *packets) {
    QAVApplyQueue(queue);
    return AudioQueueEnqueueBuffer(queue, buffer, packetCount, packets);
}

static OSStatus QAVDisposeQueue(AudioQueueRef queue, Boolean immediate) {
    QAVQueue *state = QAVAcquireQueue(queue);
    OSStatus status = AudioQueueDispose(queue, immediate);
    if (state && status == noErr) {
        pthread_mutex_lock(&QAVQueueLock);
        QAVQueue **entry = &QAVQueues;
        while (*entry && *entry != state) entry = &(*entry)->next;
        if (*entry) {
            *entry = state->next;
            state->references--;
            QAVQueueDiagnostics.activeQueues--;
        }
        pthread_mutex_unlock(&QAVQueueLock);
    }
    QAVReleaseQueue(state);
    return status;
}

void QAVAudioQueueSetGain(float gain, bool enabled) {
    pthread_mutex_lock(&QAVQueueLock);
    QAVQueueGain = QAVNormalizeGain(gain);
    QAVQueueEnabled = enabled;
    QAVQueueGeneration++;
    pthread_mutex_unlock(&QAVQueueLock);
    // Apply on the next app-owned enqueue/start, never against a queue concurrently disposed by the app.
}

QAVAudioQueueDiagnostics QAVAudioQueueGetDiagnostics(void) {
    pthread_mutex_lock(&QAVQueueLock);
    QAVAudioQueueDiagnostics result = QAVQueueDiagnostics;
    pthread_mutex_unlock(&QAVQueueLock);
    return result;
}

#ifndef QAV_AUDIOQUEUE_TEST
static char QAVBundlePath[PATH_MAX];
static char QAVFirstImage[PATH_MAX];
static const void *QAVOwnImage;

static void QAVSetInstallState(int status, int code) {
    pthread_mutex_lock(&QAVQueueLock);
    QAVQueueDiagnostics.installStatus = status;
    QAVQueueDiagnostics.installErrno = code;
    pthread_mutex_unlock(&QAVQueueLock);
}

static void QAVRecordSeenImage(const char *path) {
    pthread_mutex_lock(&QAVQueueLock);
    if (!QAVQueueDiagnostics.seenImages) snprintf(QAVFirstImage, sizeof(QAVFirstImage), "%s", path);
    QAVQueueDiagnostics.seenImages++;
    pthread_mutex_unlock(&QAVQueueLock);
}

static void QAVBindQueueImage(const struct mach_header *header, intptr_t slide, const char *name) {
    if ((const void *)header == QAVOwnImage) return;
    char path[PATH_MAX];
    if (!name || !realpath(name, path)) return;
    QAVRecordSeenImage(path);
    size_t prefixLength = strlen(QAVBundlePath);
    if (strncmp(path, QAVBundlePath, prefixLength) != 0 || path[prefixLength] != '/') return;
    // System frameworks and this dylib retain their native calls, avoiding recursive or double attenuation.
    struct rebinding bindings[] = {
        {"AudioQueueNewOutput", QAVNewOutput, NULL},
        {"AudioQueueNewOutputWithDispatchQueue", QAVNewOutputWithDispatch, NULL},
        {"AudioQueueSetParameter", QAVSetQueueParameter, NULL},
        {"AudioQueueGetParameter", QAVGetQueueParameter, NULL},
        {"AudioQueueStart", QAVStartQueue, NULL},
        {"AudioQueueEnqueueBuffer", QAVEnqueueBuffer, NULL},
        {"AudioQueueDispose", QAVDisposeQueue, NULL},
        {"AudioOutputUnitStart", QAVStartOutputUnit, NULL},
        {"AudioOutputUnitStop", QAVStopOutputUnit, NULL},
        {"AudioComponentInstanceNew", QAVNewComponentInstance, NULL},
        {"AudioComponentInstanceDispose", QAVDisposeComponentInstance, NULL},
        {"AudioUnitSetProperty", QAVSetUnitProperty, NULL},
        {"AudioUnitInitialize", QAVInitializeUnit, NULL}
    };
    int result = rebind_symbols_image((void *)header, slide, bindings, sizeof(bindings) / sizeof(bindings[0]));
    pthread_mutex_lock(&QAVQueueLock);
    QAVQueueDiagnostics.scannedImages++;
    if (result != 0) QAVQueueDiagnostics.lastError = result;
    pthread_mutex_unlock(&QAVQueueLock);
}

static void QAVRebindQueueImage(const struct mach_header *header, intptr_t slide) {
    Dl_info info;
    if (!dladdr(header, &info)) return;
    QAVBindQueueImage(header, slide, info.dli_fname);
}

// Our constructor runs after the app binary and its frameworks are already mapped, so bind them directly
// instead of relying on the add-image notification replaying them.
static void QAVBindLoadedImages(void) {
    uint32_t count = _dyld_image_count();
    for (uint32_t index = 0; index < count; index++) {
        const struct mach_header *header = _dyld_get_image_header(index);
        if (header) QAVBindQueueImage(header, _dyld_get_image_vmaddr_slide(index), _dyld_get_image_name(index));
    }
}
#endif

void QAVNativeAudioInstall(const char *appBundlePath, float gain, bool enabled) {
    QAVAudioQueueSetGain(gain, enabled);
    QAVAudioUnitSetGain(gain, enabled);
#ifndef QAV_AUDIOQUEUE_TEST
    Dl_info ownInfo;
    if (!appBundlePath) {
        QAVSetInstallState(-1, 0);
        QAVRecordQueueError(-50);
        return;
    }
    if (!realpath(appBundlePath, QAVBundlePath)) {
        QAVSetInstallState(-2, errno);
        QAVRecordQueueError(-50);
        return;
    }
    if (!dladdr((void *)QAVNativeAudioInstall, &ownInfo)) {
        QAVSetInstallState(-3, 0);
        QAVRecordQueueError(-50);
        return;
    }
    QAVOwnImage = ownInfo.dli_fbase;
    QAVBindLoadedImages();
    QAVSetInstallState(1, 0);
    _dyld_register_func_for_add_image(QAVRebindQueueImage);
#else
    (void)appBundlePath;
#endif
}

void QAVAudioQueueCopyPaths(char *bundle, size_t bundleSize, char *image, size_t imageSize) {
    pthread_mutex_lock(&QAVQueueLock);
#ifndef QAV_AUDIOQUEUE_TEST
    if (bundle && bundleSize) snprintf(bundle, bundleSize, "%s", QAVBundlePath);
    if (image && imageSize) snprintf(image, imageSize, "%s", QAVFirstImage);
#else
    if (bundle && bundleSize) bundle[0] = '\0';
    if (image && imageSize) image[0] = '\0';
#endif
    pthread_mutex_unlock(&QAVQueueLock);
}
