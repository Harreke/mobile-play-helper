/* SPDX-License-Identifier: Apache-2.0 */
/* Native Windows entry point; no shell or PowerShell dependency. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t directory[32768], executable[32768];
    DWORD n=GetModuleFileNameW(NULL,directory,32768);
    if(!n || n>=32768) return 1;
    wchar_t *slash=wcsrchr(directory,L'\\');
    if(!slash) return 1;
    *slash=0;
    if(swprintf(executable,32768,L"%ls\\dist\\MobilePlayHelper.exe",directory)<0) return 1;
    size_t length=wcslen(executable)+wcslen(args)+8;
    wchar_t *command=calloc(length,sizeof(wchar_t));
    if(!command) return 1;
    swprintf(command,length,L"\"%ls\" %ls",executable,args);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);
    PROCESS_INFORMATION process={0};
    BOOL ok=CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,directory,&startup,&process);
    free(command);
    if(!ok) {
        MessageBoxW(NULL,L"无法启动 MobilePlayHelper。请保留 EXE 旁边的 dist 文件夹，以及其中的 DLL 和 scrcpy-server。",L"MobilePlayHelper",MB_OK|MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    /* A separate process can report even an unhandled client crash. */
    wchar_t logpath[32768];
    if(swprintf(logpath,32768,L"%ls\\dist\\MobilePlayHelper-launcher.log",directory)>0) {
        FILE *log=_wfopen(logpath,L"a");
        if(log) {
            SYSTEMTIME now;GetLocalTime(&now);
            fprintf(log,"%04u-%02u-%02u %02u:%02u:%02u client_pid=%lu exit_code=%lu (0x%08lX)\n",
                now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,
                process.dwProcessId,code,code);
            fclose(log);
        }
    }
    if(code>2) {
        wchar_t message[512];
        swprintf(message,512,L"MobilePlayHelper 非正常退出（代码 0x%08lX）。\n请保留 dist 中的 MobilePlayHelper.log、MobilePlayHelper.log.previous 和 MobilePlayHelper-launcher.log 以便排查。",code);
        MessageBoxW(NULL,message,L"MobilePlayHelper",MB_OK|MB_ICONERROR);
    }
    return (int)code;
}
