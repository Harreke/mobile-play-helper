/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_INPUT_H
#define MPH_INPUT_H
#include "mph_mapper.h"
#include "mph_recording.h"
#include <SDL3/SDL.h>
struct sc_input_manager;
#define MPH_LOOK_TRACE_SIZE 2048
struct mph_look_sample {
    Uint64 ns,video_frames,video_ns;
    float mouse_x,mouse_y,pending_x,pending_y,touch_x,touch_y;
    unsigned mouse_events,resets,rejected;
    int action;
    bool accepted;
};
struct mph_input {
    SDL_TimerID mcp_timer;
    SDL_Surface *mcp_toolbar;
    char mcp_request_id[65];
    bool mcp_capture, mcp_touch_down, mcp_touch_cancelled;
    float mcp_mask_x, mcp_mask_y, mcp_mask_w, mcp_mask_h;
    Uint64 mcp_touch_started, mcp_touch_until, mcp_poll_at;
    float mcp_x0, mcp_y0, mcp_x1, mcp_y1, mcp_x, mcp_y;
    struct mph_recording *recording;
    int64_t recording_frame_pts;
    struct mph_look_sample look_trace[MPH_LOOK_TRACE_SIZE];
    unsigned look_trace_next,look_trace_count,look_mouse_events;
    float look_mouse_x,look_mouse_y;
    int look_action;
    bool look_accepted;
    bool phone_off_requested;
    bool toolbar_scale_expanded;
    bool diagnostic_arrow, diagnostic_look_seen;
    Uint64 diagnostic_redraw_at;
    bool suspended, focus_lost, f1_down, f1_began_enabled, f1_used_mouse, escape_down;
    Uint64 f1_pressed_at, locator_until, locator_redraw_at;
    Uint64 fps_start_ns; unsigned fps_frames; float fps_value; SDL_TimerID fps_timer;
    Uint64 video_frames,video_ns; SDL_TimerID look_timer; Uint64 look_last_tick; SDL_Window *toolbar_window; SDL_Renderer *toolbar_renderer; struct mph_mapper mapper; bool enabled; bool editing; bool dragging; bool assigning; bool choosing_hotkey; int selected; int toolbar_pressed; char profile_path[4096]; char title[256]; char status[128]; };
bool mph_input_set_display_power(struct sc_input_manager *im, bool on);
void mph_input_init(struct sc_input_manager *im);
void mph_input_note_frame(struct sc_input_manager *im,Uint64 now);
bool mph_input_select_profile(struct sc_input_manager *im, const char *path, bool create);
void mph_toolbar_sync(struct sc_input_manager *im);
void mph_toolbar_destroy(struct sc_input_manager *im);
void mph_input_stop(struct sc_input_manager *im);
bool mph_input_handle(struct sc_input_manager *im, const SDL_Event *event);
#define MPH_TOOLBAR_WIDTH 192
#define MPH_TOOLBAR_ROWS 16
unsigned mph_toolbar_actions(const struct mph_input *in, int *actions);
bool mph_toolbar_handle(struct sc_input_manager *im, const SDL_Event *event);
void mph_toolbar_render(struct sc_input_manager *im);
void mph_input_render(struct sc_input_manager *im);
void mph_mcp_capture_frame(struct sc_input_manager *im);
#endif
