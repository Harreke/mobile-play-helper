/* SPDX-License-Identifier: Apache-2.0 */
#include "mph_quality.h"
#include "input_manager.h"
#include "screen.h"
#include "events.h"
#include "options.h"
#include "util/log.h"
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include "../mph_quality_config.h"
static wchar_t settings_path[4096];
static char fps_string[16];
void mph_quality_load(struct scrcpy_options *opts) {
    mph_quality_path(settings_path,L"mph-quality.ini");
    struct mph_quality_settings q=mph_quality_read(settings_path);
    opts->max_size=q.size;opts->video_bit_rate=q.mbps*1000000;
    opts->video_codec=q.codec==1?SC_CODEC_H265:q.codec==2?SC_CODEC_AV1:SC_CODEC_H264;
    if(q.fps) {snprintf(fps_string,sizeof(fps_string),"%u",q.fps);opts->max_fps=fps_string;}
}
void mph_quality_connected(void) {
    if(getenv("MPH_BENCHMARK")) return;
    wchar_t pending[4096];mph_quality_path(pending,L"mph-quality.pending");DeleteFileW(pending);
}
static HWND control(HWND parent,const wchar_t *kind,const wchar_t *text,DWORD style,int x,int y,int w,int h,int id) {
    HWND c=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);return c;
}
static void number(HWND dialog,int id,unsigned n) {SetDlgItemInt(dialog,id,n,FALSE);}
static INT_PTR CALLBACK dialog_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    if(msg==WM_INITDIALOG) {
        struct mph_quality_settings q=mph_quality_read(settings_path);
        SetWindowTextW(w,L"投屏画质设置");
        control(w,L"STATIC",L"分辨率上限（长边像素，0 = 手机原始分辨率）",0,16,16,365,20,0);
        control(w,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,16,40,140,24,101);number(w,101,q.size);
        control(w,L"STATIC",L"帧率上限（0 = 不限制；实际取决于手机和内容）",0,16,74,370,20,0);
        control(w,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,16,98,140,24,102);number(w,102,q.fps);
        control(w,L"STATIC",L"视频码率（Mbps，1～100；越高不一定越清晰）",0,16,132,370,20,0);
        control(w,L"EDIT",L"",WS_BORDER|ES_NUMBER|WS_TABSTOP,16,156,140,24,103);number(w,103,q.mbps);
        control(w,L"STATIC",L"编码格式（不支持时会恢复之前设置）",0,16,190,370,20,0);
        HWND combo=control(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,16,214,190,110,104);
        SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"H.264（兼容优先）");
        SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"H.265 / HEVC");SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"AV1");
        SendMessageW(combo,CB_SETCURSEL,q.codec,0);
        control(w,L"STATIC",L"应用会短暂断开并重连投屏。\n自动测试已暂停：正在排查测试后的手机游戏黑屏。\n手动画质设置与历史测试结果仍可使用。",0,16,250,395,58,0);
        control(w,L"BUTTON",L"应用并重连",BS_DEFPUSHBUTTON|WS_TABSTOP,16,320,115,30,IDOK);
        control(w,L"BUTTON",L"自动测试（暂停）",WS_TABSTOP|WS_DISABLED,141,320,110,30,105);
        control(w,L"BUTTON",L"查看测试结果",WS_TABSTOP,261,320,125,30,106);
        control(w,L"BUTTON",L"取消",WS_TABSTOP,276,361,110,28,IDCANCEL);
        return TRUE;
    }
    if(msg==WM_CLOSE) {EndDialog(w,IDCANCEL);return TRUE;}
    if(msg!=WM_COMMAND) return FALSE;
    int id=LOWORD(wp);
    if(id==IDCANCEL) {EndDialog(w,IDCANCEL);return TRUE;}
    if(id==106) {
        wchar_t path[4096],text[8192];mph_quality_path(path,L"mph-quality-result.txt");
        FILE *f=_wfopen(path,L"rb");
        if(!f) MessageBoxW(w,L"尚无测试结果。",L"画质测试",MB_OK);
        else {size_t n=fread(text,sizeof(wchar_t),8191,f);text[n]=0;fclose(f);MessageBoxW(w,text,L"画质测试结果",MB_OK);}
        return TRUE;
    }
    if(id==105) {
        if(!MPH_QUALITY_AUTOTEST_ENABLED) return TRUE;
        EndDialog(w,105);return TRUE;
    }
    if(id==IDOK) {
        BOOL a,b,c;struct mph_quality_settings q={GetDlgItemInt(w,101,&a,FALSE),GetDlgItemInt(w,102,&b,FALSE),GetDlgItemInt(w,103,&c,FALSE),(unsigned)SendDlgItemMessageW(w,104,CB_GETCURSEL,0,0)};
        if(!a || !b || !c || !mph_quality_valid(q)) {MessageBoxW(w,L"长边：0 或 480～8192；帧率：0 或 15～240；码率：1～100 Mbps。",L"请检查设置",MB_OK);return TRUE;}
        wchar_t backup[4096],pending[4096];mph_quality_path(backup,L"mph-quality-backup.ini");mph_quality_path(pending,L"mph-quality.pending");
        if(!mph_quality_write(backup,mph_quality_read(settings_path)) || !mph_quality_write(pending,q) || !mph_quality_write(settings_path,q)) {
            MessageBoxW(w,L"保存失败，请检查程序目录的写入权限。",L"画质设置",MB_OK|MB_ICONERROR);return TRUE;
        }
        EndDialog(w,IDOK);return TRUE;
    }
    return FALSE;
}
void mph_quality_dialog(struct sc_input_manager *im) {
    HWND owner=SDL_GetPointerProperty(SDL_GetWindowProperties(im->screen->window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,NULL);
    if(im->mph.editing) {MessageBoxW(owner,L"请先保存或放弃布局编辑。",L"投屏画质",MB_OK);return;}
    if(!settings_path[0]) mph_quality_path(settings_path,L"mph-quality.ini");
    struct {DLGTEMPLATE d;WORD menu,cls,title;} t={0};
    t.d.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;t.d.cx=280;t.d.cy=270;
    INT_PTR result=DialogBoxIndirectParamW(GetModuleHandleW(NULL),&t.d,owner,dialog_proc,0);
    if(result==IDOK || result==105) {
        /* Preserve the selected game profile across the process restart. */
        wchar_t profile[4096];if(MultiByteToWideChar(CP_UTF8,0,im->mph.profile_path,-1,profile,4096)) SetEnvironmentVariableW(L"MPH_PROFILE",profile);
        SetEnvironmentVariableW(L"MPH_RESUME_COMMAND",GetCommandLineW());
        SDL_Event event={0};event.type=result==IDOK?SC_EVENT_MPH_RESTART:SC_EVENT_MPH_BENCHMARK;SDL_PushEvent(&event);
    }
}
int mph_quality_finish(int code) {
    if(getenv("MPH_BENCHMARK")) return code;
    if(code==3 || code==4) {
        wchar_t command[32768],exe[4096];
        if(code==3) wcsncpy(command,GetCommandLineW(),32767);
        else {mph_quality_path(exe,L"mph-quality-test.exe");swprintf(command,32768,L"\"%ls\"",exe);}
        command[32767]=0;
        STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
        if(CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) {
            CloseHandle(pi.hProcess);CloseHandle(pi.hThread);return 0;
        }
        LOGE("Quality restart failed: %lu",GetLastError());return 1;
    }
    if(code!=0) {
        wchar_t pending[4096],backup[4096];mph_quality_path(pending,L"mph-quality.pending");mph_quality_path(backup,L"mph-quality-backup.ini");
        if(GetFileAttributesW(pending)!=INVALID_FILE_ATTRIBUTES) {
            if(mph_quality_write(settings_path,mph_quality_read(backup))) {DeleteFileW(pending);LOGW("Quality startup failed; previous settings restored");}
        }
    }
    return code;
}
#else
void mph_quality_load(struct scrcpy_options *opts) {(void)opts;}
void mph_quality_connected(void) {}
void mph_quality_dialog(struct sc_input_manager *im) {(void)im;}
int mph_quality_finish(int code) {return code;}
#endif
