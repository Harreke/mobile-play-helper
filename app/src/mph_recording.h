/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_RECORDING_H
#define MPH_RECORDING_H
#include "trait/packet_sink.h"
#include <SDL3/SDL.h>
struct mph_recording;
enum mph_recording_state { MPH_REC_IDLE, MPH_REC_STARTING, MPH_REC_ACTIVE,
                           MPH_REC_STOPPING, MPH_REC_DONE };
struct mph_recording *mph_recording_create(void);
void mph_recording_destroy(struct mph_recording *r);
struct sc_packet_sink *mph_recording_sink(struct mph_recording *r, bool audio);
enum mph_recording_state mph_recording_state(struct mph_recording *r);
bool mph_recording_start(struct mph_recording *r);
void mph_recording_stop(struct mph_recording *r);
void mph_recording_poll(struct mph_recording *r, SDL_Window *owner);
/* CSV payload uses fixed, internal event names; no user strings. */
void mph_recording_event(struct mph_recording *r, int64_t video_pts, const char *payload);
#endif
