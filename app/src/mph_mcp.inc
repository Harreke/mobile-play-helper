/* SPDX-License-Identifier: Apache-2.0 */
/* Included by mph_input.c: all native MCP actions run on the SDL UI thread. */
static SDL_AtomicInt mcp_tick_pending;
static bool mcp_path(char *out,size_t size,const char *name) {
    const char *dir=SDL_getenv("MPH_MCP_DIR");
    return dir && *dir && snprintf(out,size,"%s/%s",dir,name)<(int)size;
}
static Uint32 SDLCALL mph_mcp_timer(void *unused,SDL_TimerID id,Uint32 interval) {
    (void)unused;(void)id;
    if(SDL_CompareAndSwapAtomicInt(&mcp_tick_pending,0,1)) {
        SDL_Event e={0};e.type=SC_EVENT_MPH_MCP_TICK;
        if(!SDL_PushEvent(&e))SDL_SetAtomicInt(&mcp_tick_pending,0);
    }
    return interval;
}
static void mcp_reply(struct sc_input_manager *im,const char *error) {
    struct mph_input *in=&im->mph;
    if(!*in->mcp_request_id)return;
    char path[4096],tmp[4096];
    if(!mcp_path(path,sizeof(path),"response.json") || !mcp_path(tmp,sizeof(tmp),"response.tmp"))return;
    FILE *f=profile_open(tmp,"wb");
    if(!f)return;
    fprintf(f,"{\"id\":\"%s\",\"ok\":%s,\"error\":\"%s\",\"enabled\":%s,\"suspended\":%s,\"editing\":%s,\"connected\":%s,\"width\":%u,\"height\":%u,\"fps\":%.2f,\"bindings\":[",
        in->mcp_request_id,error?"false":"true",error?error:"",
        in->enabled?"true":"false",in->suspended?"true":"false",in->editing?"true":"false",
        im->disconnected?"false":"true",im->screen->frame_size.width,im->screen->frame_size.height,in->fps_value);
    for(unsigned i=0;i<in->mapper.count;i++) {
        struct mph_binding *b=&in->mapper.bindings[i];
        fprintf(f,"%s{\"key\":%d,\"x\":%.6f,\"y\":%.6f}",i?",":"",b->key,b->x,b->y);
    }
    fprintf(f,"],\"joystick\":[%.6f,%.6f,%.6f],\"look\":[%.6f,%.6f,%.6f,%.6f],\"delivery\":\"queue_only_not_game_ack\"}\n",
        in->mapper.joy_x,in->mapper.joy_y,in->mapper.joy_radius,
        in->mapper.look_x,in->mapper.look_y,in->mapper.look_radius,in->mapper.sensitivity);
    bool ok=!ferror(f);if(fclose(f))ok=false;
    if(ok)SDL_RenamePath(tmp,path);
    in->mcp_request_id[0]=0;
}
static void mph_mcp_cancel(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(!in->mcp_touch_down && !in->mcp_capture)return;
    if(in->mcp_touch_down) {
        in->mcp_touch_cancelled=true;in->mcp_touch_until=SDL_GetTicks();
        if(!emit(im,MPH_CONTACTS,MPH_UP,in->mcp_x,in->mcp_y) && !im->disconnected)return;
    }
    in->mcp_touch_down=false;in->mcp_capture=false;
    mcp_reply(im,"cancelled; release queued if connection available");
}
void mph_mcp_capture_frame(struct sc_input_manager *im) {
    struct mph_input *in=&im->mph;
    if(!in->mcp_capture)return;
    in->mcp_capture=false;
    /* Crop letterboxing so normalized masks and gestures refer to the same image. */
    float scale=SDL_GetWindowPixelDensity(im->screen->window);
    if(scale<=0)scale=1;
    SDL_FRect area=im->screen->rect;
    SDL_Rect crop={(int)roundf(area.x*scale),(int)roundf(area.y*scale),
        (int)roundf(area.w*scale),(int)roundf(area.h*scale)};
    SDL_Surface *frame=crop.w>0 && crop.h>0
        ?SDL_RenderReadPixels(im->screen->renderer,&crop):NULL;
    SDL_Surface *bar=in->mcp_toolbar,*out=NULL;
    if(frame) {
        int height=bar && bar->h>frame->h?bar->h:frame->h;
        out=SDL_CreateSurface(frame->w+(bar?bar->w:0),height,SDL_PIXELFORMAT_RGBA32);
        if(out) {
            SDL_FillSurfaceRect(out,NULL,SDL_MapSurfaceRGBA(out,20,28,43,255));
            SDL_SetSurfaceBlendMode(frame,SDL_BLENDMODE_NONE);
            SDL_BlitSurface(frame,NULL,out,NULL);
            if(in->mcp_mask_w>0 && in->mcp_mask_h>0) {
                SDL_Rect mask={(int)(frame->w*in->mcp_mask_x),(int)(frame->h*in->mcp_mask_y),
                    (int)ceilf(frame->w*in->mcp_mask_w),(int)ceilf(frame->h*in->mcp_mask_h)};
                SDL_FillSurfaceRect(out,&mask,SDL_MapSurfaceRGBA(out,20,28,43,255));
            }
            if(bar) {SDL_Rect dest={frame->w,0,bar->w,bar->h};SDL_SetSurfaceBlendMode(bar,SDL_BLENDMODE_NONE);SDL_BlitSurface(bar,NULL,out,&dest);}
        }
    }
    char path[4096];
    bool ok=out && mcp_path(path,sizeof(path),"screenshot.png") && SDL_SavePNG(out,path);
    SDL_DestroySurface(frame);SDL_DestroySurface(out);
    SDL_DestroySurface(in->mcp_toolbar);in->mcp_toolbar=NULL;
    mcp_reply(im,ok?NULL:"capture failed");
}
static bool mcp_live(struct sc_input_manager *im) {
    return !im->disconnected && im->controller && !im->camera && !im->screen->paused &&
        im->screen->window_shown && im->screen->frame_size.width && im->screen->frame_size.height &&
        im->screen->orientation==SC_ORIENTATION_0;
}
static void mph_mcp_poll(struct sc_input_manager *im) {
    SDL_SetAtomicInt(&mcp_tick_pending,0);
    struct mph_input *in=&im->mph;
    char path[4096];SDL_PathInfo info;
    Uint64 now=SDL_GetTicks();
    if(mcp_path(path,sizeof(path),"cancel") && SDL_GetPathInfo(path,&info)) {
        SDL_RemovePath(path);mph_mcp_cancel(im);mph_input_stop(im);redraw(im);
    }
    if(in->mcp_touch_down) {
        if(!mcp_live(im)) {mph_mcp_cancel(im);return;}
        float fraction=now>=in->mcp_touch_until?1.0f:(float)(now-in->mcp_touch_started)/(in->mcp_touch_until-in->mcp_touch_started);
        in->mcp_x=in->mcp_x0+(in->mcp_x1-in->mcp_x0)*fraction;
        in->mcp_y=in->mcp_y0+(in->mcp_y1-in->mcp_y0)*fraction;
        bool ended=now>=in->mcp_touch_until;
        if(!emit(im,MPH_CONTACTS,ended?MPH_UP:MPH_MOVE,in->mcp_x,in->mcp_y)) {
            mph_mcp_cancel(im);return;
        }
        if(ended) {in->mcp_touch_down=false;mcp_reply(im,in->mcp_touch_cancelled?"gesture cancelled":NULL);}
        return;
    }
    if(in->mcp_capture || now<in->mcp_poll_at)return;
    in->mcp_poll_at=now+64;
    if(!mcp_path(path,sizeof(path),"request.txt"))return;
    FILE *f=profile_open(path,"rb");if(!f)return;
    char request[8192];size_t count=fread(request,1,sizeof(request)-1,f);
    bool oversized=count==sizeof(request)-1 || ferror(f);fclose(f);SDL_RemovePath(path);
    if(oversized)return;
    request[count]=0;char *cmd=strchr(request,'\n');if(!cmd)return;*cmd++=0;
    request[strcspn(request,"\r")]=0;
    if(strlen(request)!=32 || strspn(request,"0123456789abcdef")!=32)return;
    memcpy(in->mcp_request_id,request,33);
    cmd[strcspn(cmd,"\r\n")]=0;
    const char *error=NULL;float x,y,x2,y2,r,s;int key,duration,on;char extra;
    if(!strcmp(cmd,"state")) { /* read-only */ }
    else if(!strcmp(cmd,"stop")) {mph_input_stop(im);SDL_Event e={0};e.type=SDL_EVENT_QUIT;SDL_PushEvent(&e);}
    else if(!strcmp(cmd,"screenshot") || sscanf(cmd,"screenshot %f %f %f %f %c",&x,&y,&r,&s,&extra)==4) {
        in->mcp_mask_w=in->mcp_mask_h=0;
        if(!im->screen->window_shown || im->disconnected)error="no live video";
        else {
            if(strcmp(cmd,"screenshot")) {
                if(!unit(x)||!unit(y)||!unit(r)||!unit(s)||x+r>1||y+s>1) {mcp_reply(im,"invalid mask");return;}
                in->mcp_mask_x=x;in->mcp_mask_y=y;in->mcp_mask_w=r;in->mcp_mask_h=s;
            }
            in->mcp_capture=true;redraw(im);return;
        }
    }
    else if(sscanf(cmd,"mapping %d %c",&on,&extra)==1 && (on==0 || on==1)) {
        if(in->editing)error="save or discard edits first";
        else if(on) {if(!in->enabled)start(im);if(!in->enabled)error="mapping could not start";}
        else mph_input_stop(im);
    }
    else if(!strcmp(cmd,"edit begin")) {
        if(in->enabled)error="disable mapping before editing";
        else {SDL_Event e={0};e.type=SDL_EVENT_KEY_DOWN;e.key.scancode=SDL_SCANCODE_F9;editor_handle(im,&e);if(!in->editing)error="editor unavailable";}
    }
    else if(!strcmp(cmd,"edit save")) {
        if(!in->editing)error="not editing";
        else if(!save_profile(in))error="profile save failed";
        else {in->editing=false;in->assigning=false;in->dragging=false;in->selected=-1;SDL_SetWindowTitle(im->screen->window,in->title);}
    }
    else if(!strcmp(cmd,"edit discard")) {editor_discard(im);if(in->editing)error="discard failed";}
    else if(!strncmp(cmd,"profile ",8)) {
        if(!mph_input_select_profile(im,cmd+8,false))error="profile load failed or editing active";
    }
    else if(sscanf(cmd,"bind %d %f %f %c",&key,&x,&y,&extra)==3) {
        if(!in->editing || key<=0 || key>=518 || key==512 || reserved_key(key) || !unit(x) || !unit(y))error="invalid binding or editor inactive";
        else {unsigned i;for(i=0;i<in->mapper.count;i++)if(in->mapper.bindings[i].key==key)break;
            if(i==MPH_BINDINGS)error="binding limit reached";
            else {in->mapper.bindings[i]=(struct mph_binding){key,x,y};if(i==in->mapper.count)in->mapper.count++;in->selected=(int)i;in->assigning=false;snprintf(in->status,sizeof(in->status),"MCP 已更新点位；F5 保存退出");}}
    }
    else if(sscanf(cmd,"delete %d %c",&key,&extra)==1) {
        if(!in->editing)error="not editing";
        else {unsigned i;for(i=0;i<in->mapper.count;i++)if(in->mapper.bindings[i].key==key)break;
            if(i==in->mapper.count)error="binding not found";else {in->selected=(int)i;editor_delete(in);}}
    }
    else if(sscanf(cmd,"zone %d %f %f %f %f %c",&key,&x,&y,&r,&s,&extra)==5) {
        float aspect=im->screen->frame_size.height?(float)im->screen->frame_size.width/im->screen->frame_size.height:0;
        if(!in->editing || (key!=0 && key!=1) || !unit(x)||!unit(y)||!isfinite(r)||r<=0||r>.4f||!isfinite(s)||s<=0||s>.05f||aspect<=0||x-r/aspect<0||x+r/aspect>1||y-r<0||y+r>1)error="invalid zone";
        else if(key==0) {in->mapper.joy_x=x;in->mapper.joy_y=y;in->mapper.joy_radius=r;in->selected=-2;}
        else {in->mapper.look_x=x;in->mapper.look_y=y;in->mapper.look_radius=r;in->mapper.sensitivity=s;in->selected=-3;}
    }
    else if(sscanf(cmd,"touch %f %f %f %f %d %c",&x,&y,&x2,&y2,&duration,&extra)==5) {
        if(!mcp_live(im)||in->editing||in->enabled)error="touch requires live ordinary mode";
        else if(!unit(x)||!unit(y)||!unit(x2)||!unit(y2)||duration<50||duration>5000)error="invalid gesture";
        else if(!emit(im,MPH_CONTACTS,MPH_DOWN,x,y))error="touch queue rejected";
        else {in->mcp_touch_down=true;in->mcp_touch_cancelled=false;in->mcp_x0=in->mcp_x=x;in->mcp_y0=in->mcp_y=y;in->mcp_x1=x2;in->mcp_y1=y2;in->mcp_touch_started=now;in->mcp_touch_until=now+duration;return;}
    }
    else error="unknown or malformed command";
    redraw(im);mcp_reply(im,error);
}
