/* SPDX-License-Identifier: Apache-2.0 */
/* MobilePlayHelper SDL integration. All state is owned by the UI thread. */
#include "mph_input.h"
#include "mph_text.h"
#include "mph_quality.h"
#include "input_manager.h"
#include "screen.h"
#include "events.h"
#include "android/input.h"
#include "util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#endif

/* A static coalescing flag avoids timer callbacks referencing a destroyed UI. */
static void redraw(struct sc_input_manager *im);
static void mapping_visual(struct sc_input_manager *im);
static void mph_mcp_poll(struct sc_input_manager *im);
static void mph_mcp_cancel(struct sc_input_manager *im);
static Uint32 SDLCALL mph_mcp_timer(void *unused, SDL_TimerID id, Uint32 interval);
static SDL_AtomicInt look_tick_pending;
static Uint32 SDLCALL look_timer(void *unused,SDL_TimerID id,Uint32 interval) {
    (void)unused;(void)id;
    if(SDL_CompareAndSwapAtomicInt(&look_tick_pending,0,1)) {
        SDL_Event e={0};e.type=SC_EVENT_MPH_LOOK_TICK;
        if(!SDL_PushEvent(&e)) SDL_SetAtomicInt(&look_tick_pending,0);
    }
    return interval;
}

static SDL_AtomicInt fps_tick_pending;
static Uint32 SDLCALL fps_timer(void *unused,SDL_TimerID id,Uint32 interval) {
    (void)unused;(void)id;
    if(SDL_CompareAndSwapAtomicInt(&fps_tick_pending,0,1)) {
        SDL_Event e={0};e.type=SC_EVENT_MPH_FPS_TICK;
        if(!SDL_PushEvent(&e)) SDL_SetAtomicInt(&fps_tick_pending,0);
    }
    return interval;
}
/* State is sampled on the UI thread; touch rows describe the attempted action,
 * while look_down is the mapper state immediately before that attempt. */
static void record_event(struct sc_input_manager *im,const char *kind,int id,int action,float x,float y,int accepted) {
    struct mph_input *in=&im->mph;
    enum mph_recording_state state=mph_recording_state(in->recording);
    if(state!=MPH_REC_STARTING && state!=MPH_REC_ACTIVE)return;
    struct mph_mapper *m=&in->mapper;char row[768];
    snprintf(row,sizeof(row),"%s,%d,%d,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%u,%u,%u,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%d",
        kind,id,action,x,y,accepted,in->enabled,in->suspended,in->focus_lost,
        SDL_GetWindowRelativeMouseMode(im->screen->window),m->contacts[1].down,
        m->look_pending_x,m->look_pending_y,m->look_refresh_phase,m->look_resets,m->look_rejected,
        (unsigned long long)in->video_frames,(unsigned long long)(in->video_ns/1000),
        m->look_x,m->look_y,m->sensitivity,m->look_radius,in->diagnostic_arrow);
    mph_recording_event(in->recording,in->recording_frame_pts,row);
}
void mph_input_note_frame(struct sc_input_manager *im,Uint64 now) {
    struct mph_input *in=&im->mph;
    ++in->video_frames;in->video_ns=now;
    record_event(im,"frame",0,0,0,0,-1);
    if(!in->fps_start_ns) {in->fps_start_ns=now;return;}
    ++in->fps_frames;
    Uint64 elapsed=now-in->fps_start_ns;
    if(elapsed>=500000000) {
        in->fps_value=(float)((double)in->fps_frames*1000000000.0/elapsed);
        in->fps_start_ns=now;in->fps_frames=0;
    }
}

static bool save_profile(struct mph_input *in);
static FILE *profile_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wide[4096], access[8];
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096) ||
        !MultiByteToWideChar(CP_UTF8,0,mode,-1,access,8)) return NULL;
    return _wfopen(wide,access);
#else
    return fopen(path,mode);
#endif
}


/* No disk writes while aiming: save a bounded ring when mapping stops. */
static void look_trace_save(struct mph_input *in) {
    if(!in->look_trace_count) return;
    const char *base=SDL_GetBasePath();char path[4096],from[4096],to[4096];
    snprintf(path,sizeof(path),"%sMobilePlayHelper-look.csv",base?base:"./");
    for(int i=3;i>=1;--i) {
        snprintf(from,sizeof(from),"%.*s.%d",4080,path,i);
        snprintf(to,sizeof(to),"%.*s.%d",4080,path,i+1);
        SDL_RenamePath(from,to);
    }
    snprintf(to,sizeof(to),"%.*s.1",4080,path);SDL_RenamePath(path,to);
    FILE *f=profile_open(path,"w");
    if(!f) {LOGW("MPH look trace: cannot open %s",path);return;}
    fprintf(f,"# policy=screen-drag-v1; accepted=PC_queue_only; samples=%u; radius=%.6f; sensitivity=%.6f\n",in->look_trace_count,in->mapper.look_radius,in->mapper.sensitivity);
    fprintf(f,"time_ns,mouse_events,mouse_dx,mouse_dy,action,queue_accepted,touch_x,touch_y,pending_x,pending_y,reanchors,rejected,frame_apply_count,last_frame_ns\n");
    unsigned first=(in->look_trace_next+MPH_LOOK_TRACE_SIZE-in->look_trace_count)%MPH_LOOK_TRACE_SIZE;
    for(unsigned i=0;i<in->look_trace_count;++i) {
        const struct mph_look_sample *s=&in->look_trace[(first+i)%MPH_LOOK_TRACE_SIZE];
        char action=s->action<0?'-':s->action==MPH_DOWN?'D':s->action==MPH_UP?'U':'M';
        fprintf(f,"%llu,%u,%.3f,%.3f,%c,%d,%.6f,%.6f,%.6f,%.6f,%u,%u,%llu,%llu\n",
            (unsigned long long)s->ns,s->mouse_events,s->mouse_x,s->mouse_y,action,s->accepted,
            s->touch_x,s->touch_y,s->pending_x,s->pending_y,s->resets,s->rejected,
            (unsigned long long)s->video_frames,(unsigned long long)s->video_ns);
    }
    bool ok=!ferror(f);if(fclose(f)) ok=false;
    if(ok) LOGI("MPH look trace saved: %s (%u samples)",path,in->look_trace_count);
    else LOGW("MPH look trace: incomplete write %s",path);
}

static void look_trace_record(struct mph_input *in) {
    if(!in->look_mouse_events && in->look_action<0 &&
        !in->mapper.look_pending_x && !in->mapper.look_pending_y) return;
    struct mph_mapper *m=&in->mapper;
    in->look_trace[in->look_trace_next]=(struct mph_look_sample){
        .ns=SDL_GetTicksNS(),.video_frames=in->video_frames,.video_ns=in->video_ns,
        .mouse_x=in->look_mouse_x,.mouse_y=in->look_mouse_y,.mouse_events=in->look_mouse_events,
        .action=in->look_action,.accepted=in->look_accepted,
        .touch_x=m->contacts[1].x,.touch_y=m->contacts[1].y,
        .pending_x=m->look_pending_x,.pending_y=m->look_pending_y,
        .resets=m->look_resets,.rejected=m->look_rejected};
    in->look_trace_next=(in->look_trace_next+1)%MPH_LOOK_TRACE_SIZE;
    if(in->look_trace_count<MPH_LOOK_TRACE_SIZE) ++in->look_trace_count;
    in->look_mouse_events=0;in->look_mouse_x=0;in->look_mouse_y=0;
}

static bool emit(void *userdata, unsigned id, enum mph_action a, float x, float y) {
    struct sc_input_manager *im = userdata;
    if(id==1) {im->mph.look_action=a;im->mph.look_accepted=false;}
    if (!im->controller || im->disconnected) {record_event(im,"touch",id,a,x,y,0);return false;}
    struct sc_size size = im->screen->frame_size;
    if (!size.width || !size.height) {record_event(im,"touch",id,a,x,y,0);return false;}
    struct sc_control_msg msg; memset(&msg,0,sizeof(msg));
    msg.type=SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    msg.inject_touch_event.action = a == MPH_DOWN ? AMOTION_EVENT_ACTION_DOWN :
                                    a == MPH_UP ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_MOVE;
    msg.inject_touch_event.pointer_id = 0x4d504800ULL + id;
    msg.inject_touch_event.position.screen_size = size;
    msg.inject_touch_event.position.point = (struct sc_point) {
        (int32_t)(x * (size.width-1)), (int32_t)(y * (size.height-1)) };
    msg.inject_touch_event.pressure = a == MPH_UP ? 0 : 1;
    bool ok=sc_controller_push_msg(im->controller, &msg);
    if(id==1) im->mph.look_accepted=ok;
    if(id==1 && ok) im->mph.diagnostic_look_seen=true;
    record_event(im,"touch",id,a,x,y,ok);
    return ok;
}

