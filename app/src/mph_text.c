/* SPDX-License-Identifier: Apache-2.0 */
/* UTF-8 UI text. Cached Windows system-font glyphs, white with black outline. */
#include "mph_text.h"
#include <math.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

struct text_entry { SDL_Renderer *renderer; SDL_Texture *texture; char *text; int pixels, w, h; };
static struct text_entry cache[128];
static unsigned next_entry;

static void clear_entry(struct text_entry *entry) {
    if(entry->texture) SDL_DestroyTexture(entry->texture);
    SDL_free(entry->text);memset(entry,0,sizeof(*entry));
}

static struct text_entry *get_text(SDL_Renderer *renderer,const char *text,float height) {
    float sx,sy;SDL_GetRenderScale(renderer,&sx,&sy);
    int pixels=(int)ceilf(height*sy);
    if(!*text || pixels<1 || pixels>256) return NULL;
    for(unsigned i=0;i<128;++i)
        if(cache[i].renderer==renderer && cache[i].pixels==pixels && !strcmp(cache[i].text,text)) return &cache[i];
    wchar_t wide[1024];
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,wide,1024);
    if(!length) return NULL;
    HDC dc=CreateCompatibleDC(NULL);if(!dc) return NULL;
    HFONT font=CreateFontW(-pixels,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    if(!font) {DeleteDC(dc);return NULL;}
    HGDIOBJ old_font=SelectObject(dc,font);
    SIZE size;GetTextExtentPoint32W(dc,wide,length-1,&size);
    int border=(pixels+9)/10,w=size.cx+border*2,h=size.cy+border*2;
    BITMAPINFO info;memset(&info,0,sizeof(info));info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    unsigned char *bits=NULL;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void **)&bits,NULL,0);
    SDL_Surface *surface=NULL;
    if(bitmap) {
        HGDIOBJ old_bitmap=SelectObject(dc,bitmap);
        memset(bits,0,(size_t)w*h*4);SetBkColor(dc,RGB(0,0,0));SetTextColor(dc,RGB(255,255,255));
        TextOutW(dc,border,border,wide,length-1);GdiFlush();
        surface=SDL_CreateSurface(w,h,SDL_PIXELFORMAT_RGBA32);
        if(surface) {
            for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                unsigned alpha=0,body=bits[(y*w+x)*4];
                for(int dy=-border;dy<=border;++dy) for(int dx=-border;dx<=border;++dx) {
                    int px=x+dx,py=y+dy;
                    if(px>=0 && px<w && py>=0 && py<h) {
                        unsigned a=bits[(py*w+px)*4];if(a>alpha) alpha=a;
                    }
                }
                unsigned char *out=(unsigned char *)surface->pixels+y*surface->pitch+x*4;
                out[0]=out[1]=out[2]=alpha?body*255/alpha:0;out[3]=alpha;
            }
        }
        SelectObject(dc,old_bitmap);DeleteObject(bitmap);
    }
    SelectObject(dc,old_font);DeleteObject(font);DeleteDC(dc);
    if(!surface) return NULL;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(renderer,surface);SDL_DestroySurface(surface);
    if(!texture) return NULL;
    char *copy=SDL_strdup(text);if(!copy) {SDL_DestroyTexture(texture);return NULL;}
    struct text_entry *entry=&cache[next_entry++%128];clear_entry(entry);
    *entry=(struct text_entry){renderer,texture,copy,pixels,w,h};
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
    return entry;
}

float mph_text_width(SDL_Renderer *renderer,const char *text,float height) {
    struct text_entry *e=get_text(renderer,text,height);float sx,sy;SDL_GetRenderScale(renderer,&sx,&sy);
    return e?e->w/sx:0;
}
void mph_text_draw(SDL_Renderer *renderer,float x,float y,const char *text,float height) {
    struct text_entry *e=get_text(renderer,text,height);if(!e) return;
    float sx,sy;SDL_GetRenderScale(renderer,&sx,&sy);
    SDL_FRect dst={x,y,e->w/sx,e->h/sy};SDL_RenderTexture(renderer,e->texture,NULL,&dst);
}
void mph_text_destroy(SDL_Renderer *renderer) {
    for(unsigned i=0;i<128;++i) if(cache[i].renderer==renderer) clear_entry(&cache[i]);
}
#else
float mph_text_width(SDL_Renderer *r,const char *t,float h) {(void)r;(void)h;return strlen(t)*8;}
void mph_text_draw(SDL_Renderer *r,float x,float y,const char *t,float h) {(void)h;SDL_RenderDebugText(r,x,y,t);}
void mph_text_destroy(SDL_Renderer *r) {(void)r;}
#endif
