/* SPDX-License-Identifier: Apache-2.0 */
/* Windows sidecar: bounded files, bounded adb commands, no UI thread work. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#define LIMIT (4u*1024u*1024u)
#define KEEP 6
static wchar_t base[32768],folder[32768],adb[32768],serial[1024];
static HANDLE parent;
static HANDLE client_log=INVALID_HANDLE_VALUE;
static DWORD client_pid;
static void timestamp(FILE *f,const char *label) {
    SYSTEMTIME t;GetLocalTime(&t);
    fprintf(f,"\n[%04u-%02u-%02uT%02u:%02u:%02u.%03u local] %s\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,label);
}
static FILE *open_log(const wchar_t *name) {
    wchar_t path[32768],old[32768];swprintf(path,32768,L"%ls\\%ls",folder,name);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if(GetFileAttributesExW(path,GetFileExInfoStandard,&data) && (data.nFileSizeHigh || data.nFileSizeLow>=LIMIT)) {
        swprintf(old,32768,L"%ls.previous",path);MoveFileExW(path,old,MOVEFILE_REPLACE_EXISTING);
    }
    return _wfopen(path,L"ab");
}
static void note(const char *message) {
    FILE *f=open_log(L"memory.log");if(f) {timestamp(f,message);fclose(f);}
}
/* stdin is a fixed local shell script. stdout/stderr share a drained pipe.
 * Every child belongs to a kill-on-close job and has a deadline and output cap. */
static void capture(const wchar_t *arguments,const wchar_t *name,BOOL probe,DWORD timeout) {
    FILE *f=open_log(name);if(!f) return;
    timestamp(f,"ADB_CAPTURE_BEGIN");fflush(f);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};HANDLE rd=NULL,wr=NULL,input=INVALID_HANDLE_VALUE;
    if(!CreatePipe(&rd,&wr,&sa,0)) {fclose(f);return;}
    SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    wchar_t script[32768];swprintf(script,32768,L"%ls\\mph-phone-probe.sh",base);
    input=CreateFileW(probe?script:L"NUL",GENERIC_READ,FILE_SHARE_READ,&sa,OPEN_EXISTING,0,NULL);
    HANDLE job=CreateJobObjectW(NULL,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(input==INVALID_HANDLE_VALUE || !job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
        fprintf(f,"helper setup failed: %lu\n",GetLastError());goto cleanup;
    }
    wchar_t command[32768];
    if(swprintf(command,32768,L"\"%ls\" -s \"%ls\" %ls",adb,serial,arguments)<0) goto cleanup;
    STARTUPINFOEXW si={0};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput=input;si.StartupInfo.hStdOutput=wr;si.StartupInfo.hStdError=wr;
    SIZE_T attribute_bytes=0;InitializeProcThreadAttributeList(NULL,1,0,&attribute_bytes);
    si.lpAttributeList=HeapAlloc(GetProcessHeap(),0,attribute_bytes);
    if(!si.lpAttributeList) goto cleanup;
    if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attribute_bytes)) {
        HeapFree(GetProcessHeap(),0,si.lpAttributeList);goto cleanup;
    }
    HANDLE inherited[]={input,wr};PROCESS_INFORMATION pi={0};
    BOOL created=FALSE;
    if(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL))
        created=CreateProcessW(NULL,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,base,&si.StartupInfo,&pi);
    DWORD launch_error=GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);HeapFree(GetProcessHeap(),0,si.lpAttributeList);
    if(!created) {fprintf(f,"adb start failed: %lu\n",launch_error);goto cleanup;}

    if(!AssignProcessToJobObject(job,pi.hProcess)) {
        fprintf(f,"job assignment failed: %lu\n",GetLastError());TerminateProcess(pi.hProcess,1);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);goto cleanup;
    }
    ResumeThread(pi.hThread);CloseHandle(pi.hThread);CloseHandle(wr);wr=NULL;
    long position=ftell(f);DWORD budget=position>=0 && position<(long)LIMIT?LIMIT-(DWORD)position:0;
    ULONGLONG start=GetTickCount64();DWORD written=0;BOOL timedout=FALSE;
    for(;;) {
        DWORD available=0;
        if(!PeekNamedPipe(rd,NULL,0,NULL,&available,NULL)) break;
        if(available) {
            char buffer[8192];DWORD got=0;
            if(!ReadFile(rd,buffer,available<sizeof(buffer)?available:sizeof(buffer),&got,NULL)) break;
            if(written<budget) {DWORD n=got<budget-written?got:budget-written;fwrite(buffer,1,n,f);fflush(f);written+=n;}
        } else if(WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0) break;
        else Sleep(30);
        if(GetTickCount64()-start>timeout || written>=budget) {timedout=TRUE;TerminateJobObject(job,1);break;}
    }
    if(WaitForSingleObject(pi.hProcess,1000)!=WAIT_OBJECT_0) TerminateJobObject(job,1);
    DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);
    fprintf(f,"\nADB_CAPTURE_END exit=%lu elapsed_ms=%llu truncated_or_timeout=%d\n",code,GetTickCount64()-start,timedout);
    CloseHandle(pi.hProcess);
