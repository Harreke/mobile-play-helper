/* SPDX-License-Identifier: Apache-2.0 */
/* Sequential, user-started quality trials. Never runs alongside a second stream. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include "mph_quality_config.h"
#include "mph_quality_metrics.h"
static HWND window;
static volatile LONG cancelled;
static wchar_t report[8192];
static BOOL CALLBACK close_child(HWND w,LPARAM pid) {
    DWORD owner;GetWindowThreadProcessId(w,&owner);
    if(owner==(DWORD)pid) PostMessageW(w,WM_CLOSE,0,0);
    return TRUE;
}
struct quality_trial {
    struct mph_quality_metrics metrics;
    DWORD code;
    BOOL timedout,stopped,stalled;
    unsigned width,height;
};
static void read_metrics(const wchar_t *path,struct quality_trial *trial) {
    mph_quality_metrics_init(&trial->metrics);
    FILE *f=_wfopen(path,L"rb");if(!f)return;
    char line[2048];
    while(fgets(line,sizeof(line),f)) {
        unsigned fps,drop=0,w,h;
        if(!strchr(line,'\n'))continue; /* a writer may still be completing the line */
        if(sscanf(line,"INFO: Texture: %ux%u",&w,&h)==2) {trial->width=w;trial->height=h;}
        if(sscanf(line,"INFO: %u fps",&fps)!=1)continue;
        const char *plus=strstr(line,"(+");if(plus)sscanf(plus,"(+%u frames skipped)",&drop);
        mph_quality_metrics_add(&trial->metrics,fps,drop);
    }
    fclose(f);
}
static struct quality_trial trial_run(unsigned size,unsigned mbps,unsigned samples,const wchar_t *archive_name) {
    wchar_t exe[4096],log[4096],command[32768],archive[4096];
    mph_quality_path(exe,L"MobilePlayHelper.exe");mph_quality_path(log,L"MobilePlayHelper.log");
    struct quality_trial trial={0};trial.code=1;mph_quality_metrics_init(&trial.metrics);
    unsigned duration=MPH_QUALITY_WARMUP_SAMPLES+samples+1;
    swprintf(command,32768,L"\"%ls\" --no-control --no-audio --no-downsize-on-error --print-fps --time-limit=%u --video-codec=h264 --max-size=%u --max-fps=60 --video-bit-rate=%uM --window-title=MPH-60FPS-Test",exe,duration,size,mbps);
    STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi))return trial;
    CloseHandle(pi.hThread);ULONGLONG start=GetTickCount64(),last_read=start;
    while(WaitForSingleObject(pi.hProcess,100)==WAIT_TIMEOUT) {
        ULONGLONG now=GetTickCount64();
        if(now-last_read>=1000) {read_metrics(log,&trial);last_read=now;}
        trial.stopped=InterlockedCompareExchange(&cancelled,0,0)!=0;
        trial.timedout=now-start>(duration+15)*1000ULL;
        trial.stalled=trial.metrics.stalled;
        if(trial.stopped || trial.timedout || trial.stalled) {
            EnumWindows(close_child,pi.dwProcessId);
            if(WaitForSingleObject(pi.hProcess,3000)!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,1);
            break;
        }
    }
    WaitForSingleObject(pi.hProcess,3000);GetExitCodeProcess(pi.hProcess,&trial.code);CloseHandle(pi.hProcess);
    read_metrics(log,&trial);
    trial.stalled=trial.stalled || trial.metrics.stalled;
    trial.stopped=trial.stopped || InterlockedCompareExchange(&cancelled,0,0)!=0;
    mph_quality_path(archive,archive_name);CopyFileW(log,archive,FALSE);
    return trial;
}
static BOOL trial_pass(const struct quality_trial *t,unsigned required) {
    return !t->code && !t->timedout && !t->stopped && !t->stalled && mph_quality_metrics_pass(&t->metrics,required);
}
static void describe_trial(const wchar_t *label,unsigned size,unsigned mbps,const struct quality_trial *t,unsigned samples) {
    const wchar_t *status=t->stopped?L"已中止":t->stalled?L"连续三秒无帧，已终止":t->timedout?L"超时":t->code?L"连接/进程异常":
        t->metrics.count<samples?L"样本不足":trial_pass(t,samples)?L"60 FPS 达标":L"未达到稳定 60 FPS";
    wchar_t row[700];swprintf(row,700,L"%ls：长边上限 %u（0=原生），%u Mbps，60 FPS\r\n  实际 %ux%u；%ls；平均 %.2f，最低秒帧数 %u，跳帧 %llu，有效秒数 %u\r\n",
        label,size,mbps,t->width,t->height,status,mph_quality_metrics_average(&t->metrics),t->metrics.count?t->metrics.minimum:0,t->metrics.skipped,t->metrics.count);
    if(wcslen(report)+wcslen(row)<8192)wcscat(report,row);
}
static DWORD WINAPI run(void *unused) {
    (void)unused;
    /* A finite quality ladder: resolution first, then native-resolution bitrate.
     * It is NOT a claim of an absolute hardware or perceptual-quality maximum. */
    const unsigned sizes[]={720,1080,1440,1920,2560,0,0,0,0};
    const unsigned bitrates[]={4,6,8,10,12,16,20,24,32};
    const unsigned count=sizeof(sizes)/sizeof(sizes[0]);
    wcscpy(report,L"固定 60 FPS 画质搜索（H.264）\r\n每档预热 5 秒，采样 30 秒；最后候选另做 120 秒复验。\r\n达标：平均至少 59.5 FPS，每秒至少 58 帧，跳帧率不超过 0.5%。30 FPS 不合格。\r\n须保持同一持续动态、源端达到 60 FPS 的游戏场景；不补帧、不重复帧凑数。\r\n\r\n");
    int best=-1;BOOL unsafe=FALSE;
    SetEnvironmentVariableW(L"MPH_BENCHMARK",L"1");
    for(unsigned i=0;i<count && !InterlockedCompareExchange(&cancelled,0,0);++i) {
        wchar_t progress[256],name[64],label[64];
        swprintf(progress,256,L"60 FPS 搜索 %u/%u：长边 %u（0=原生），%u Mbps\r\n请保持同一动态场景；未达标即停止升档。",i+1,count,sizes[i],bitrates[i]);SetDlgItemTextW(window,200,progress);
        swprintf(name,64,L"mph-quality-trial-%u.log",i+1);swprintf(label,64,L"候选 %u",i+1);
        struct quality_trial t=trial_run(sizes[i],bitrates[i],MPH_QUALITY_PROBE_SAMPLES,name);
        describe_trial(label,sizes[i],bitrates[i],&t,MPH_QUALITY_PROBE_SAMPLES);
        unsafe=t.code || t.stopped || t.stalled || t.timedout;
        if(!trial_pass(&t,MPH_QUALITY_PROBE_SAMPLES)) {wcscat(report,L"已停止升档。\r\n");break;}
        best=(int)i;
    }
    BOOL confirmed=FALSE;
    if(best>=0 && !unsafe && !InterlockedCompareExchange(&cancelled,0,0)) {
        SetDlgItemTextW(window,200,L"正在复验最后通过的候选：固定 60 FPS，连续采样 120 秒。\r\n请保持相同游戏场景，可随时停止。");
        struct quality_trial t=trial_run(sizes[best],bitrates[best],MPH_QUALITY_CONFIRM_SAMPLES,L"mph-quality-confirm.log");
        describe_trial(L"最终复验",sizes[best],bitrates[best],&t,MPH_QUALITY_CONFIRM_SAMPLES);
        confirmed=trial_pass(&t,MPH_QUALITY_CONFIRM_SAMPLES);
    }
    SetEnvironmentVariableW(L"MPH_BENCHMARK",NULL);
    if(confirmed) {wchar_t summary[256];swprintf(summary,256,L"\r\n已测候选中通过复验的最高档：长边 %u（0=原生），%u Mbps，H.264，60 FPS。\r\n",sizes[best],bitrates[best]);wcscat(report,summary);}
    else wcscat(report,L"\r\n未找到经复验稳定 60 FPS 的配置，不生成合格推荐。\r\n");
    wcscat(report,L"\r\n原画质设置保留，结果不会自动应用。\r\n源端只有 30 FPS、场景静止或切换时，降低投屏画质也不能凭空产生 60 个新画面；本测试不能独立判断低帧根因。\r\n音频和映射未参与测试，复验不保证长时间热稳定性。结果只覆盖已测候选，不代表绝对极限或客观清晰度评分。\r\n原始记录：mph-quality-trial-*.log、mph-quality-confirm.log。\r\n");
    wchar_t path[4096];mph_quality_path(path,L"mph-quality-result.txt");
    FILE *f=_wfopen(path,L"wb");if(f){fwrite(report,sizeof(wchar_t),wcslen(report),f);fclose(f);}
    PostMessageW(window,WM_APP,0,0);return 0;
}
static LRESULT CALLBACK proc(HWND w,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_COMMAND && LOWORD(wp)==100) message=WM_CLOSE;
    if(message==WM_CLOSE) {InterlockedExchange(&cancelled,1);SetDlgItemTextW(w,200,L"正在停止测试并恢复原投屏设置……");return 0;}
    if(message==WM_APP) {
        MessageBoxW(w,report,L"画质测试结果",MB_OK);
        wchar_t command[32768],exe[4096];
        if(!GetEnvironmentVariableW(L"MPH_RESUME_COMMAND",command,32768)) {mph_quality_path(exe,L"MobilePlayHelper.exe");swprintf(command,32768,L"\"%ls\"",exe);}
        STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
        if(CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) {CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
        else MessageBoxW(w,L"无法重新启动投屏，请手动运行 MobilePlayHelper.exe。原画质设置未改变。",L"画质测试",MB_OK|MB_ICONERROR);
        DestroyWindow(w);return 0;
    }
    if(message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR args,int show) {
    (void)previous;(void)args;(void)show;
    if(!MPH_QUALITY_AUTOTEST_ENABLED) {
        MessageBoxW(NULL,L"自动画质测试暂时停用，正在排查手机游戏黑屏。请使用主程序中的手动画质设置。",L"画质测试暂停",MB_OK|MB_ICONINFORMATION);
        return 0;
    }
    WNDCLASSW cls={0};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=L"MPHQualityTest";cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);cls.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClassW(&cls)) return 1;
    window=CreateWindowW(cls.lpszClassName,L"投屏画质测试",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,570,180,NULL,NULL,instance,NULL);
    if(!window) return 1;
    HWND text=CreateWindowW(L"STATIC",L"准备测试……",WS_CHILD|WS_VISIBLE,16,16,525,70,window,(HMENU)200,instance,NULL);
    HWND stop=CreateWindowW(L"BUTTON",L"停止并恢复原设置",WS_CHILD|WS_VISIBLE|WS_TABSTOP,16,92,185,30,window,(HMENU)100,instance,NULL);
    SendMessageW(text,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);SendMessageW(stop,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    ShowWindow(window,SW_SHOW);
    HANDLE worker=CreateThread(NULL,0,run,NULL,0,NULL);if(!worker) {DestroyWindow(window);return 1;}
    MSG message;while(GetMessageW(&message,NULL,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
    WaitForSingleObject(worker,1000);CloseHandle(worker);return 0;
}
