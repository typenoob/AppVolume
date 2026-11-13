#ifndef QAV_SAMPLE_GAIN_H
#define QAV_SAMPLE_GAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

// In-place master gain over one AudioBuffer. Interleaved and non-interleaved layouts are both fine:
// every sample in the buffer is the same type, so scaling each one scales every channel equally.
static inline void QAVApplySampleGain(void *data, size_t bytes, uint32_t bitsPerChannel, bool isFloat, float gain) {
    if (!data || bytes == 0) return;
    if (bitsPerChannel == 32 && isFloat) {
        float *samples = (float *)data;
        size_t count = bytes / sizeof(float);
        for (size_t index = 0; index < count; index++) samples[index] *= gain;
        return;
    }
    if (bitsPerChannel == 16 && !isFloat) {
        int16_t *samples = (int16_t *)data;
        size_t count = bytes / sizeof(int16_t);
        for (size_t index = 0; index < count; index++) samples[index] = (int16_t)lrintf((float)samples[index] * gain);
        return;
    }
    if (bitsPerChannel == 32 && !isFloat) {
        // Fixed point: gain <= 1.0, so the product cannot overflow.
        int32_t *samples = (int32_t *)data;
        size_t count = bytes / sizeof(int32_t);
        int64_t scaled = (int64_t)lrintf(gain * 65536.0f);
        for (size_t index = 0; index < count; index++) samples[index] = (int32_t)(((int64_t)samples[index] * scaled) >> 16);
        return;
    }
}

#endif