cleanup:
    if(job) CloseHandle(job);
    if(input!=INVALID_HANDLE_VALUE) CloseHandle(input);
    if(wr) CloseHandle(wr);
    if(rd) CloseHandle(rd);
    fclose(f);
}
static DWORD WINAPI event_worker(void *unused) {
    (void)unused;
    while(WaitForSingleObject(parent,0)==WAIT_TIMEOUT) {
        /* Stream while connected instead of relying on a small last-N snapshot. */
        capture(L"logcat -v threadtime -b main -b system -b crash -b events -T 100 MPH_DIAG:I ActivityManager:I lmkd:V lowmemorykiller:V am_kill:I am_proc_died:I am_crash:I am_low_memory:I AndroidRuntime:E DEBUG:E MediaCodec:W CCodec:W ACodec:W scrcpy:V *:S",L"events.log",FALSE,30000);
        if(WaitForSingleObject(parent,1000)!=WAIT_TIMEOUT) break;
    }
    return 0;
}
static void pc_sample(void) {
    FILE *f=open_log(L"memory.log");if(!f) return;
    timestamp(f,"PC_MEMORY bytes");
    PROCESS_MEMORY_COUNTERS_EX c={0};c.cb=sizeof(c);
    if(GetProcessMemoryInfo(parent,(PROCESS_MEMORY_COUNTERS *)&c,sizeof(c)))
        fprintf(f,"client_pid=%lu working_set=%llu private_commit=%llu peak_working_set=%llu page_faults=%lu\n",client_pid,(unsigned long long)c.WorkingSetSize,(unsigned long long)c.PrivateUsage,(unsigned long long)c.PeakWorkingSetSize,c.PageFaultCount);
    else fprintf(f,"client_memory_unavailable=%lu\n",GetLastError());
    if(GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&c,sizeof(c)))
        fprintf(f,"diagnostics_working_set=%llu diagnostics_private_commit=%llu\n",(unsigned long long)c.WorkingSetSize,(unsigned long long)c.PrivateUsage);
    DWORD handles=0;
    if(GetProcessHandleCount(parent,&handles)) fprintf(f,"client_handles=%lu gdi_objects=%lu user_objects=%lu\n",handles,GetGuiResources(parent,GR_GDIOBJECTS),GetGuiResources(parent,GR_USEROBJECTS));
    MEMORYSTATUSEX m={0};m.dwLength=sizeof(m);
    if(GlobalMemoryStatusEx(&m)) fprintf(f,"pc_memory_load_percent=%lu physical_total=%llu physical_available=%llu commit_available=%llu\n",m.dwMemoryLoad,m.ullTotalPhys,m.ullAvailPhys,m.ullAvailPageFile);
    fclose(f);
}
/* Delete only our known log files, never a recursive or arbitrary directory. */
static void retention(const wchar_t *root) {
    wchar_t pattern[32768];swprintf(pattern,32768,L"%ls\\session-*",root);
    WIN32_FIND_DATAW d;HANDLE search=FindFirstFileW(pattern,&d);if(search==INVALID_HANDLE_VALUE) return;
    wchar_t names[128][96];unsigned count=0;
    do {
        if(!(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (d.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if(wcslen(d.cFileName)>=96 || wcsspn(d.cFileName+8,L"0123456789-")!=wcslen(d.cFileName+8)) continue;
        if(count<128) wcscpy(names[count++],d.cFileName);
    } while(FindNextFileW(search,&d));FindClose(search);
    for(unsigned i=0;i<count;++i) for(unsigned j=i+1;j<count;++j) if(wcscmp(names[i],names[j])>0) {
        wchar_t tmp[96];wcscpy(tmp,names[i]);wcscpy(names[i],names[j]);wcscpy(names[j],tmp);
    }
    const wchar_t *files[]={L"memory.log",L"phone.log",L"detail.log",L"events.log",L"client.log"};
    for(unsigned i=0;i+KEEP<count;++i) {
        wchar_t directory[32768];swprintf(directory,32768,L"%ls\\%ls",root,names[i]);
        for(unsigned j=0;j<5;++j) {
            wchar_t path[32768];swprintf(path,32768,L"%ls\\%ls",directory,files[j]);DeleteFileW(path);
            swprintf(path,32768,L"%ls\\%ls.previous",directory,files[j]);DeleteFileW(path);
        }
        RemoveDirectoryW(directory);
    }
}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE prev,PWSTR cmd,int show) {
    (void)h;(void)prev;(void)cmd;(void)show;
    int argc;wchar_t **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv || argc!=4) return 1;
    client_pid=wcstoul(argv[1],NULL,10);
    if(!client_pid || wcslen(argv[2])>=1024 || wcslen(argv[3])>=32768 || wcschr(argv[2],L'"') || wcschr(argv[3],L'"')) return 1;
    wcscpy(serial,argv[2]);wcscpy(adb,argv[3]);LocalFree(argv);
    parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,client_pid);
    if(!parent) return 1;
    DWORD n=GetModuleFileNameW(NULL,base,32768);if(!n || n>32000) return 1;
    wchar_t *slash=wcsrchr(base,L'\\');if(!slash) return 1;*slash=0;
    wchar_t root[32768];swprintf(root,32768,L"%ls\\diagnostics",base);CreateDirectoryW(root,NULL);
    SYSTEMTIME t;GetLocalTime(&t);
    swprintf(folder,32768,L"%ls\\session-%04u%02u%02u-%02u%02u%02u-%lu",root,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,client_pid);
    if(!CreateDirectoryW(folder,NULL)) return 1;
    wchar_t client_path[32768];swprintf(client_path,32768,L"%ls\\MobilePlayHelper.log",base);
    client_log=CreateFileW(client_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    retention(root);SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    note("diagnostics v1: PC 5s; phone 30s; detailed 120s; missing/permission-denied metrics are UNKNOWN, not zero. Times are local on each host. Files rotate at 4 MiB, 6 sessions retained.");
    HANDLE events=CreateThread(NULL,0,event_worker,NULL,0,NULL);
    if(!events) note("EVENT_STREAM_START_FAILED");
    ULONGLONG next_phone=0,next_detail=0;
    while(WaitForSingleObject(parent,0)==WAIT_TIMEOUT) {
        pc_sample();ULONGLONG now=GetTickCount64();
        if(now>=next_phone) {
            capture(L"shell sh -s -- quick",L"phone.log",TRUE,10000);

            next_phone=GetTickCount64()+30000;
        }
        if(now>=next_detail) {capture(L"shell sh -s -- detail",L"detail.log",TRUE,15000);next_detail=GetTickCount64()+120000;}
        if(WaitForSingleObject(parent,5000)!=WAIT_TIMEOUT) break;
    }
    if(events) {WaitForSingleObject(events,INFINITE);CloseHandle(events);}
    DWORD exitcode=0;GetExitCodeProcess(parent,&exitcode);
    char message[128];snprintf(message,sizeof(message),"CLIENT_EXIT code=%lu (0x%08lX); collecting final phone evidence",exitcode,exitcode);note(message);
    capture(L"shell sh -s -- detail",L"detail.log",TRUE,15000);
    capture(L"logcat -d -v threadtime -b main -b system -b crash -b events -t 1200 MPH_DIAG:I ActivityManager:I lmkd:V lowmemorykiller:V am_kill:I am_proc_died:I am_crash:I am_low_memory:I AndroidRuntime:E DEBUG:E MediaCodec:W CCodec:W ACodec:W scrcpy:V *:S",L"events.log",FALSE,5000);
    /* Bound the archived client log too. */
    FILE *dst=open_log(L"client.log");
    if(client_log!=INVALID_HANDLE_VALUE && dst) {
        LARGE_INTEGER size,offset;
        if(GetFileSizeEx(client_log,&size)) {
            offset.QuadPart=size.QuadPart>LIMIT?size.QuadPart-LIMIT:0;
            SetFilePointerEx(client_log,offset,NULL,FILE_BEGIN);
            char data[8192];DWORD got;
            while(ReadFile(client_log,data,sizeof(data),&got,NULL) && got) fwrite(data,1,got,dst);
        }
    }
    if(client_log!=INVALID_HANDLE_VALUE) CloseHandle(client_log);
    if(dst) fclose(dst);
    note("DIAGNOSTICS_FINISHED");CloseHandle(parent);return 0;
}
