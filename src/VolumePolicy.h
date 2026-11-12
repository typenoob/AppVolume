#ifndef QAV_VOLUME_POLICY_H
#define QAV_VOLUME_POLICY_H

#include <math.h>
#include <stdbool.h>

typedef struct {
    float requestedVolume;
    float fadeStartVolume;
    double fadeStartTime;
    double fadeDuration;
} QAVVolumePolicy;

static inline float QAVNormalizeGain(float gain) {
    if (!isfinite(gain)) return 1.0f;
    return fminf(1.0f, fmaxf(0.0f, gain));
}

static inline float QAVRequestedVolumeAt(QAVVolumePolicy policy, double now) {
    if (policy.fadeDuration <= 0.0) return policy.requestedVolume;
    double progress = fmin(1.0, fmax(0.0, (now - policy.fadeStartTime) / policy.fadeDuration));
    return policy.fadeStartVolume + (policy.requestedVolume - policy.fadeStartVolume) * progress;
}

static inline double QAVRemainingFade(QAVVolumePolicy policy, double now) {
    return fmax(0.0, policy.fadeDuration - (now - policy.fadeStartTime));
}

static inline QAVVolumePolicy QAVBeginFade(QAVVolumePolicy policy, float target, double duration, double now) {
    return (QAVVolumePolicy){
        .requestedVolume = target,
        .fadeStartVolume = QAVRequestedVolumeAt(policy, now),
        .fadeStartTime = now,
        .fadeDuration = isfinite(duration) ? fmax(0.0, duration) : 0.0
    };
}

static inline float QAVScaleVolume(float requested, float gain, bool enabled) {
    if (!enabled) return requested;
    float normalized = QAVNormalizeGain(gain);
    return normalized == 0.0f ? 0.0f : requested * normalized;
}

static inline float QAVEffectiveVolume(QAVVolumePolicy policy, float gain, bool enabled) {
    return QAVScaleVolume(policy.requestedVolume, gain, enabled);
}

#endif
