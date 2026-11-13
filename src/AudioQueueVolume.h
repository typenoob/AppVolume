#ifndef QAV_AUDIO_QUEUE_VOLUME_H
#define QAV_AUDIO_QUEUE_VOLUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t activeQueues;
    uint64_t createdQueues;
    uint64_t appliedChanges;
    uint64_t scannedImages;
    uint64_t seenImages;
    int32_t lastError;
    int32_t installStatus;
    int32_t installErrno;
} QAVAudioQueueDiagnostics;

void QAVAudioQueueInstall(const char *appBundlePath, float gain, bool enabled);
void QAVAudioQueueSetGain(float gain, bool enabled);
QAVAudioQueueDiagnostics QAVAudioQueueGetDiagnostics(void);

// Resolved app bundle path plus the first external image dyld reported, so the
// panel can show why image scanning matched nothing. Always NUL-terminates.
void QAVAudioQueueCopyPaths(char *bundle, size_t bundleSize, char *image, size_t imageSize);

#endif