static int keypad_action(int key) {
    switch(key) {
        case SDL_SCANCODE_KP_PLUS: return 2;
        case SDL_SCANCODE_KP_MINUS: return 3;
        case SDL_SCANCODE_KP_MULTIPLY: return 4;
        case SDL_SCANCODE_KP_DIVIDE: return 12;
        default: return -1;
    }
}
static bool reserved_key(int key) {
    return (key>=SDL_SCANCODE_F1 && key<=SDL_SCANCODE_F12) || keypad_action(key)>=0 || key==SDL_SCANCODE_GRAVE || key==SDL_SCANCODE_NUMLOCKCLEAR;
}

static bool unit(float f) { return isfinite(f) && f >= 0 && f <= 1; }
static bool load_profile(struct mph_mapper *m, const char *path, bool missing_ok) {
    FILE *f = profile_open(path, "r");
    if (!f) { if (missing_ok && errno==ENOENT) return true;
        LOGE("MPH: cannot open profile %s", path); return false; }
    char line[256], kind[32], key[64], extra;
    unsigned number = 0;
    bool ok = true;
    m->count = 0;
    while (fgets(line, sizeof(line), f)) {
        ++number;
        char *comment = strchr(line, '#'); if (comment) *comment = 0;
        if (sscanf(line, "%31s", kind) != 1) continue;
        float x, y, r, s;
        int toggle; char tail;
        if (!strcmp(kind,"toggle") && sscanf(line,"%*s %d %c",&toggle,&tail)==1 &&
            toggle>=SDL_SCANCODE_F1 && toggle<=SDL_SCANCODE_F12 && toggle!=SDL_SCANCODE_F6 && toggle!=SDL_SCANCODE_F9) {
            m->toggle_key=SDL_SCANCODE_F1; /* accept legacy setting, always use F1 */
        } else if (!strcmp(kind, "joystick") &&
            sscanf(line, "%*s %f %f %f %c", &x, &y, &r, &extra) == 3 &&
            unit(x) && unit(y) && isfinite(r) && r > 0 && r <= .4f &&
            y-r >= 0 && y+r <= 1) {
            m->joy_x=x; m->joy_y=y; m->joy_radius=r;
        } else if (!strcmp(kind, "look") &&
            sscanf(line, "%*s %f %f %f %f %c", &x, &y, &r, &s, &extra) == 4 &&
            unit(x) && unit(y) && isfinite(r) && r > 0 && r <= .4f &&
            y-r >= 0 && y+r <= 1 && isfinite(s) && s > 0 && s <= .05f) {
            m->look_x=x; m->look_y=y; m->look_radius=r; m->sensitivity=s;
        } else if (!strcmp(kind, "bind") &&
            sscanf(line, "%*s %63s %f %f %c", key, &x, &y, &extra) == 3 &&
            unit(x) && unit(y) && m->count < MPH_BINDINGS) {
            int code;
            if (!strcmp(key, "Mouse1")) code=513;
            else if (!strcmp(key, "Mouse2")) code=515;
            else if (!strcmp(key, "Mouse3")) code=514;
            else if (!strcmp(key, "Mouse4")) code=516;
            else if (!strcmp(key, "Mouse5")) code=517;
            else if (!strcmp(key, "LCtrl")) code=SDL_SCANCODE_LCTRL;
            else if (!strcmp(key, "LShift")) code=SDL_SCANCODE_LSHIFT;
            else if (!strncmp(key,"Key",3)) { char *end; long v=strtol(key+3,&end,10); code=(!*end && v>0 && v<SDL_SCANCODE_COUNT) ? (int)v : 0; }
            else code = SDL_GetScancodeFromName(key);
            if (reserved_key(code)) {LOGW("MPH：F1–F12、·、Num Lock 和小键盘运算键保留给工具，忽略对应绑定");continue;}
            if (!code) { ok=false; break; }
            for (unsigned i=0; i<m->count; ++i) if (m->bindings[i].key==code) ok=false;
            if (!ok) break;
            m->bindings[m->count++] = (struct mph_binding){code,x,y};
        } else { ok=false; break; }
    }
    for (unsigned i=0;i<m->count;++i) if(m->bindings[i].key==m->toggle_key) ok=false;
    if (ferror(f)) ok=false;
    fclose(f);
    if (!ok) LOGE("MPH: invalid profile %s, line %u", path, number);
    return ok;
}

static void defaults(struct mph_mapper *m) {
    const struct mph_binding defaults[] = {
        {513,.90f,.52f}, {515,.80f,.52f}, {SDL_SCANCODE_SPACE,.90f,.73f},
        {SDL_SCANCODE_C,.81f,.84f}, {SDL_SCANCODE_R,.72f,.84f},
        {SDL_SCANCODE_F,.60f,.65f}, {SDL_SCANCODE_1,.44f,.92f},
        {SDL_SCANCODE_2,.54f,.92f} };
    memcpy(m->bindings, defaults, sizeof(defaults)); m->count=8;
}

