/* SPDX-License-Identifier: Apache-2.0 */
/* Run against real SDL with a synthetic screen; no Android input is sent. */
#include "input_manager.h"
#include "screen.h"
#include "mph_text.h"
#include "mph_quality.h"
#include "../mph_quality_config.h"
#include "events.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dwmapi.h>
static unsigned injections, back_injections, power_injections;
static bool last_power;
static bool allow_injection;
static bool reject_touch;
static int last_touch_action;
/* The invalid/unassigned-save cases must not block automation on a native UI. */
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags,const char *title,
                              const char *message,SDL_Window *window) {
    (void)window;
    assert(flags==SDL_MESSAGEBOX_ERROR && title && message && *message);
    return true;
}
bool sc_controller_push_msg(struct sc_controller *c,const struct sc_control_msg *m) {
    (void)c; assert(allow_injection);
    if(m->type==SC_CONTROL_MSG_TYPE_SET_DISPLAY_POWER) { ++power_injections;last_power=m->set_display_power.on;return true; }
    if(m->type==SC_CONTROL_MSG_TYPE_INJECT_KEYCODE) { assert(m->inject_keycode.keycode==AKEYCODE_BACK); ++back_injections; return true; }
    assert(m->type==SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT);
    assert(m->inject_touch_event.pointer_id>=0x4d504800ULL);
    last_touch_action=m->inject_touch_event.action;
    if(reject_touch)return false;
    ++injections; return true;
}
struct sc_point sc_screen_convert_window_to_frame_coords(struct sc_screen *s,int32_t x,int32_t y) {
    return (struct sc_point){x*s->frame_size.width/1000,y*s->frame_size.height/600};
}
static void key(struct sc_input_manager *im,SDL_Scancode k) {
    SDL_Event e; memset(&e,0,sizeof(e)); e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=k;mph_input_handle(im,&e);
    if(k==SDL_SCANCODE_F1) {e.type=SDL_EVENT_KEY_UP;mph_input_handle(im,&e);}
}
static void mcp_command(struct sc_input_manager *im,const char *command) {
    FILE *f=fopen("work/mcp-native-test/request.txt","wb");assert(f);
    fprintf(f,"0123456789abcdef0123456789abcdef\r\n%s",command);assert(!fclose(f));
    im->mph.mcp_poll_at=0;
    SDL_Event e={0};e.type=SC_EVENT_MPH_MCP_TICK;assert(mph_input_handle(im,&e));
}
static void click(struct sc_input_manager *im,int x,int y) {
    SDL_Event e; memset(&e,0,sizeof(e)); e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=1;e.button.x=x;e.button.y=y;
    mph_input_handle(im,&e);e.type=SDL_EVENT_MOUSE_BUTTON_UP;mph_input_handle(im,&e);
}
static void toolbar(struct sc_input_manager *im,int action) {
    mph_toolbar_sync(im);
    int w,h;SDL_GetWindowSize(im->mph.toolbar_window,&w,&h);
    int actions[MPH_TOOLBAR_ROWS],row=-1;
    int rows=(int)mph_toolbar_actions(&im->mph,actions);
    for(int i=0;i<rows;++i) if(actions[i]==action) row=i;
    assert(row>=0);
    int step=MIN(34,MAX(18,(h-56)/rows));
    SDL_Event e={0};e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID=SDL_GetWindowID(im->mph.toolbar_window);
    e.button.button=1;e.button.x=100;e.button.y=48+row*step+5;
    assert(mph_input_handle(im,&e));e.type=SDL_EVENT_MOUSE_BUTTON_UP;
    assert(mph_input_handle(im,&e));
}
static void check_arrow(struct sc_input_manager *im,float nx,float ny,bool visible) {
    SDL_Renderer *r=im->screen->renderer;
    SDL_SetRenderDrawColor(r,35,45,60,255);SDL_RenderClear(r);mph_input_render(im);
    SDL_Surface *shot=SDL_RenderReadPixels(r,NULL);assert(shot);
    SDL_FRect area=im->screen->rect;
    float scale=SDL_GetWindowPixelDensity(im->screen->window);
    Uint8 red,green,blue,alpha;
    assert(SDL_ReadSurfacePixel(shot,(int)((area.x+nx*area.w+3)*scale),
                               (int)((area.y+ny*area.h+8)*scale),&red,&green,&blue,&alpha));
    if(visible) {
        assert(red>240 && green>200 && blue<60);
        assert(SDL_SavePNG(shot,"work/diagnostic-arrow-test.png"));
    } else assert(red==35 && green==45 && blue==60);
    SDL_DestroySurface(shot);
}
int main(void) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER,"software");
    assert(SDL_Init(SDL_INIT_VIDEO));
    struct sc_screen s={0}; struct sc_input_manager *im=&s.im;
    s.window=SDL_CreateWindow("MPH editor integration",1000,600,SDL_WINDOW_HIDDEN);
    assert(s.window);s.renderer=SDL_CreateRenderer(s.window,NULL);assert(s.renderer);
    s.video=true;s.frame_size=(struct sc_size){2000,1200};s.rect=(SDL_FRect){0,0,1000,600};
    im->screen=&s;mph_input_init(im);
    if(getenv("MPH_TEST_QUALITY_GUI")) {SDL_ShowWindow(s.window);mph_quality_dialog(im);SDL_HideWindow(s.window);}
    assert(!im->mph.enabled && !im->mph.editing); /* no startup chooser or capture */
    int actions[MPH_TOOLBAR_ROWS];
    assert(mph_toolbar_actions(&im->mph,actions)==6);
    assert(actions[0]==0 && actions[1]==1 && actions[2]==14 && actions[3]==11 && actions[4]==12);
    toolbar(im,14);assert(im->mph.toolbar_scale_expanded);
    assert(mph_toolbar_actions(&im->mph,actions)==9 && actions[3]==2 && actions[5]==4);
    toolbar(im,14);assert(!im->mph.toolbar_scale_expanded);
    toolbar(im,1);assert(im->mph.editing);
    assert(mph_toolbar_actions(&im->mph,actions)==12 && actions[2]==6 && actions[4]==13);
    toolbar(im,13);assert(!im->mph.editing);
    assert(mph_toolbar_actions(&im->mph,actions)==6);
    mph_toolbar_render(im);
    SDL_Surface *bar_shot=SDL_RenderReadPixels(im->mph.toolbar_renderer,NULL);assert(bar_shot);
    assert(SDL_SavePNG(bar_shot,"work/toolbar-default-test.png"));SDL_DestroySurface(bar_shot);
    /* Count video frames, not redraws; thirty intervals over 0.5 s = 60 FPS. */
    mph_input_note_frame(im,1000000000);
    for(unsigned n=1;n<=30;++n) mph_input_note_frame(im,1000000000+(Uint64)n*500000000/30);
    assert(im->mph.fps_value>59.9f && im->mph.fps_value<60.1f);
    SDL_SetRenderDrawColor(s.renderer,35,45,60,255);SDL_RenderClear(s.renderer);
    mph_input_render(im);
    SDL_Surface *fps_shot=SDL_RenderReadPixels(s.renderer,NULL);assert(fps_shot);
    assert(SDL_SavePNG(fps_shot,"work/fps-test.png"));SDL_DestroySurface(fps_shot);
    im->mph.video_ns=0;SDL_Event idle={0};idle.type=SC_EVENT_MPH_FPS_TICK;
    assert(mph_input_handle(im,&idle));assert(im->mph.fps_value==0);
    wchar_t quality_path[4096];mph_quality_path(quality_path,L"quality-unit-test.ini");
    struct mph_quality_settings q={1440,90,20,1};assert(mph_quality_write(quality_path,q));
    struct mph_quality_settings read=mph_quality_read(quality_path);
    assert(read.size==1440 && read.fps==90 && read.mbps==20 && read.codec==1);
    q.mbps=0;assert(!mph_quality_write(quality_path,q));
    read=mph_quality_read(quality_path);assert(read.mbps==20);DeleteFileW(quality_path);
    key(im,SDL_SCANCODE_F9);assert(im->mph.editing);
    mph_toolbar_render(im);
    bar_shot=SDL_RenderReadPixels(im->mph.toolbar_renderer,NULL);assert(bar_shot);
    assert(SDL_SavePNG(bar_shot,"work/toolbar-editor-test.png"));SDL_DestroySurface(bar_shot);
    unsigned original=im->mph.mapper.count;
    click(im,400,200);assert(im->mph.assigning);key(im,SDL_SCANCODE_Q);
    assert(im->mph.mapper.count==original+1);
    assert(im->mph.mapper.bindings[original].key==SDL_SCANCODE_Q);
    SDL_Event e; memset(&e,0,sizeof(e)); e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=1;e.button.x=400;e.button.y=200;
    mph_input_handle(im,&e);
    e.type=SDL_EVENT_MOUSE_MOTION;e.motion.x=500;e.motion.y=300;mph_input_handle(im,&e);
    e.type=SDL_EVENT_MOUSE_BUTTON_UP;mph_input_handle(im,&e);
    assert(im->mph.mapper.bindings[original].x==.5f && im->mph.mapper.bindings[original].y==.5f);
    key(im,SDL_SCANCODE_F2);key(im,SDL_SCANCODE_E);
    assert(im->mph.mapper.bindings[original].key==SDL_SCANCODE_E);
    key(im,SDL_SCANCODE_F5);assert(!im->mph.editing);
    key(im,SDL_SCANCODE_F9);assert(im->mph.editing);
    assert(im->mph.mapper.bindings[original].x==.5f);
    click(im,500,300);toolbar(im,7);assert(im->mph.mapper.count==original);
    key(im,SDL_SCANCODE_F5);assert(!im->mph.editing); /* overwrite existing */
    key(im,SDL_SCANCODE_F9);assert(im->mph.mapper.count==original);
    click(im,400,200);key(im,SDL_SCANCODE_Q);toolbar(im,8);
    key(im,SDL_SCANCODE_F9);assert(im->mph.mapper.count==original); /* discard */
    SDL_SetRenderDrawColor(s.renderer,35,45,60,255);SDL_RenderClear(s.renderer);
    mph_input_render(im);
    SDL_Surface *shot=SDL_RenderReadPixels(s.renderer,NULL);assert(shot);
    Uint8 red,green,blue,alpha;
    assert(SDL_ReadSurfacePixel(shot,990,5,&red,&green,&blue,&alpha));
    assert(red==35 && green==45 && blue==60); /* no opaque help over top-right */
    assert(SDL_SavePNG(shot,"work/editor-test.png"));SDL_DestroySurface(shot);
    toolbar(im,8);
    assert(im->mph.toolbar_window && im->mph.toolbar_window!=s.window);
    assert(SDL_GetWindowFlags(im->mph.toolbar_window)&SDL_WINDOW_NOT_FOCUSABLE);
    int mw,mh;SDL_GetWindowSize(s.window,&mw,&mh);assert(mw==1000 && mh==600);
    SDL_Event bar;memset(&bar,0,sizeof(bar));bar.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    bar.button.windowID=SDL_GetWindowID(im->mph.toolbar_window);bar.button.button=1;bar.button.x=100;bar.button.y=48+5*34+5;
    assert(mph_input_handle(im,&bar));bar.type=SDL_EVENT_MOUSE_BUTTON_UP;assert(mph_input_handle(im,&bar));
    assert(!im->mph.enabled);
    assert(im->mph.mapper.toggle_key==SDL_SCANCODE_F1);
    assert(!im->mph.choosing_hotkey);
    char original_path[4096];snprintf(original_path,sizeof(original_path),"%s",im->mph.profile_path);
    const char *clone=SDL_getenv("MPH_TEST_PROFILE");assert(clone);
    assert(mph_input_select_profile(im,clone,true));assert(!im->mph.enabled);
    assert(!strcmp(im->mph.profile_path,clone));
    assert(mph_input_select_profile(im,original_path,false));
    assert(mph_input_select_profile(im,clone,false)); /* Unicode path round-trip */
    assert(im->mph.mapper.toggle_key==SDL_SCANCODE_F1);
    assert(!mph_input_select_profile(im,"work/missing/invalid.mph",false));
    assert(!strcmp(im->mph.profile_path,clone));
    assert(!mph_input_select_profile(im,"work/missing/invalid.mph",true));
    assert(!strcmp(im->mph.profile_path,clone));
    key(im,SDL_SCANCODE_F9);
    assert(!mph_input_select_profile(im,original_path,false)); /* preserve edits */
    toolbar(im,8);
    assert(mph_input_select_profile(im,original_path,false));
    allow_injection=true; im->controller=(struct sc_controller *)im;
    toolbar(im,12);assert(power_injections==1 && !last_power && im->mph.phone_off_requested);
    toolbar(im,12);assert(power_injections==2 && last_power && !im->mph.phone_off_requested);
    im->disconnected=true;toolbar(im,12);assert(power_injections==2 && !im->mph.phone_off_requested);im->disconnected=false;
    key(im,SDL_SCANCODE_F1); assert(im->mph.enabled);
    SDL_Event escape;memset(&escape,0,sizeof(escape));escape.type=SDL_EVENT_KEY_DOWN;
    escape.key.scancode=SDL_SCANCODE_ESCAPE;escape.key.key=SDLK_ESCAPE;
    assert(mph_input_handle(im,&escape));assert(im->mph.enabled && !im->mph.suspended);
    escape.type=SDL_EVENT_KEY_UP;assert(mph_input_handle(im,&escape));assert(im->mph.enabled && !im->mph.suspended);
    assert(back_injections==2);
    key(im,SDL_SCANCODE_F8);assert(im->mph.enabled); /* old toggle no longer toggles */
    key(im,SDL_SCANCODE_F9);assert(im->mph.enabled && !im->mph.editing);
    key(im,SDL_SCANCODE_F1);assert(!im->mph.enabled);
    key(im,SDL_SCANCODE_F1);assert(im->mph.enabled);
    key(im,SDL_SCANCODE_W);
    e.type=SDL_EVENT_MOUSE_MOTION;e.motion.xrel=10;e.motion.yrel=5;mph_input_handle(im,&e);
    SDL_Event tick={0};tick.type=SC_EVENT_MPH_LOOK_TICK;assert(mph_input_handle(im,&tick));
    unsigned tick_injections=injections;
    assert(mph_input_handle(im,&tick));assert(injections==tick_injections);
    SDL_Delay(10);assert(mph_input_handle(im,&tick));assert(injections==tick_injections+1);
    key(im,SDL_SCANCODE_SPACE);
    assert(im->mph.mapper.contacts[0].down && im->mph.mapper.contacts[1].down && im->mph.mapper.contacts[2].down);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST;mph_input_handle(im,&e);assert(im->mph.enabled && im->mph.suspended);
    assert(im->mph.look_timer && !im->mph.mapper.look_pending_x && !im->mph.mapper.look_pending_y);
    assert(im->mph.look_trace_count==2);
    assert(im->mph.look_trace[0].mouse_events==1 && im->mph.look_trace[0].action==MPH_DOWN);
    assert(im->mph.look_trace[1].action==MPH_MOVE);
    assert(im->mph.look_trace[1].ns>im->mph.look_trace[0].ns);
    FILE *trace=fopen("work/MobilePlayHelper-look.csv","r");assert(trace);
    char trace_header[512];assert(fgets(trace_header,sizeof(trace_header),trace));
    assert(strstr(trace_header,"screen-drag-v1"));fclose(trace);
    for(unsigned i=0;i<MPH_CONTACTS;++i) assert(!im->mph.mapper.contacts[i].down);
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; assert(mph_input_handle(im,&e));
    assert(injections>=8);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mph_input_handle(im,&e);
    assert(im->mph.enabled && !im->mph.suspended);
    assert(SDL_GetWindowRelativeMouseMode(s.window));
    /* Capture loss is repaired without forgetting the enabled state. */
    SDL_SetWindowRelativeMouseMode(s.window,false);
    mph_input_handle(im,&tick);assert(!im->mph.suspended && SDL_GetWindowRelativeMouseMode(s.window));
    SDL_Event hold={0};hold.type=SDL_EVENT_KEY_DOWN;hold.key.scancode=SDL_SCANCODE_F1;
    mph_input_handle(im,&hold);assert(im->mph.suspended && !SDL_GetWindowRelativeMouseMode(s.window));
    assert(im->mph.locator_until>SDL_GetTicks());
    SDL_SetRenderDrawColor(s.renderer,35,45,60,255);SDL_RenderClear(s.renderer);mph_input_render(im);
    SDL_Surface *locator=SDL_RenderReadPixels(s.renderer,NULL);assert(locator);
    assert(SDL_SavePNG(locator,"work/cursor-locator-test.png"));SDL_DestroySurface(locator);
    hold.key.repeat=true;mph_input_handle(im,&hold);assert(im->mph.suspended);
    SDL_Event normal={0};normal.type=SDL_EVENT_MOUSE_BUTTON_DOWN;normal.button.button=SDL_BUTTON_LEFT;
    assert(!mph_input_handle(im,&normal));assert(im->mph.f1_used_mouse);
    normal.type=SDL_EVENT_MOUSE_BUTTON_UP;assert(!mph_input_handle(im,&normal));
    im->mph.f1_pressed_at=SDL_GetTicks()-300;
    im->mouse_buttons_state=1; /* Do not recapture halfway through an ordinary click. */
    hold.type=SDL_EVENT_KEY_UP;hold.key.repeat=false;mph_input_handle(im,&hold);
    assert(im->mph.enabled && im->mph.suspended);
    im->mouse_buttons_state=0;mph_input_handle(im,&tick);
    assert(im->mph.enabled && !im->mph.suspended && !im->mph.locator_until);
    key(im,SDL_SCANCODE_F1);assert(!im->mph.enabled);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mph_input_handle(im,&e);assert(!im->mph.enabled);
    key(im,SDL_SCANCODE_F1);assert(im->mph.enabled);
    hold.type=SDL_EVENT_KEY_DOWN;mph_input_handle(im,&hold);assert(im->mph.f1_down);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST;mph_input_handle(im,&e);
    assert(im->mph.enabled && im->mph.suspended && !im->mph.f1_down);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mph_input_handle(im,&e);assert(!im->mph.suspended);
    hold.type=SDL_EVENT_KEY_UP;mph_input_handle(im,&hold);assert(im->mph.enabled);
    key(im,SDL_SCANCODE_F1);assert(!im->mph.enabled);
    /* Ordinary keys, including Esc/Delete/punctuation/W, are editable bindings. */
    key(im,SDL_SCANCODE_F9);assert(im->mph.editing);
    click(im,970,15);assert(im->mph.assigning);
    unsigned top=im->mph.mapper.count-1;
    for (int k=SDL_SCANCODE_F1;k<=SDL_SCANCODE_F12;++k) {
        key(im,(SDL_Scancode)k);
        assert(im->mph.editing && !im->mph.mapper.bindings[top].key);
    }
    SDL_Scancode ordinary[]={SDL_SCANCODE_DELETE,SDL_SCANCODE_EQUALS,
        SDL_SCANCODE_LEFTBRACKET,SDL_SCANCODE_W,SDL_SCANCODE_ESCAPE};
    for (unsigned i=0;i<sizeof(ordinary)/sizeof(ordinary[0]);++i) {
        key(im,SDL_SCANCODE_F2);key(im,ordinary[i]);
        assert(im->mph.editing && im->mph.mapper.bindings[top].key==(int)ordinary[i]);
    }
    key(im,SDL_SCANCODE_F9);assert(im->mph.editing); /* never save or toggle out */
    key(im,SDL_SCANCODE_F5);assert(!im->mph.editing);
    for (int i=0;i<3;++i) {
        key(im,SDL_SCANCODE_F9);assert(im->mph.editing);
        assert(im->mph.mapper.bindings[top].key==SDL_SCANCODE_ESCAPE);
        assert(im->mph.mapper.bindings[top].x==.97f);
        assert(im->mph.mapper.bindings[top].y==.025f);
        key(im,SDL_SCANCODE_F5);assert(!im->mph.editing);
    }
    key(im,SDL_SCANCODE_F1);assert(im->mph.enabled);
    unsigned before=injections;
    escape.type=SDL_EVENT_KEY_DOWN;assert(mph_input_handle(im,&escape));
    assert(im->mph.enabled && injections==before+1);
    assert(im->mph.mapper.contacts[top+2].down);
    escape.type=SDL_EVENT_KEY_UP;assert(mph_input_handle(im,&escape));
    assert(im->mph.enabled && injections==before+2);
    assert(!im->mph.mapper.contacts[top+2].down);
    key(im,SDL_SCANCODE_F1);assert(!im->mph.enabled);
    /* Esc has no delayed refresh; grave refreshes immediately without a tick. */
    for(int bound=0;bound<2;++bound) {
        key(im,SDL_SCANCODE_F1);assert(im->mph.enabled);
        if(!bound) im->mph.mapper.count=0;
        escape.type=SDL_EVENT_KEY_DOWN;escape.key.repeat=false;mph_input_handle(im,&escape);
        escape.type=SDL_EVENT_KEY_UP;mph_input_handle(im,&escape);
        SDL_Delay(210);mph_input_handle(im,&tick);
        assert(!im->mph.mapper.contacts[1].down);
        SDL_Event refresh={0};refresh.type=SDL_EVENT_KEY_DOWN;refresh.key.scancode=SDL_SCANCODE_GRAVE;
        unsigned sent=injections;mph_input_handle(im,&refresh);
        assert(injections==sent+1 && im->mph.mapper.contacts[1].down);
        sent=injections;mph_input_handle(im,&refresh);
        assert(injections==sent+2 && im->mph.mapper.contacts[1].down);
        refresh.key.repeat=true;mph_input_handle(im,&refresh);
        refresh.type=SDL_EVENT_KEY_UP;refresh.key.repeat=false;mph_input_handle(im,&refresh);
        assert(injections==sent+2 && !im->mph.mapper.look_refresh_phase);
        e.type=SDL_EVENT_WINDOW_FOCUS_LOST;mph_input_handle(im,&e);
        sent=injections;refresh.type=SDL_EVENT_KEY_DOWN;mph_input_handle(im,&refresh);
        assert(injections==sent && !im->mph.mapper.contacts[1].down);
        e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mph_input_handle(im,&e);
        key(im,SDL_SCANCODE_F1);assert(!im->mph.enabled);
        sent=injections;mph_input_handle(im,&refresh);assert(injections==sent);
    }
    /* Diagnostic cursor tracks the mapped finger, without changing capture. */
    key(im,SDL_SCANCODE_NUMLOCKCLEAR);assert(!im->mph.diagnostic_arrow);
    key(im,SDL_SCANCODE_F1);assert(im->mph.enabled);
    key(im,SDL_SCANCODE_GRAVE);
    s.rect=(SDL_FRect){100,60,800,480};
    float ox=im->mph.mapper.look_x,oy=im->mph.mapper.look_y;
    check_arrow(im,ox,oy,false);
    unsigned toggle_injections=injections;
    key(im,SDL_SCANCODE_NUMLOCKCLEAR);assert(im->mph.diagnostic_arrow);
    assert(injections==toggle_injections && SDL_GetWindowRelativeMouseMode(s.window));
    SDL_Event lock={0};lock.type=SDL_EVENT_KEY_DOWN;lock.key.scancode=SDL_SCANCODE_NUMLOCKCLEAR;
    lock.key.repeat=true;mph_input_handle(im,&lock);assert(im->mph.diagnostic_arrow);
    lock.type=SDL_EVENT_KEY_UP;mph_input_handle(im,&lock);assert(im->mph.diagnostic_arrow);
    check_arrow(im,ox,oy,true);
    e.type=SDL_EVENT_MOUSE_MOTION;e.motion.xrel=80;e.motion.yrel=20;
    mph_input_handle(im,&e);im->mph.look_last_tick=0;mph_input_handle(im,&tick);
    struct mph_contact point=im->mph.mapper.contacts[1];
    assert(point.x>ox && point.y>oy);
    check_arrow(im,point.x,point.y,true);
    key(im,SDL_SCANCODE_GRAVE);
    assert(im->mph.mapper.contacts[1].x==ox && im->mph.mapper.contacts[1].y==oy);
    check_arrow(im,ox,oy,true);
    key(im,SDL_SCANCODE_NUMLOCKCLEAR);assert(!im->mph.diagnostic_arrow);
    check_arrow(im,ox,oy,false);
    key(im,SDL_SCANCODE_NUMLOCKCLEAR);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST;mph_input_handle(im,&e);check_arrow(im,ox,oy,false);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mph_input_handle(im,&e);
    key(im,SDL_SCANCODE_F1);check_arrow(im,ox,oy,false);
    s.rect=(SDL_FRect){0,0,1000,600};
    /* Keypad shortcuts work in normal, editing and captured mapping modes. */
    for(int mode=0;mode<3;++mode) {
        if(mode==1) key(im,SDL_SCANCODE_F9);
        if(mode==2) key(im,SDL_SCANCODE_F1);
        unsigned count=im->mph.mapper.count, touch_before=injections;
        SDL_SetWindowSize(s.window,1000,600);
        key(im,SDL_SCANCODE_KP_PLUS);
        int w,h;SDL_GetWindowSize(s.window,&w,&h);assert(h==690);
        key(im,SDL_SCANCODE_KP_MINUS);
        SDL_GetWindowSize(s.window,&w,&h);assert(h>=599 && h<=600);
        key(im,SDL_SCANCODE_KP_MULTIPLY);
        SDL_Rect bounds;assert(SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(s.window),&bounds));
        SDL_GetWindowSize(s.window,&w,&h);
        int fit=(int)fminf(bounds.h-100,(bounds.w-MPH_TOOLBAR_WIDTH-100)/(2000.0f/1200));
        assert(h==fit && w==(int)(fit*(2000.0f/1200)));
        int centered_x,centered_y;SDL_GetWindowPosition(s.window,&centered_x,&centered_y);
        assert(centered_x==bounds.x+(bounds.w-w)/2);
        assert(centered_y==bounds.y+(bounds.h-h)/2);
        unsigned power_before=power_injections;bool was_off=im->mph.phone_off_requested;
        key(im,SDL_SCANCODE_KP_DIVIDE);
        assert(power_injections==power_before+1 && im->mph.phone_off_requested!=was_off);
        SDL_Event kp={0};kp.type=SDL_EVENT_KEY_DOWN;kp.key.scancode=SDL_SCANCODE_KP_DIVIDE;
        kp.key.repeat=true;assert(mph_input_handle(im,&kp));
        kp.type=SDL_EVENT_KEY_UP;kp.key.repeat=false;assert(mph_input_handle(im,&kp));
        assert(power_injections==power_before+1);
        key(im,SDL_SCANCODE_KP_DIVIDE);
        assert(power_injections==power_before+2 && im->mph.phone_off_requested==was_off);
        assert(im->mph.mapper.count==count && injections==touch_before);
        assert(im->mph.editing==(mode==1) && im->mph.enabled==(mode==2));
        if(mode==1) key(im,SDL_SCANCODE_F5);
        if(mode==2) key(im,SDL_SCANCODE_F1);
    }
    SDL_SetWindowSize(s.window,1000,600);
    SDL_ShowWindow(s.window);mph_toolbar_sync(im);
    mph_toolbar_render(im);
    bar_shot=SDL_RenderReadPixels(im->mph.toolbar_renderer,NULL);assert(bar_shot);
    assert(SDL_SavePNG(bar_shot,"work/toolbar-default-test.png"));SDL_DestroySurface(bar_shot);
    int bx,by,bw,bh;SDL_GetWindowPosition(im->mph.toolbar_window,&bx,&by);
    SDL_GetWindowSize(im->mph.toolbar_window,&bw,&bh);assert(bw==MPH_TOOLBAR_WIDTH && bh==260);
    int sx,sy;SDL_GetWindowPosition(s.window,&sx,&sy);
    HWND hwnd=SDL_GetPointerProperty(SDL_GetWindowProperties(s.window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    RECT visible;assert(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))));
    assert(bx==visible.right && by==visible.top); /* exactly flush with visible border */
    SDL_SetWindowPosition(s.window,sx+25,sy+20);mph_toolbar_sync(im);
    int moved_x,moved_y;SDL_GetWindowPosition(im->mph.toolbar_window,&moved_x,&moved_y);
    assert(moved_x==bx+25 && moved_y==by+20);
    SDL_HideWindow(s.window);mph_toolbar_sync(im);assert(SDL_GetWindowFlags(im->mph.toolbar_window)&SDL_WINDOW_HIDDEN);
    /* Real IPC parser and native gesture lifecycle, with a fake controller. */
    assert(SDL_CreateDirectory("work/mcp-native-test"));
    assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"MPH_MCP_DIR","work/mcp-native-test",true));
    s.window_shown=true;s.disconnected=false;im->disconnected=false;
    unsigned before_remote=injections;
    mcp_command(im,"touch -1 .5 .5 .5 100");assert(injections==before_remote);
    mcp_command(im,"edit begin");assert(im->mph.editing);
    mcp_command(im,"touch .2 .3 .4 .5 100");assert(injections==before_remote);
    mcp_command(im,"bind 58 .2 .3"); /* F1 must remain reserved. */
    for(unsigned i=0;i<im->mph.mapper.count;i++)assert(im->mph.mapper.bindings[i].key!=58);
    mcp_command(im,"edit discard");assert(!im->mph.editing);
    mcp_command(im,"touch .2 .3 .4 .5 100");assert(im->mph.mcp_touch_down && last_touch_action==AMOTION_EVENT_ACTION_DOWN);
    im->mph.mcp_touch_until=SDL_GetTicks();
    SDL_Event mt={0};mt.type=SC_EVENT_MPH_MCP_TICK;mph_input_handle(im,&mt);
    assert(!im->mph.mcp_touch_down && last_touch_action==AMOTION_EVENT_ACTION_UP);
    mcp_command(im,"touch .2 .3 .2 .3 5000");assert(im->mph.mcp_touch_down);
    reject_touch=true;
    SDL_Event focus={0};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;mph_input_handle(im,&focus);
    assert(im->mph.mcp_touch_down); /* retry release after temporary queue failure */
    reject_touch=false;mph_input_handle(im,&mt);
    assert(!im->mph.mcp_touch_down && last_touch_action==AMOTION_EVENT_ACTION_UP);
    mcp_command(im,"screenshot .9 0 .5 1");assert(!im->mph.mcp_capture);
    s.rect=(SDL_FRect){100,50,800,500}; /* Letterboxing is excluded from export. */
    mcp_command(im,"screenshot 0 .95 .35 .05");assert(im->mph.mcp_capture);
    SDL_SetRenderDrawColor(s.renderer,45,55,65,255);SDL_RenderClear(s.renderer);
    mph_input_render(im);mph_mcp_capture_frame(im);assert(!im->mph.mcp_capture);
    SDL_Surface *mcp_shot=SDL_LoadPNG("work/mcp-native-test/screenshot.png");assert(mcp_shot);
    float density=SDL_GetWindowPixelDensity(s.window);
    int phone_w=(int)roundf(800*density),phone_h=(int)roundf(500*density);
    assert(mcp_shot->w>phone_w);
    Uint8 mr,mg,mb,ma;
    assert(SDL_ReadSurfacePixel(mcp_shot,phone_w/10,phone_h-2,&mr,&mg,&mb,&ma));
    assert(mr==20 && mg==28 && mb==43 && ma==255);
    SDL_DestroySurface(mcp_shot);
    assert(SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(),"MPH_MCP_DIR"));
    puts("MCP native: PASS (CRLF mailbox, invalid input, editor exclusion, DOWN/UP, cancellation retry, PNG)");
    mph_toolbar_destroy(im);assert(!im->mph.toolbar_window && !im->mph.toolbar_renderer);
    mph_text_destroy(s.renderer);
    SDL_DestroyRenderer(s.renderer);SDL_DestroyWindow(s.window);SDL_Quit();
    puts("MPH editor: PASS (add/rebind/drag/normalized/save/reload/overwrite/delete/discard/render)");
}

