/* SPDX-License-Identifier: Apache-2.0 */
#include "../mph_quality_metrics.h"
#include <assert.h>
#include <stdio.h>
static struct mph_quality_metrics series(unsigned fps,unsigned n,unsigned drop) {
 struct mph_quality_metrics m;mph_quality_metrics_init(&m);
 for(unsigned i=0;i<MPH_QUALITY_WARMUP_SAMPLES;++i)mph_quality_metrics_add(&m,0,0);
 for(unsigned i=0;i<n;++i)mph_quality_metrics_add(&m,fps,drop);
 return m;
}
int main(void) {
 struct mph_quality_metrics m=series(60,30,0);assert(mph_quality_metrics_pass(&m,30));assert(!mph_quality_metrics_pass(&m,120));
 m=series(60,120,0);assert(mph_quality_metrics_pass(&m,120));
 m=series(30,120,0);assert(!mph_quality_metrics_pass(&m,120));
 m=series(59,120,0);assert(!mph_quality_metrics_pass(&m,120));
 m=series(60,119,0);mph_quality_metrics_add(&m,57,0);assert(!mph_quality_metrics_pass(&m,120));
 m=series(60,120,1);assert(!mph_quality_metrics_pass(&m,120));
 m=series(60,118,0);mph_quality_metrics_add(&m,59,0);mph_quality_metrics_add(&m,58,0);assert(mph_quality_metrics_pass(&m,120));
 m=series(60,20,0);mph_quality_metrics_add(&m,0,0);mph_quality_metrics_add(&m,0,0);assert(!m.stalled);mph_quality_metrics_add(&m,0,0);assert(m.stalled);
 puts("60 FPS criteria: PASS (30/59 FPS rejected, dip/drop/stall/sample limits, 120s confirmation)");return 0;
}
