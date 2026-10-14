/* SPDX-License-Identifier: Apache-2.0 */
/* Run only in an isolated directory: settings live beside this test EXE. */
#include "mph_quality.h"
#include "options.h"
#include "../mph_quality_config.h"
#include <assert.h>
#include <string.h>
int main(void) {
 wchar_t path[4096],backup[4096],pending[4096];
 mph_quality_path(path,L"mph-quality.ini");mph_quality_path(backup,L"mph-quality-backup.ini");mph_quality_path(pending,L"mph-quality.pending");
 struct mph_quality_settings old={1080,60,8,0},next={1440,90,20,1};
 assert(mph_quality_write(path,next));assert(mph_quality_write(backup,old));assert(mph_quality_write(pending,next));
 struct scrcpy_options opts={0};mph_quality_load(&opts);
 assert(opts.max_size==1440 && opts.video_bit_rate==20000000 && opts.video_codec==SC_CODEC_H265 && !strcmp(opts.max_fps,"90"));
 assert(mph_quality_finish(1)==1);struct mph_quality_settings restored=mph_quality_read(path);
 assert(restored.size==1080 && restored.fps==60 && restored.mbps==8 && restored.codec==0);
 assert(GetFileAttributesW(pending)==INVALID_FILE_ATTRIBUTES);
 assert(mph_quality_write(pending,next));mph_quality_connected();assert(GetFileAttributesW(pending)==INVALID_FILE_ATTRIBUTES);
 assert(mph_quality_finish(0)==0);
 DeleteFileW(path);DeleteFileW(backup);
 puts("Quality settings: PASS (load/rollback/pending/connected)");return 0;
}
