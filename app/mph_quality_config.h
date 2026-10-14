/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MPH_QUALITY_CONFIG_H
#define MPH_QUALITY_CONFIG_H
/* Quarantined after a real phone-side black-screen incident. */
#define MPH_QUALITY_AUTOTEST_ENABLED 0
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
struct mph_quality_settings {unsigned size,fps,mbps,codec;};
static inline void mph_quality_path(wchar_t *path,const wchar_t *file) {
    DWORD n=GetModuleFileNameW(NULL,path,4096);
    if(!n || n>=4000) {path[0]=0;return;}
    wchar_t *end=wcsrchr(path,L'\\');if(end) wcscpy(end+1,file);
}
static inline BOOL mph_quality_valid(struct mph_quality_settings q) {
    return (!q.size || (q.size>=480 && q.size<=8192)) &&
        (!q.fps || (q.fps>=15 && q.fps<=240)) && q.mbps>=1 && q.mbps<=100 && q.codec<=2;
}
static inline struct mph_quality_settings mph_quality_read(const wchar_t *path) {
    struct mph_quality_settings q={
        GetPrivateProfileIntW(L"video",L"size",0,path),GetPrivateProfileIntW(L"video",L"fps",0,path),
        GetPrivateProfileIntW(L"video",L"mbps",8,path),GetPrivateProfileIntW(L"video",L"codec",0,path)};
    if(!mph_quality_valid(q)) q=(struct mph_quality_settings){0,0,8,0};
    return q;
}
static inline BOOL mph_quality_write(const wchar_t *path,struct mph_quality_settings q) {
    if(!mph_quality_valid(q)) return FALSE;
    wchar_t temp[4096];if(swprintf(temp,4096,L"%ls.tmp",path)<0) return FALSE;
    FILE *f=_wfopen(temp,L"wb");if(!f) return FALSE;
    int n=fprintf(f,"[video]\r\nsize=%u\r\nfps=%u\r\nmbps=%u\r\ncodec=%u\r\n",q.size,q.fps,q.mbps,q.codec);
    int closed=fclose(f);return n>0 && !closed && MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
#endif
