/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_QUALITY_METRICS_H
#define MPH_QUALITY_METRICS_H
#include <stdbool.h>
#include <limits.h>
#define MPH_QUALITY_TARGET_FPS 60
#define MPH_QUALITY_WARMUP_SAMPLES 5
#define MPH_QUALITY_PROBE_SAMPLES 30
#define MPH_QUALITY_CONFIRM_SAMPLES 120
struct mph_quality_metrics {
    unsigned seen,count,minimum,zero_run;
    unsigned long long rendered,skipped;
    bool stalled;
};
static inline void mph_quality_metrics_init(struct mph_quality_metrics *m) {
    *m=(struct mph_quality_metrics){.minimum=UINT_MAX};
}
static inline void mph_quality_metrics_add(struct mph_quality_metrics *m,unsigned fps,unsigned skipped) {
    if(++m->seen<=MPH_QUALITY_WARMUP_SAMPLES) return;
    ++m->count;m->rendered+=fps;m->skipped+=skipped;
    if(fps<m->minimum)m->minimum=fps;
    m->zero_run=fps?0:m->zero_run+1;
    if(m->zero_run>=3)m->stalled=true;
}
static inline double mph_quality_metrics_average(const struct mph_quality_metrics *m) {
    return m->count?(double)m->rendered/m->count:0;
}
static inline bool mph_quality_metrics_pass(const struct mph_quality_metrics *m,unsigned required) {
    /* One-second bins allow small scheduling jitter, never a sustained 30/59 FPS. */
    return m->count>=required && !m->stalled &&
        mph_quality_metrics_average(m)>=59.5 && m->minimum>=58 &&
        (double)m->skipped*200 <= (double)m->rendered+m->skipped;
}
#endif
