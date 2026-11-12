#ifndef QAV_AUDIO_QUEUE_VOLUME_H
#define QAV_AUDIO_QUEUE_VOLUME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t activeQueues;
    uint64_t createdQueues;
    uint64_t appliedChanges;
    uint64_t scannedImages;
    int32_t lastError;
} QAVAudioQueueDiagnostics;

void QAVAudioQueueInstall(const char *appBundlePath, float gain, bool enabled);
void QAVAudioQueueSetGain(float gain, bool enabled);
QAVAudioQueueDiagnostics QAVAudioQueueGetDiagnostics(void);

#endif
