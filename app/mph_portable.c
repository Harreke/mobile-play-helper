/* SPDX-License-Identifier: Apache-2.0 */
/* Single-file Windows distribution. Payload is built from an explicit manifest. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

struct payload_file { const wchar_t *path; unsigned id; DWORD size; const char *sha256; };
#include "portable_manifest.h"

static BOOL hash_bytes(const BYTE *data, DWORD size, char hex[65]) {
    BYTE digest[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,NULL,0,(BYTE *)data,size,digest,32)<0) return FALSE;
    for (unsigned i=0;i<32;++i) sprintf(hex+2*i,"%02x",digest[i]);
    return TRUE;
}
static BOOL matches(const wchar_t *path, const struct payload_file *file) {
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size; DWORD got=0; char hex[65];
    BYTE *data=NULL; BOOL ok=FALSE;
    if(GetFileSizeEx(h,&size) && size.QuadPart==file->size) {
        data=malloc(file->size?file->size:1);
        ok=data && ReadFile(h,data,file->size,&got,NULL) && got==file->size
            && hash_bytes(data,got,hex) && !strcmp(hex,file->sha256);
    }
    free(data);CloseHandle(h);return ok;
}
static BOOL directory(const wchar_t *path) {
    int result=SHCreateDirectoryExW(NULL,path,NULL);
    return result==ERROR_SUCCESS || result==ERROR_ALREADY_EXISTS || result==ERROR_FILE_EXISTS;
}
static BOOL install(const wchar_t *base, const struct payload_file *file) {
    wchar_t path[4096], parent[4096], temp[4096];
    if(swprintf(path,4096,L"%ls\\%ls",base,file->path)<0) return FALSE;
    if(matches(path,file)) return TRUE;
    wcscpy(parent,path);wchar_t *end=wcsrchr(parent,L'\\');if(!end)return FALSE;*end=0;
    if(!directory(parent))return FALSE;
    HRSRC resource=FindResourceW(NULL,MAKEINTRESOURCEW(file->id),RT_RCDATA);
    if(!resource)return FALSE;
    DWORD packed=SizeofResource(NULL,resource);
    const BYTE *data=LockResource(LoadResource(NULL,resource));
    BYTE *plain=malloc(file->size?file->size:1);DECOMPRESSOR_HANDLE decoder=NULL;
    SIZE_T actual=0;char hex[65];
    BOOL ok=data && plain && CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,NULL,&decoder);
    if(ok) ok=Decompress(decoder,(void *)data,packed,plain,file->size,&actual) && actual==file->size
        && hash_bytes(plain,file->size,hex) && !strcmp(hex,file->sha256);
    if(decoder)CloseDecompressor(decoder);
    if(ok) {
        ok=swprintf(temp,4096,L"%ls.%lu.tmp",path,GetCurrentProcessId())>0;
        if(ok) {
            HANDLE h=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
            DWORD written=0;
            ok=h!=INVALID_HANDLE_VALUE;
            if(ok){ok=WriteFile(h,plain,file->size,&written,NULL) && written==file->size && FlushFileBuffers(h);CloseHandle(h);}
            if(ok)ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            if(!ok)DeleteFileW(temp);
        }
    }
    free(plain);return ok;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR args,int show) {
    (void)instance;(void)previous;(void)show;
    BOOL extract_only=!wcscmp(args,L"--mph-extract-only");
    wchar_t home[4096],base[4096],profiles[4096],profile[4096],source[4096],exe[4096];
    DWORD n=GetEnvironmentVariableW(L"MPH_PORTABLE_HOME",home,4096);
    if(!n) {
        if(FAILED(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA,NULL,SHGFP_TYPE_CURRENT,home)))return 1;
        if(wcslen(home)>3900)return 1;
        wcscat(home,L"\\MobilePlayHelper\\Standalone");
    } else if(n>=3900 || !((home[0] && home[1]==L':') || (home[0]==L'\\' && home[1]==L'\\')))return 1;
    if(swprintf(base,4096,L"%ls\\runtime\\%ls",home,MPH_PAYLOAD_VERSION)<0)return 1;
    /* Serialize extraction and child execution; never overwrite a running cache. */
    HANDLE mutex=CreateMutexW(NULL,FALSE,L"Local\\MobilePlayHelper-Standalone");
    if(!mutex)return 1;
    DWORD locked=WaitForSingleObject(mutex,0);
    if(locked!=WAIT_OBJECT_0 && locked!=WAIT_ABANDONED) {
        if(!extract_only)MessageBoxW(NULL,L"独立版已在运行或正在启动。",L"MobilePlayHelper",MB_OK);
        CloseHandle(mutex);return 2;
    }
    DWORD code=1;
    for(unsigned i=0;i<sizeof(payload)/sizeof(payload[0]);++i) {
        if(!install(base,&payload[i]))goto failed;
    }
    if(swprintf(profiles,4096,L"%ls\\profiles",home)<0 || !directory(profiles))goto failed;
    if(swprintf(profile,4096,L"%ls\\default-fps.mph",profiles)<0
        || swprintf(source,4096,L"%ls\\default-fps.mph",base)<0)goto failed;
    if(!CopyFileW(source,profile,TRUE) && GetLastError()!=ERROR_FILE_EXISTS)goto failed;
    if(extract_only){code=0;goto done;}
    if(!SetEnvironmentVariableW(L"MPH_PROFILE",profile))goto failed;
    if(swprintf(exe,4096,L"%ls\\MobilePlayHelper.exe",base)<0)goto failed;
    size_t length=wcslen(exe)+wcslen(args)+8;
    wchar_t *command=calloc(length,sizeof(wchar_t));if(!command)goto failed;
    swprintf(command,length,L"\"%ls\" %ls",exe,args);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION child={0};
    BOOL ok=CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,base,&startup,&child);
    free(command);if(!ok)goto failed;
    CloseHandle(child.hThread);WaitForSingleObject(child.hProcess,INFINITE);
    GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hProcess);goto done;
failed:
    if(!extract_only)MessageBoxW(NULL,L"独立版启动失败：无法释放或校验运行文件。\n请检查用户数据目录是否可写、磁盘空间以及安全软件的拦截记录。",L"MobilePlayHelper",MB_OK|MB_ICONERROR);
done:
    ReleaseMutex(mutex);CloseHandle(mutex);return (int)code;
}