static void toolbar_init(struct sc_input_manager *im) {
    if(!im->screen->video) return;
    struct mph_input *in=&im->mph;
    in->toolbar_window=SDL_CreateWindow("MobilePlayHelper 工具条",MPH_TOOLBAR_WIDTH,600,
        SDL_WINDOW_HIDDEN|SDL_WINDOW_BORDERLESS|SDL_WINDOW_UTILITY|SDL_WINDOW_NOT_FOCUSABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(!in->toolbar_window) {LOGE("MPH toolbar: %s",SDL_GetError());return;}
#ifdef _WIN32
    HWND main=SDL_GetPointerProperty(SDL_GetWindowProperties(im->screen->window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    HWND bar=SDL_GetPointerProperty(SDL_GetWindowProperties(in->toolbar_window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    SetWindowLongPtrW(bar,GWLP_HWNDPARENT,(LONG_PTR)main);
#endif
    in->toolbar_renderer=SDL_CreateRenderer(in->toolbar_window,NULL);
    if(!in->toolbar_renderer) {SDL_DestroyWindow(in->toolbar_window);in->toolbar_window=NULL;}
}

void mph_toolbar_destroy(struct sc_input_manager *im) {
    mph_mcp_cancel(im);
    if(im->mph.mcp_timer) {SDL_RemoveTimer(im->mph.mcp_timer);im->mph.mcp_timer=0;}
    SDL_DestroySurface(im->mph.mcp_toolbar);im->mph.mcp_toolbar=NULL;
    mph_recording_destroy(im->mph.recording);im->mph.recording=NULL;
    if(im->mph.fps_timer) {SDL_RemoveTimer(im->mph.fps_timer);im->mph.fps_timer=0;}
    if(im->mph.look_timer) {SDL_RemoveTimer(im->mph.look_timer);im->mph.look_timer=0;}
    if(im->mph.toolbar_renderer) {mph_text_destroy(im->mph.toolbar_renderer);SDL_DestroyRenderer(im->mph.toolbar_renderer);}
    if(im->mph.toolbar_window) SDL_DestroyWindow(im->mph.toolbar_window);
    im->mph.toolbar_renderer=NULL;im->mph.toolbar_window=NULL;
}

void mph_toolbar_sync(struct sc_input_manager *im) {
    SDL_Window *bar=im->mph.toolbar_window,*main=im->screen->window;
    if(!bar) return;
    SDL_WindowFlags flags=SDL_GetWindowFlags(main);
    if(flags&(SDL_WINDOW_HIDDEN|SDL_WINDOW_MINIMIZED|SDL_WINDOW_FULLSCREEN)) {SDL_HideWindow(bar);return;}
    int x,y,w,h;SDL_GetWindowPosition(main,&x,&y);SDL_GetWindowSize(main,&w,&h);
    int actions[MPH_TOOLBAR_ROWS];
    int rows=(int)mph_toolbar_actions(&im->mph,actions);
    int target_x=x+w,target_y=y,target_h=48+rows*34+8;
#ifdef _WIN32
    HWND hwnd=SDL_GetPointerProperty(SDL_GetWindowProperties(main),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    RECT outer;
    /* GetWindowRect includes invisible resize borders on Windows. */
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&outer,sizeof(outer))) ||
        GetWindowRect(hwnd,&outer)) {target_x=outer.right;target_y=outer.top;}
#endif
    int bx,by,bw,bh;SDL_GetWindowPosition(bar,&bx,&by);SDL_GetWindowSize(bar,&bw,&bh);
    if(bx!=target_x || by!=target_y) SDL_SetWindowPosition(bar,target_x,target_y);
    if(bw!=MPH_TOOLBAR_WIDTH || bh!=target_h) SDL_SetWindowSize(bar,MPH_TOOLBAR_WIDTH,target_h);
    if(SDL_GetWindowFlags(bar)&SDL_WINDOW_HIDDEN) SDL_ShowWindow(bar);
}

void mph_input_init(struct sc_input_manager *im) {
    memset(&im->mph, 0, sizeof(im->mph));
    im->mph.recording=mph_recording_create();
    im->mph.recording_frame_pts=AV_NOPTS_VALUE;
    mph_init(&im->mph.mapper);
    im->mph.mapper.emit=emit; im->mph.mapper.userdata=im;
    im->mph.selected=-1;
    im->mph.toolbar_pressed=-1;
    defaults(&im->mph.mapper);
    const char *override=SDL_getenv("MPH_PROFILE");
    if (override && *override) snprintf(im->mph.profile_path,sizeof(im->mph.profile_path),"%s",override);
    else {
        const char *base=SDL_GetBasePath();
        char folder[4096]; SDL_PathInfo info;
        snprintf(folder,sizeof(folder),"%s../profiles",base?base:"./");
        if (!SDL_GetPathInfo(folder,&info) || info.type!=SDL_PATHTYPE_DIRECTORY)
            snprintf(folder,sizeof(folder),"%sprofiles",base?base:"./");
        SDL_CreateDirectory(folder);
        snprintf(im->mph.profile_path,sizeof(im->mph.profile_path),"%.*s/default-fps.mph",4000,folder);
    }
    struct mph_mapper candidate=im->mph.mapper;
    if (load_profile(&candidate,im->mph.profile_path,true)) im->mph.mapper=candidate;
    toolbar_init(im);
    if(im->screen->video && SDL_getenv("MPH_MCP_DIR"))
        im->mph.mcp_timer=SDL_AddTimer(16,mph_mcp_timer,NULL);
    if(im->screen->video) {SDL_SetAtomicInt(&fps_tick_pending,0);im->mph.fps_timer=SDL_AddTimer(500,fps_timer,NULL);}
    mapping_visual(im);
    /* Initialization intentionally never enables mapping or captures the cursor. */
}

bool mph_input_set_display_power(struct sc_input_manager *im, bool on) {
    if (!im->controller || im->camera || im->disconnected || im->screen->paused) return false;
    struct sc_control_msg msg = {0};
    msg.type = SC_CONTROL_MSG_TYPE_SET_DISPLAY_POWER;
    msg.set_display_power.on = on;
    if (!sc_controller_push_msg(im->controller, &msg)) {
        LOGW("MPH: could not queue phone display power request");
        return false;
    }
    im->mph.phone_off_requested = !on;
    LOGI("MPH: phone display power requested: %s", on ? "on" : "off");
    mph_toolbar_render(im);
    return true;
}

/* enabled is the user's intent; suspended owns no mapped contacts or capture. */
static void mapping_visual(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(in->enabled) SDL_SetWindowTitle(im->screen->window,in->suspended?
        "MobilePlayHelper [映射已暂停，等待恢复]":"MobilePlayHelper [映射开启] F1：短按关闭 / 按住临时鼠标");
#ifdef _WIN32
    HWND hwnd=SDL_GetPointerProperty(SDL_GetWindowProperties(im->screen->window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    COLORREF color=in->enabled?(in->suspended?RGB(180,112,15):RGB(22,105,210)):RGB(255,255,255);
    COLORREF text=in->enabled?RGB(255,255,255):RGB(0,0,0);
    DwmSetWindowAttribute(hwnd,DWMWA_CAPTION_COLOR,&color,sizeof(color));
    DwmSetWindowAttribute(hwnd,DWMWA_TEXT_COLOR,&text,sizeof(text));
#endif
    redraw(im);
}
static bool escape_send(struct sc_input_manager *im,bool down) {
    struct sc_control_msg msg={0};msg.type=SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
    msg.inject_keycode.action=down?AKEY_EVENT_ACTION_DOWN:AKEY_EVENT_ACTION_UP;
    msg.inject_keycode.keycode=AKEYCODE_BACK;
    if(!im->controller || im->disconnected || !sc_controller_push_msg(im->controller,&msg))return false;
    im->mph.escape_down=down;return true;
}
static void release_normal_mouse(struct sc_input_manager *im) {
    if(!im->controller || (!im->mouse_buttons_state && !im->vfinger_down))return;
    float x,y;SDL_GetMouseState(&x,&y);
    struct sc_point pos=sc_screen_convert_window_to_frame_coords(im->screen,x,y);
    struct sc_size size=im->screen->frame_size;
    if(!size.width || !size.height)return;
    pos.x=MAX(0,MIN(pos.x,size.width-1));pos.y=MAX(0,MIN(pos.y,size.height-1));
    uint64_t ids[]={im->vfinger_down?SC_POINTER_ID_GENERIC_FINGER:SC_POINTER_ID_MOUSE,SC_POINTER_ID_VIRTUAL_FINGER};
    bool ok=true;
    for(unsigned i=0;i<(im->vfinger_down?2u:1u);++i) {
        struct sc_control_msg msg={0};msg.type=SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
        msg.inject_touch_event.action=AMOTION_EVENT_ACTION_UP;msg.inject_touch_event.pointer_id=ids[i];
        msg.inject_touch_event.position.screen_size=size;msg.inject_touch_event.position.point=pos;
        if(!sc_controller_push_msg(im->controller,&msg))ok=false;
    }
    if(ok){im->mouse_buttons_state=0;im->vfinger_down=false;}
}
static void mapping_suspend(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(!in->enabled)return;
    bool changed=!in->suspended;in->suspended=true;
    mph_release(&in->mapper);
    if(in->escape_down)escape_send(im,false);
    in->look_last_tick=0;in->look_mouse_events=0;in->look_mouse_x=0;in->look_mouse_y=0;
    SDL_SetWindowRelativeMouseMode(im->screen->window,false);
    if(changed){record_event(im,"mapping_suspend",0,0,0,0,-1);mapping_visual(im);LOGI("MPH mapping suspended; intent preserved");}
}
static bool mapping_inputs_released(struct sc_input_manager *im) {
    int count;const bool *keys=SDL_GetKeyboardState(&count);
    for(int i=0;i<count;++i)if(keys[i])return false;
    return !SDL_GetMouseState(NULL,NULL) && !im->mouse_buttons_state && !im->vfinger_down;
}
static void mapping_resume(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(!in->enabled || !in->suspended || in->focus_lost || in->f1_down || !mapping_inputs_released(im))return;
    struct sc_screen *s=im->screen;
    if(im->disconnected || s->paused || !im->controller || !s->frame_size.height || s->orientation!=SC_ORIENTATION_0)return;
    if(!mph_release(&in->mapper) || (in->escape_down && !escape_send(im,false)))return;
    if(!SDL_SetWindowRelativeMouseMode(s->window,true))return;
    in->suspended=false;in->locator_until=0;in->look_last_tick=0;
    SDL_GetRelativeMouseState(NULL,NULL); /* discard deltas from cursor navigation */
    record_event(im,"mapping_resume",0,0,0,0,-1);
    mapping_visual(im);LOGI("MPH mapping resumed after release check");
}
void mph_input_stop(struct sc_input_manager *im) {
    mph_mcp_cancel(im);
    if(im->mph.look_timer) {SDL_RemoveTimer(im->mph.look_timer);im->mph.look_timer=0;}
    if (!im->mph.enabled) return;
    LOGI("MPH look statistics: reanchors=%u rejected=%u input_overflows=%u",
        im->mph.mapper.look_resets,im->mph.mapper.look_rejected,im->mph.mapper.look_overflows);
    if (!mph_release(&im->mph.mapper)) LOGW("MPH: some contacts could not be released (connection/queue failure)");
    SDL_SetWindowRelativeMouseMode(im->screen->window, false);
    SDL_SetWindowTitle(im->screen->window, im->mph.title);
    im->mph.enabled=false;
    im->mph.suspended=false;im->mph.f1_down=false;im->mph.locator_until=0;
    record_event(im,"mapping_stop",0,0,0,0,-1);
    if(im->mph.escape_down)escape_send(im,false);
    mapping_visual(im);
    look_trace_save(&im->mph);
    LOGI("MPH：普通模式（%s 开启映射）",SDL_GetScancodeName(im->mph.mapper.toggle_key));
}

static void start(struct sc_input_manager *im) {
    struct sc_screen *screen = im->screen;
    if (!im->controller || im->camera || im->disconnected || screen->paused ||
        !screen->video || !screen->frame_size.width || !screen->frame_size.height ||
        (im->mp && im->mp->relative_mode) || screen->orientation != SC_ORIENTATION_0) {
        LOGW("MPH: mapping requires live video, control, SDK mouse and display orientation 0"); return;
    }
    int count;
    const bool *keys=SDL_GetKeyboardState(&count);
    for (int i=0; i<count; ++i) if (keys[i] && i != im->mph.mapper.toggle_key) {
        LOGW("MPH: release other keys before switching modes"); return;
    }
    if (SDL_GetMouseState(NULL,NULL) || im->mouse_buttons_state || im->vfinger_down) {
        LOGW("MPH: release mouse buttons before switching modes"); return;
    }
    if (!mph_release(&im->mph.mapper)) { LOGE("MPH: pending contact release failed"); return; }
    if(im->mph.escape_down && !escape_send(im,false))return;
    struct mph_mapper candidate;
    mph_init(&candidate); candidate.emit=emit; candidate.userdata=im;
    defaults(&candidate);
    if (!load_profile(&candidate,im->mph.profile_path,true)) return;
    float aspect=(float)screen->frame_size.width/screen->frame_size.height;
    if (candidate.joy_x-candidate.joy_radius/aspect < 0 ||
        candidate.joy_x+candidate.joy_radius/aspect > 1 ||
        candidate.look_x-candidate.look_radius/aspect < 0 ||
        candidate.look_x+candidate.look_radius/aspect > 1) {
        LOGE("MPH: profile zones do not fit current screen aspect ratio"); return;
    }
    if (!SDL_SetWindowRelativeMouseMode(screen->window,true)) {
        LOGE("MPH: mouse capture failed: %s",SDL_GetError()); return;
    }
    SDL_SetAtomicInt(&look_tick_pending,0);
    im->mph.look_last_tick=0;
    im->mph.diagnostic_look_seen=false;im->mph.diagnostic_redraw_at=0;
    im->mph.look_trace_count=0;im->mph.look_trace_next=0;
    im->mph.look_mouse_events=0;im->mph.look_mouse_x=0;im->mph.look_mouse_y=0;
    LOGI("MPH look policy: screen-drag-v1; center start, physical screen bounds");
    im->mph.look_timer=SDL_AddTimer(8,look_timer,NULL);
    if(!im->mph.look_timer) {
        SDL_SetWindowRelativeMouseMode(screen->window,false);
        LOGE("MPH: cannot start look timer: %s",SDL_GetError());return;
    }
    im->mph.mapper=candidate;
    snprintf(im->mph.title,sizeof(im->mph.title),"%s",SDL_GetWindowTitle(screen->window));
    char title[128];
    snprintf(title,sizeof(title),"MobilePlayHelper [映射模式] %s：关闭映射",SDL_GetScancodeName(candidate.toggle_key));
    SDL_SetWindowTitle(screen->window,title);
    im->mph.enabled=true;im->mph.suspended=false;im->mph.focus_lost=false;
    record_event(im,"mapping_start",0,0,0,0,-1);
    mapping_visual(im);
    LOGI("MPH：映射已开启；%s 关闭映射",SDL_GetScancodeName(candidate.toggle_key));
}


static bool save_profile(struct mph_input *in) {
    struct mph_mapper *m=&in->mapper;
    for (unsigned i=0;i<m->count;++i) if (!m->bindings[i].key) {
        snprintf(in->status,sizeof(in->status),"存在未绑定点位：选中后按 F2，再按目标键"); return false;
    }
    const char *path=in->profile_path;
    char temp[4096];
    if (snprintf(temp,sizeof(temp),"%s.tmp",path)>=(int)sizeof(temp)) return false;
    FILE *f=profile_open(temp,"w");
    if (!f) { snprintf(in->status,sizeof(in->status),"无法写入配置，请检查文件夹权限"); return false; }
    fprintf(f,"# MobilePlayHelper profile v1; x/y are fractions of frame width/height (0..1)\n");
    fprintf(f,"toggle %d\n",m->toggle_key);
    fprintf(f,"joystick %.6f %.6f %.6f\nlook %.6f %.6f %.6f %.6f\n",
        m->joy_x,m->joy_y,m->joy_radius,m->look_x,m->look_y,m->look_radius,m->sensitivity);
    for (unsigned i=0;i<m->count;++i) {
        struct mph_binding *b=&m->bindings[i]; char name[32];
        if (b->key>512) {
            int mouse=b->key-512;
            snprintf(name,sizeof(name),"Mouse%d",mouse==2?3:mouse==3?2:mouse);
        } else snprintf(name,sizeof(name),"Key%d",b->key);
        fprintf(f,"bind %s %.6f %.6f\n",name,b->x,b->y);
    }
    bool ok=!ferror(f); if (fclose(f)) ok=false;
    if (ok) ok=SDL_RenamePath(temp,path);
    if (!ok) snprintf(in->status,sizeof(in->status),"保存失败，编辑内容已保留，请查看日志");
    else LOGI("MPH: saved profile %s",path);
    return ok;
}

bool mph_input_select_profile(struct sc_input_manager *im,const char *path,bool create) {
    struct mph_input *in=&im->mph;
    if(in->editing || strlen(path)>=sizeof(in->profile_path)) return false;
    mph_input_stop(im);
    struct mph_mapper candidate=in->mapper;
    if(!create) {
        mph_init(&candidate); defaults(&candidate);candidate.emit=emit;candidate.userdata=im;
        if(!load_profile(&candidate,path,false)) return false;
    }
    char previous[sizeof(in->profile_path)];
    memcpy(previous,in->profile_path,sizeof(previous));
    snprintf(in->profile_path,sizeof(in->profile_path),"%s",path);
    if(create && !save_profile(in)) {
        memcpy(in->profile_path,previous,sizeof(previous));return false;
    }
    const char *name=strrchr(path,'/'), *back=strrchr(path,'\\');
    if(back && (!name || back>name)) name=back;
    char title[256];snprintf(title,sizeof(title),"MobilePlayHelper - %s",name?name+1:path);
    SDL_SetWindowTitle(im->screen->window,title);
    in->mapper=candidate;in->selected=-1;in->assigning=false;in->choosing_hotkey=false;
    snprintf(in->status,sizeof(in->status),"已切换配置，点击“编辑布局”进行设置");
    /* A profile switch never starts game input. */
    return true;
}

static void profile_dialog(struct sc_input_manager *im,bool create) {
#ifdef _WIN32
    HWND owner=SDL_GetPointerProperty(SDL_GetWindowProperties(im->screen->window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    if(im->mph.editing) {
        MessageBoxW(owner,L"请先保存或放弃当前布局编辑，再切换游戏配置。",L"MobilePlayHelper",MB_OK|MB_ICONINFORMATION);return;
    }
    wchar_t path[4096]={0},folder[4096]={0};
    MultiByteToWideChar(CP_UTF8,0,im->mph.profile_path,-1,folder,4096);
    wchar_t *slash=wcsrchr(folder,L'/'),*back=wcsrchr(folder,L'\\');
    if(back && (!slash || back>slash)) slash=back;
    if(slash) *slash=0;
    for(wchar_t *p=folder;*p;++p) if(*p==L'/') *p=L'\\';
    OPENFILENAMEW dialog;memset(&dialog,0,sizeof(dialog));dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=owner;dialog.lpstrFile=path;dialog.nMaxFile=4096;
    dialog.lpstrFilter=L"MobilePlayHelper 游戏配置 (*.mph)\0*.mph\0\0";
    dialog.lpstrInitialDir=folder;dialog.lpstrDefExt=L"mph";
    dialog.lpstrTitle=create?L"新建游戏配置（复制当前布局）":L"打开游戏配置";
    dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|
                 (create?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    bool chosen=create?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog);
    if(!chosen) {
        if(CommDlgExtendedError()) MessageBoxW(owner,L"无法打开配置选择窗口。",L"MobilePlayHelper",MB_OK|MB_ICONERROR);
        return;
    }
    char utf8[4096];
    if(!WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),NULL,NULL) ||
        !mph_input_select_profile(im,utf8,create))
        MessageBoxW(owner,L"无法加载或保存此配置，已保留原配置。",L"MobilePlayHelper",MB_OK|MB_ICONERROR);
#else
    (void)im;(void)create;
#endif
}

static void redraw(struct sc_input_manager *im) {
    if (!im->screen->window_shown) return;
    SDL_Event e; memset(&e,0,sizeof(e)); e.type=SDL_EVENT_WINDOW_EXPOSED;
    e.window.windowID=SDL_GetWindowID(im->screen->window);
    SDL_PushEvent(&e);
}

static void assign(struct mph_input *in,int key) {
    if (in->selected<0 || key>517) return;
    if (reserved_key(key) || !key) {
        snprintf(in->status,sizeof(in->status),"F1–F12、·、Num Lock 和小键盘运算键为工具保留键"); return;
    }
    for (unsigned i=0;i<in->mapper.count;++i)
        if ((int)i!=in->selected && in->mapper.bindings[i].key==key) {
            snprintf(in->status,sizeof(in->status),"按键已被占用，请先修改或删除原绑定"); return;
        }
    in->mapper.bindings[in->selected].key=key;
    in->assigning=false;
    snprintf(in->status,sizeof(in->status),"绑定已更新：F5 保存退出");
}

static void editor_discard(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if (!in->editing) return;
    struct mph_mapper restored; mph_init(&restored); defaults(&restored);
    restored.emit=emit;restored.userdata=im;
    if (!load_profile(&restored,in->profile_path,true)) {
        snprintf(in->status,sizeof(in->status),"配置读取失败，编辑内容已保留");return;
    }
    in->mapper=restored;in->editing=false;in->dragging=false;in->assigning=false;
    in->selected=-1;in->choosing_hotkey=false;
    SDL_SetWindowTitle(im->screen->window,in->title);
}

static void editor_delete(struct mph_input *in) {
    if (!in->editing || in->selected<0) return;
    struct mph_mapper *m=&in->mapper;
    unsigned i=in->selected;
    memmove(&m->bindings[i],&m->bindings[i+1],(m->count-i-1)*sizeof(m->bindings[0]));
    --m->count;in->selected=-1;in->assigning=false;in->dragging=false;
    snprintf(in->status,sizeof(in->status),"已删除点位；F5 保存退出");
}

static bool editor_handle(struct sc_input_manager *im,const SDL_Event *e) {
    struct mph_input *in=&im->mph; struct mph_mapper *m=&in->mapper;
    if (e->type==SDL_EVENT_KEY_DOWN && e->key.scancode==SDL_SCANCODE_F9 && !e->key.repeat) {
        if (in->editing) return true; /* F9 enters only; F5 saves. */
        if (im->screen->video && im->screen->frame_size.width &&
                   im->screen->orientation==SC_ORIENTATION_0 && !im->disconnected &&
                   !(im->mp && im->mp->relative_mode)) {
            if(im->mph.enabled) {LOGI("请先按 F1 关闭映射，再编辑布局");return true;}
            struct mph_mapper candidate; mph_init(&candidate); defaults(&candidate);
            candidate.emit=emit; candidate.userdata=im;
            if (!load_profile(&candidate,im->mph.profile_path,true)) return true;
            *m=candidate;
            snprintf(in->title,sizeof(in->title),"%s",SDL_GetWindowTitle(im->screen->window));
            SDL_SetWindowTitle(im->screen->window,"MobilePlayHelper [布局编辑] F5 保存退出");
            in->editing=true; in->selected=-1; in->assigning=false;
            snprintf(in->status,sizeof(in->status),"点击空白处后按键新增；拖动点位调整位置");
        }
        redraw(im); return true;
    }
    if ((e->type==SDL_EVENT_KEY_UP || e->type==SDL_EVENT_KEY_DOWN) &&
        (e->key.scancode==SDL_SCANCODE_F9 || (!in->editing && e->key.scancode==SDL_SCANCODE_F5))) return true;
    if (!in->editing) return false;
    if (e->type==SDL_EVENT_WINDOW_FOCUS_LOST) in->dragging=false;
    if (e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        SDL_Scancode k=e->key.scancode;
        if (k==SDL_SCANCODE_F5) {
            if (save_profile(in)) {
                in->editing=false;in->dragging=false;in->assigning=false;in->selected=-1;
                SDL_SetWindowTitle(im->screen->window,in->title);
            } else {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"布局保存失败",in->status,im->screen->window);
            }
        } else if (k==SDL_SCANCODE_F2 && in->selected>=0) {
            in->assigning=true;snprintf(in->status,sizeof(in->status),"请按目标键；Esc 也可绑定");
        } else if (k==SDL_SCANCODE_F3 || k==SDL_SCANCODE_F4) {
            float *r=in->selected==-2?&m->joy_radius:NULL;
            if (r) *r=fmaxf(.01f,fminf(.4f,*r+(k==SDL_SCANCODE_F4?.01f:-.01f)));
        } else if (k==SDL_SCANCODE_F7 || k==SDL_SCANCODE_F8) {
            m->sensitivity=fmaxf(.0001f,fminf(.05f,m->sensitivity*(k==SDL_SCANCODE_F8?1.1f:1/1.1f)));
        } else if (!reserved_key(k) && in->assigning) assign(in,k);
        float aspect=(float)im->screen->frame_size.width/im->screen->frame_size.height;
        m->joy_radius=fminf(m->joy_radius,fminf(.49f,aspect*.49f));
        m->look_radius=fminf(m->look_radius,fminf(.49f,aspect*.49f));
        m->joy_x=fmaxf(m->joy_radius/aspect,fminf(1-m->joy_radius/aspect,m->joy_x));
        m->joy_y=fmaxf(m->joy_radius,fminf(1-m->joy_radius,m->joy_y));
        m->look_x=fmaxf(m->look_radius/aspect,fminf(1-m->look_radius/aspect,m->look_x));
        m->look_y=fmaxf(m->look_radius,fminf(1-m->look_radius,m->look_y));
        redraw(im); return true;
    }
    if (e->type==SDL_EVENT_MOUSE_BUTTON_DOWN && in->assigning) {
        assign(in,512+e->button.button); redraw(im); return true;
    }
    if (e->type==SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button==SDL_BUTTON_LEFT) {
        SDL_FRect r=im->screen->rect;
        float x=(e->button.x-r.x)/r.w,y=(e->button.y-r.y)/r.h;
        if (!unit(x)||!unit(y)) return true;
        in->selected=-1;
        for (int i=-3;i<(int)m->count;++i) {
            if (i==-1) continue;
            float px=i==-3?m->look_x:i==-2?m->joy_x:m->bindings[i].x;
            float py=i==-3?m->look_y:i==-2?m->joy_y:m->bindings[i].y;
            if (fabsf(x-px)*r.w<22 && fabsf(y-py)*r.h<22) in->selected=i;
        }
        if (in->selected==-1 && m->count<MPH_BINDINGS) {
            in->selected=m->count; m->bindings[m->count++]=(struct mph_binding){0,x,y};
            in->assigning=true;
            snprintf(in->status,sizeof(in->status),"新点位：请按键盘键或鼠标键");
        } else in->dragging=in->selected!=-1;
        redraw(im); return true;
    }
    if (e->type==SDL_EVENT_MOUSE_BUTTON_UP) { in->dragging=false; return true; }
    if (e->type==SDL_EVENT_MOUSE_MOTION && in->dragging) {
        SDL_FRect r=im->screen->rect;
        float x=fmaxf(0,fminf(1,(e->motion.x-r.x)/r.w));
        float y=fmaxf(0,fminf(1,(e->motion.y-r.y)/r.h));
        float aspect=(float)im->screen->frame_size.width/im->screen->frame_size.height;
        if (in->selected < -1) {
            float radius=in->selected==-2?m->joy_radius:m->look_radius;
            x=fmaxf(radius/aspect,fminf(1-radius/aspect,x));
            y=fmaxf(radius,fminf(1-radius,y));
        }
        if (in->selected==-2) {m->joy_x=x;m->joy_y=y;}
        else if (in->selected==-3) {m->look_x=x;m->look_y=y;}
        else if (in->selected>=0) {m->bindings[in->selected].x=x;m->bindings[in->selected].y=y;}
        snprintf(in->status,sizeof(in->status),"坐标：%.0f,%.0f",x*100,y*100);
        redraw(im); return true;
    }
    return e->type==SDL_EVENT_KEY_UP || e->type==SDL_EVENT_KEY_DOWN ||
        e->type==SDL_EVENT_TEXT_INPUT || e->type==SDL_EVENT_MOUSE_MOTION ||
        e->type==SDL_EVENT_MOUSE_BUTTON_DOWN || e->type==SDL_EVENT_MOUSE_WHEEL ||
        e->type==SDL_EVENT_FINGER_DOWN || e->type==SDL_EVENT_FINGER_UP || e->type==SDL_EVENT_FINGER_MOTION;
}

static void render_diagnostic_arrow(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(!in->enabled || in->suspended || !in->diagnostic_arrow) return;
    SDL_Renderer *renderer=im->screen->renderer;
    SDL_FRect r=im->screen->rect;
    if(!renderer || r.w<=0 || r.h<=0) return;
    const struct mph_contact *c=&in->mapper.contacts[1];
    float nx=in->diagnostic_look_seen?c->x:in->mapper.look_x;
    float ny=in->diagnostic_look_seen?c->y:in->mapper.look_y;
    float scale=SDL_GetWindowPixelDensity(im->screen->window);
    float x=(r.x+nx*r.w)*scale,y=(r.y+ny*r.h)*scale;
    /* Keep the tip exactly on the injected point; turn the body inward at edges. */
    float sx=(nx*r.w>r.w-26?-1:1)*scale;
    float sy=(ny*r.h>r.h-32?-1:1)*scale;
    const SDL_FPoint shape[]={{0,0},{0,24},{6,18},{12,29},{17,26},{11,16},{23,16}};
    SDL_Vertex vertices[7];SDL_FPoint outline[8];
    SDL_FColor color=c->down?(SDL_FColor){1,.88f,.1f,1}:(SDL_FColor){.65f,.65f,.65f,1};
    for(unsigned i=0;i<7;++i) {
        outline[i]=(SDL_FPoint){x+shape[i].x*sx,y+shape[i].y*sy};
        vertices[i]=(SDL_Vertex){.position=outline[i],.color=color};
    }
    outline[7]=outline[0];
    const int indices[]={0,1,2,0,2,6,2,3,4,2,4,5,2,5,6};
    SDL_RenderGeometry(renderer,NULL,vertices,7,indices,15);
    SDL_SetRenderDrawColor(renderer,0,0,0,255);
    SDL_RenderLines(renderer,outline,8);
}

void mph_input_render(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    mph_toolbar_render(im);
    if(in->enabled && im->screen->renderer) {
        SDL_Renderer *r=im->screen->renderer;float scale=SDL_GetWindowPixelDensity(im->screen->window);
        SDL_FRect border=im->screen->rect;border.x*=scale;border.y*=scale;border.w*=scale;border.h*=scale;
        SDL_SetRenderDrawColor(r,in->suspended?240:25,in->suspended?165:120,in->suspended?35:255,255);
        SDL_RenderRect(r,&border);border.x+=1;border.y+=1;border.w-=2;border.h-=2;SDL_RenderRect(r,&border);
        if(in->suspended && in->locator_until>SDL_GetTicks()) {
            float x,y;SDL_GetMouseState(&x,&y);x*=scale;y*=scale;
            for(int pass=0;pass<2;++pass) {
                SDL_SetRenderDrawColor(r,pass?255:0,pass?235:0,pass?30:0,255);
                float radius=(pass?26:28)*scale;
                for(int n=0;n<48;++n) {
                    float a=n*6.2831853f/48,b=(n+1)*6.2831853f/48;
                    SDL_RenderLine(r,x+cosf(a)*radius,y+sinf(a)*radius,x+cosf(b)*radius,y+sinf(b)*radius);
                }
                SDL_RenderLine(r,x-36*scale,y,x+36*scale,y);SDL_RenderLine(r,x,y-36*scale,x,y+36*scale);
            }
            mph_text_draw(r,x+32*scale,y+20*scale,"鼠标在这里",16*scale);
        }
    }
    if (!in->editing) {
        render_diagnostic_arrow(im);
        if(im->screen->video && im->screen->rect.w>0 && im->screen->renderer) {
            float scale=SDL_GetWindowPixelDensity(im->screen->window);
            char label[64];snprintf(label,sizeof(label),"投屏 FPS: %.0f",im->screen->paused?0:in->fps_value);
            float width=mph_text_width(im->screen->renderer,label,14*scale);
            float x=(im->screen->rect.x+im->screen->rect.w)*scale-width-8*scale;
            mph_text_draw(im->screen->renderer,fmaxf(0,x),im->screen->rect.y*scale+5*scale,label,14*scale);
        }
        return;
    }
    struct mph_mapper *m=&in->mapper;
    SDL_Renderer *renderer=im->screen->renderer;
    float scale=SDL_GetWindowPixelDensity(im->screen->window);
    SDL_FRect r=im->screen->rect;
    r.x*=scale;r.y*=scale;r.w*=scale;r.h*=scale;
    for (int i=-3;i<(int)m->count;++i) {
        if (i==-1) continue;
        float x=i==-3?m->look_x:i==-2?m->joy_x:m->bindings[i].x;
        float y=i==-3?m->look_y:i==-2?m->joy_y:m->bindings[i].y;
        float px=r.x+x*r.w,py=r.y+y*r.h;
        SDL_SetRenderDrawColor(renderer,i==in->selected?255:60,i==in->selected?200:230,160,255);
        if (i==-2) {
            float radius=(i==-3?m->look_radius:m->joy_radius)*r.h;
            SDL_FRect zone={px-radius,py-radius,radius*2,radius*2}; SDL_RenderRect(renderer,&zone);
        }
        SDL_FRect point={px-13*scale,py-13*scale,26*scale,26*scale}; SDL_RenderFillRect(renderer,&point);
        char label[96],coordinates[32];
        if(i==-3) snprintf(label,sizeof(label),"视角起点");
        else if(i==-2) snprintf(label,sizeof(label),"移动 WASD");
        else {
            int k=m->bindings[i].key;
            const char *name=k>512?(k==513?"鼠标左键":k==515?"鼠标右键":k==514?"鼠标中键":k==516?"鼠标侧键1":"鼠标侧键2"):
                k==SDL_SCANCODE_SPACE?"空格":k==SDL_SCANCODE_RETURN?"回车":k==SDL_SCANCODE_TAB?"Tab":
                k?SDL_GetScancodeName(k):"未绑定";
            snprintf(label,sizeof(label),"%s",name);
        }
        snprintf(coordinates,sizeof(coordinates),"%d,%d",(int)lroundf(x*100),(int)lroundf(y*100));
        float font_height=16*scale;
        float label_width=fmaxf(mph_text_width(renderer,label,font_height),mph_text_width(renderer,coordinates,font_height));
        float tx=fmaxf(4*scale,fminf(r.x+r.w-label_width-4*scale,px+16*scale));
        float ty=fmaxf(r.y,fminf(r.y+r.h-44*scale,py-18*scale));
        mph_text_draw(renderer,tx,ty,label,font_height);
        mph_text_draw(renderer,tx,ty+22*scale,coordinates,font_height);
    }
}


/* Rendering and hit testing share stable action IDs and the same visible list. */
unsigned mph_toolbar_actions(const struct mph_input *in,int *actions) {
    unsigned n=0;
    actions[n++]=0;actions[n++]=1;
    if(in->editing) {
        const int editor[]={6,7,13,8,9,10};
        for(unsigned i=0;i<sizeof(editor)/sizeof(editor[0]);++i) actions[n++]=editor[i];
    }
    actions[n++]=14;
    if(in->toolbar_scale_expanded) {
        actions[n++]=2;actions[n++]=3;actions[n++]=4;
    }
    actions[n++]=11;actions[n++]=12;actions[n++]=15;
    return n;
}

void mph_toolbar_render(struct sc_input_manager *im) {
    if(!im->screen->video) return;
    SDL_Renderer *r=im->mph.toolbar_renderer;
    if(!r) return;
    mph_toolbar_sync(im);
    int width,height; SDL_GetWindowSize(im->mph.toolbar_window,&width,&height);
    float scale=SDL_GetWindowPixelDensity(im->mph.toolbar_window);
    SDL_SetRenderScale(r,scale,scale);
    float x=0;
    SDL_SetRenderDrawColor(r,20,28,43,255);SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r,20,28,43,255);
    SDL_FRect bg={x,0,MPH_TOOLBAR_WIDTH,height};SDL_RenderFillRect(r,&bg);
    char hotkey[64];snprintf(hotkey,sizeof(hotkey),"开关快捷键：%s",SDL_GetScancodeName(im->mph.mapper.toggle_key));
    enum mph_recording_state rec=mph_recording_state(im->mph.recording);
    const char *record_label=rec==MPH_REC_STARTING?"停止录制（等待关键帧）":rec==MPH_REC_ACTIVE?"停止录制":rec==MPH_REC_STOPPING || rec==MPH_REC_DONE?"正在保存录制…":"开始录制（含手机声音）";
    const char *labels[]={im->mph.enabled?(im->mph.suspended?"映射暂停 (F1)":"映射开启 (F1)"):"映射关闭 (F1)",
        im->mph.editing?"布局编辑：收起并保存":"编辑布局 (F9)",
        "放大画面 (小键盘+)", "缩小画面 (小键盘-)", "适配屏幕 (小键盘*)", hotkey,
        im->mph.assigning?"请按目标键…":"修改选中按键 (F2)", "删除选中点位", "放弃本次编辑",
        "保存并打开游戏配置", "保存并新建游戏配置", "投屏画质设置",
        im->mph.phone_off_requested?"手机点亮 (小键盘/)":"手机熄屏 (小键盘/)",
        "保存编辑并退出 (F5)",
        im->mph.toolbar_scale_expanded?"窗口尺寸缩放 −":"窗口尺寸缩放 +",record_label};
    int actions[MPH_TOOLBAR_ROWS];
    int rows=(int)mph_toolbar_actions(&im->mph,actions);
    int step=MIN(34,MAX(18,(height-56)/rows));
    for(int i=0;i<rows;++i) {
        int action=actions[i];
        bool child=(action>=6 && action<=10) || action==13 || (action>=2 && action<=4);
        float inset=child?18:10;
        SDL_FRect button={x+inset,48+i*step,MPH_TOOLBAR_WIDTH-inset-10,step-6};
        if(action==0 && im->mph.enabled)
            SDL_SetRenderDrawColor(r,im->mph.suspended?180:22,im->mph.suspended?112:105,im->mph.suspended?15:210,255);
        else SDL_SetRenderDrawColor(r,43,57,80,255);
        SDL_RenderFillRect(r,&button);
        float font=child?14:16;
        float text_width=mph_text_width(r,labels[action],font);
        float available=button.w-12;
        if(text_width>available) font*=available/text_width;
        SDL_SetRenderDrawColor(r,232,240,250,255);
        mph_text_draw(r,x+inset+6,48+i*step+(step-6-font)/2,labels[action],font);
    }
    SDL_SetRenderDrawColor(r,65,220,185,255);mph_text_draw(r,x+14,12,"手游操控助手",18);
    SDL_SetRenderScale(r,1,1);
    if(im->mph.mcp_capture) {
        SDL_DestroySurface(im->mph.mcp_toolbar);
        im->mph.mcp_toolbar=SDL_RenderReadPixels(r,NULL);
    }
    SDL_RenderPresent(r);
}

static void toolbar_action(struct sc_input_manager *im,int button) {
    struct mph_input *in=&im->mph;
    SDL_Event action; memset(&action,0,sizeof(action)); action.type=SDL_EVENT_KEY_DOWN;
    switch(button) {
        case 0: break; /* state indicator only; F1 is the sole manual toggle */
        case 1: action.key.scancode=in->editing?SDL_SCANCODE_F5:SDL_SCANCODE_F9;editor_handle(im,&action);break;
        case 2: case 3: {
            float factor=button==2?1.15f:1/1.15f;
            if(!(SDL_GetWindowFlags(im->screen->window)&SDL_WINDOW_FULLSCREEN))
                {
                int mw,mh;SDL_GetWindowSize(im->screen->window,&mw,&mh);
                float ratio=(float)im->screen->content_size.width/im->screen->content_size.height;
                if(!isfinite(ratio) || ratio<=0) ratio=(float)mw/mh;
                int new_h=(int)fmaxf(120,mh*factor);
                SDL_SetWindowSize(im->screen->window,(int)(new_h*ratio),new_h);
            }
            break;
        }
        case 4: {
            if(!im->screen->frame_size.height) break;
            SDL_Rect bounds;
            if(!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(im->screen->window),&bounds)) break;
            float aspect=(float)im->screen->frame_size.width/im->screen->frame_size.height;
            int h=(int)fminf(bounds.h-100,(bounds.w-MPH_TOOLBAR_WIDTH-100)/aspect);
            if(h>0) {
                SDL_Window *window=im->screen->window;
                SDL_SetWindowSize(window,(int)(h*aspect),h);
                int width,height;SDL_GetWindowSize(window,&width,&height);
                /* Center the video window alone; the external toolbar follows. */
                SDL_SetWindowPosition(window,bounds.x+(bounds.w-width)/2,
                                      bounds.y+(bounds.h-height)/2);
            }
            break;
        }
        case 5: break; /* fixed F1 */
        case 6: action.key.scancode=SDL_SCANCODE_F2;editor_handle(im,&action);break;
        case 7: editor_delete(in);break;
        case 8: editor_discard(im);break;
        case 9: case 10:
            action.key.scancode=SDL_SCANCODE_F5;editor_handle(im,&action);
            if(!in->editing) {
                profile_dialog(im,button==10);
                action.key.scancode=SDL_SCANCODE_F9;editor_handle(im,&action);
            }
            break;
        case 11: mph_quality_dialog(im);break;
        case 12: mph_input_set_display_power(im,in->phone_off_requested);break;
        case 13: action.key.scancode=SDL_SCANCODE_F5;editor_handle(im,&action);break;
        case 14: in->toolbar_scale_expanded=!in->toolbar_scale_expanded;break;
        case 15: {
            enum mph_recording_state state=mph_recording_state(in->recording);
            if(state==MPH_REC_STARTING || state==MPH_REC_ACTIVE) {
                record_event(im,"record_stop",0,0,0,0,-1);mph_recording_stop(in->recording);
            } else if(state==MPH_REC_IDLE) {
                if(!mph_recording_start(in->recording))SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"无法开始录制",SDL_GetError(),im->screen->window);
                else {
                    record_event(im,"record_start",0,0,0,0,-1);
                    for(unsigned i=0;i<in->mapper.count;++i) {
                        const struct mph_binding *b=&in->mapper.bindings[i];
                        record_event(im,"binding",b->key,(int)i+2,b->x,b->y,-1);
                    }
                }
            }
            break;
        }
    }
    mph_toolbar_sync(im);
    redraw(im);
}

bool mph_toolbar_handle(struct sc_input_manager *im,const SDL_Event *e) {
    if(!im->screen->video) return false;
    struct mph_input *in=&im->mph;
    if(!in->toolbar_window || SDL_GetWindowFromEvent(e)!=in->toolbar_window) return false;
    if(e->type==SDL_EVENT_WINDOW_EXPOSED || e->type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {mph_toolbar_render(im);return true;}
    if(in->enabled && !in->suspended) return true;
    int width,height;SDL_GetWindowSize(in->toolbar_window,&width,&height);
    int actions[MPH_TOOLBAR_ROWS];
    int rows=(int)mph_toolbar_actions(in,actions);
    int step=MIN(34,MAX(18,(height-56)/rows));
    float x=-1,y=0;
    if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN || e->type==SDL_EVENT_MOUSE_BUTTON_UP) {x=e->button.x;y=e->button.y;}
    else if(e->type==SDL_EVENT_MOUSE_MOTION) {x=e->motion.x;y=e->motion.y;}
    else if(e->type==SDL_EVENT_MOUSE_WHEEL) {SDL_GetMouseState(&x,&y);}
    else return true;
    if(x<10 || x>=width-10) {in->toolbar_pressed=-1;return true;}
    if(im->mouse_buttons_state || im->vfinger_down) return true;
    if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button==SDL_BUTTON_LEFT) {
        int row=y>=48?(int)(y-48)/step:-1;
        in->toolbar_pressed=row>=0 && row<rows && ((int)y-48)%step<step-6?actions[row]:-1;
        return true;
    }
    if(e->type!=SDL_EVENT_MOUSE_BUTTON_UP || e->button.button!=SDL_BUTTON_LEFT) return true;
    /* Do not turn the release of a point dragged over the toolbar into a click. */
    if(in->dragging) {in->dragging=false;return true;}
    int row=(int)(y-48)/step;
    int pressed=in->toolbar_pressed;in->toolbar_pressed=-1;
    if(y<48 || row>=rows || ((int)y-48)%step>=step-6) return true;
    int button=actions[row];
    if(button!=pressed) return true;
    if(in->enabled && button!=12 && button!=15) return true;
    toolbar_action(im,button);
    return true;
}

