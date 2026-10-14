/* SPDX-License-Identifier: Apache-2.0 */
/* Keep diagnostics outside the video process so a client crash cannot erase it. */
#include "mph_diagnostics.h"
#include "util/log.h"
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "util/str.h"
#include "adb/adb.h"
void mph_diagnostics_start(const char *serial) {
    if (getenv("MPH_DIAGNOSTICS_OFF")) { LOGW("MPH diagnostics disabled by environment");return; }
    wchar_t path[32768],command[32768];
    DWORD n=GetModuleFileNameW(NULL,path,32768);
    if(!n || n>32000) return;
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash) return;
    wcscpy(slash+1,L"mph-diagnostics.exe");
    wchar_t *device=sc_str_to_wchars(serial),*adb=sc_str_to_wchars(sc_adb_get_executable());
    if(!device || !adb) {free(device);free(adb);return;}
    /* Arguments are quoted; device ids and executable paths must not contain quotes. */
    if(wcschr(device,L'"') || wcschr(adb,L'"')) {free(device);free(adb);return;}
    int len=swprintf(command,32768,L"\"%ls\" %lu \"%ls\" \"%ls\"",path,GetCurrentProcessId(),device,adb);
    free(device);free(adb);
    if(len<0) return;
    STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
    if(CreateProcessW(path,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) {
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        LOGI("MPH diagnostics started: PID=%lu; dist/diagnostics",pi.dwProcessId);
    } else LOGW("MPH diagnostics could not start: Windows error %lu",GetLastError());
}
#else
void mph_diagnostics_start(const char *serial) {(void)serial;}
#endif