bool mph_input_handle(struct sc_input_manager *im, const SDL_Event *e) {
    if(e->type==SC_EVENT_MPH_MCP_TICK) {mph_mcp_poll(im);return true;}
    if(e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_MOUSE_BUTTON_DOWN ||
       e->type==SDL_EVENT_WINDOW_FOCUS_LOST || e->type==SC_EVENT_DEVICE_DISCONNECTED)
        mph_mcp_cancel(im);
    switch(e->type) {
        case SDL_EVENT_MOUSE_MOTION: record_event(im,"mouse",0,0,e->motion.xrel,e->motion.yrel,-1);break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            record_event(im,"key",e->key.scancode,e->type==SDL_EVENT_KEY_DOWN,e->key.repeat,0,-1);break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            record_event(im,"button",e->button.button,e->type==SDL_EVENT_MOUSE_BUTTON_DOWN,e->button.x,e->button.y,-1);break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_MINIMIZED: case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT: case SC_EVENT_DEVICE_DISCONNECTED:
            record_event(im,"window",e->type,0,0,0,-1);break;
    }
    if(e->type==SC_EVENT_MPH_FPS_TICK) {
        SDL_SetAtomicInt(&fps_tick_pending,0);
        mph_recording_poll(im->mph.recording,im->screen->window);
        record_event(im,"heartbeat",0,0,0,0,-1);
        Uint64 now=SDL_GetTicksNS();
        if(!im->mph.video_ns || now-im->mph.video_ns>1000000000) {
            im->mph.fps_value=0;im->mph.fps_start_ns=0;im->mph.fps_frames=0;
        }
        redraw(im);return true;
    }
    if(e->type==SC_EVENT_MPH_LOOK_TICK) {
        SDL_SetAtomicInt(&look_tick_pending,0);
        Uint64 now=SDL_GetTicks();
        if(im->mph.enabled) {
            if(!im->mph.suspended && !SDL_GetWindowRelativeMouseMode(im->screen->window))mapping_suspend(im);
            mapping_resume(im);
            if(im->mph.locator_until>now && now>=im->mph.locator_redraw_at) {
                im->mph.locator_redraw_at=now+33;redraw(im);
            }
        }
        if(im->mph.enabled && !im->mph.suspended && im->screen->frame_size.height &&
           (!im->mph.look_last_tick || now-im->mph.look_last_tick>=8)) {
            im->mph.look_last_tick=now;
            im->mph.look_action=-1;im->mph.look_accepted=false;
            mph_look_tick(&im->mph.mapper,(float)im->screen->frame_size.width/im->screen->frame_size.height);
            look_trace_record(&im->mph);
            if(im->mph.diagnostic_arrow && im->mph.look_action>=0 && now>=im->mph.diagnostic_redraw_at) {
                im->mph.diagnostic_redraw_at=now+16;redraw(im);
            }
        }
        return true;
    }
    if(im->mph.toolbar_window && SDL_GetWindowFromEvent(e)==im->mph.toolbar_window) {mph_toolbar_handle(im,e);return true;}
    struct mph_input *in=&im->mph;
    if(e->type==SDL_EVENT_WINDOW_FOCUS_LOST || e->type==SDL_EVENT_WINDOW_MINIMIZED) {
        in->focus_lost=true;in->f1_down=false;in->locator_until=0;
        mapping_suspend(im);if(in->enabled)release_normal_mouse(im);
        if(in->enabled)look_trace_save(in);
    } else if(e->type==SDL_EVENT_WINDOW_FOCUS_GAINED) {
        in->focus_lost=false;mapping_resume(im);
    }
    if(e->type==SDL_EVENT_WINDOW_CLOSE_REQUESTED || e->type==SDL_EVENT_QUIT || e->type==SC_EVENT_DEVICE_DISCONNECTED)mph_input_stop(im);
    if (mph_toolbar_handle(im,e)) return true;
    if((e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_KEY_UP) &&
       e->key.scancode==SDL_SCANCODE_NUMLOCKCLEAR && im->screen->video) {
        if(in->enabled && !in->focus_lost && e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            in->diagnostic_arrow=!in->diagnostic_arrow;
            record_event(im,"arrow_toggle",0,0,0,0,-1);
            redraw(im);
        }
        return true;
    }
    if(im->screen->video && (e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_KEY_UP)) {
        int action=keypad_action(e->key.scancode);
        if(action>=0) {
            if(e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat && !in->focus_lost)
                toolbar_action(im,action);
            return true; /* Tool keys never become game touches or editor bindings. */
        }
    }
    if (editor_handle(im,e)) return true;
    if ((e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_KEY_UP) && (int)e->key.scancode==in->mapper.toggle_key) {
        if(e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat && !in->f1_down && !in->focus_lost) {
            in->f1_down=true;in->f1_pressed_at=SDL_GetTicks();in->f1_began_enabled=in->enabled;in->f1_used_mouse=false;
            if(!in->enabled)start(im);
            else {
                mapping_suspend(im);
                SDL_FRect r=im->screen->rect;
                SDL_WarpMouseInWindow(im->screen->window,r.x+r.w/2,r.y+r.h/2);
                in->locator_until=SDL_GetTicks()+900;in->locator_redraw_at=0;redraw(im);
            }
        } else if(e->type==SDL_EVENT_KEY_UP && in->f1_down) {
            in->f1_down=false;
            if(in->f1_began_enabled) {
                if(SDL_GetTicks()-in->f1_pressed_at<250 && !in->f1_used_mouse)mph_input_stop(im);
                else mapping_resume(im);
            }
        }
        return true;
    }
    if(in->enabled && in->suspended) {
        if(in->focus_lost)return true;
        if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN || e->type==SDL_EVENT_MOUSE_WHEEL)in->f1_used_mouse=true;
        /* Normal mouse events reach scrcpy. Keys cannot leave half a key pair
         * on Android when the temporary mode ends. Resume runs on the next tick. */
        if(e->type==SDL_EVENT_KEY_DOWN || e->type==SDL_EVENT_KEY_UP || e->type==SDL_EVENT_TEXT_INPUT)return true;
        return false;
    }
    if (!im->mph.enabled) {
        if (e->type==SDL_EVENT_KEY_DOWN && e->key.scancode==SDL_SCANCODE_F6 && !e->key.repeat && im->screen->video) {
            float x,y; SDL_GetMouseState(&x,&y);
            struct sc_point p=sc_screen_convert_window_to_frame_coords(im->screen,x,y);
            struct sc_size sz=im->screen->frame_size;
            if (sz.width>1 && sz.height>1 && p.x>=0 && p.y>=0 && p.x<sz.width && p.y<sz.height)
                LOGI("MPH coordinate: %.5f %.5f",(double)p.x/(sz.width-1),(double)p.y/(sz.height-1));
            return true;
        }
        return false;
    }
    struct mph_mapper *m=&im->mph.mapper;
    float aspect=(float)im->screen->frame_size.width/im->screen->frame_size.height;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            bool down=e->type==SDL_EVENT_KEY_DOWN;
            if(e->key.scancode==SDL_SCANCODE_GRAVE) {
                if(down && !e->key.repeat) {
                    record_event(im,"refresh",0,0,m->look_x,m->look_y,-1);
                    mph_look_refresh(m);
                    look_trace_record(in);
                    if(in->diagnostic_arrow) redraw(im);
                    LOGI("MPH: manual look drag refresh (grave key)");
                }
                return true;
            }
            bool bound=false;
            for (unsigned i=0;i<m->count;++i) if(m->bindings[i].key==(int)e->key.scancode) bound=true;
            if (e->key.scancode==SDL_SCANCODE_ESCAPE && !bound) {
                if(!e->key.repeat)escape_send(im,down);
                return true; /* never enter the legacy key/capture path */
            }
            if (e->key.repeat || reserved_key(e->key.scancode)) return true;
            if (bound) {mph_button(m,e->key.scancode,down);return true;}
            int direction=-1;
            switch (e->key.scancode) {
                case SDL_SCANCODE_W: direction=0; break;
                case SDL_SCANCODE_A: direction=1; break;
                case SDL_SCANCODE_S: direction=2; break;
                case SDL_SCANCODE_D: direction=3; break;
                default: break;
            }
            if (direction>=0) mph_direction(m,direction,down,aspect);
            else mph_button(m,e->key.scancode,down);
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (e->motion.which != SDL_TOUCH_MOUSEID) {
                ++im->mph.look_mouse_events;
                im->mph.look_mouse_x+=e->motion.xrel;im->mph.look_mouse_y+=e->motion.yrel;
                mph_look(m,e->motion.xrel,e->motion.yrel,aspect);
            }
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e->button.which != SDL_TOUCH_MOUSEID) mph_button(m,512+e->button.button,e->type==SDL_EVENT_MOUSE_BUTTON_DOWN);
            return true;
        case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION:
            return true;
        default: return false;
    }
}

#include "mph_mcp.inc"
