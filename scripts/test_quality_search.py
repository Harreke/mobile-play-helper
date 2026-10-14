# SPDX-License-Identifier: Apache-2.0
"""Exercise the real search worker with a fake client, without ADB or a phone."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parents[1]
os.chdir(root)
folder=root/'work/quality-search-test';folder.mkdir(exist_ok=True)
(folder/'driver.c').write_text('#define wWinMain unused_gui_main\n#include "mph_quality_test.c"\nint main(void){return (int)run(NULL);}\n')
(folder/'fake.c').write_text(r"""
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int WINAPI WinMain(HINSTANCE a,HINSTANCE b,LPSTR args,int show) {
 (void)a;(void)b;(void)show;
 if(!strstr(args,"--max-fps=60 "))return 2;
 char *time=strstr(args,"--time-limit=");if(!time)return 3;
 unsigned duration=(unsigned)atoi(time+13);
 const char *scenario=getenv("MPH_FAKE_FPS");unsigned fps=scenario?(unsigned)atoi(scenario):30;
 FILE *f=fopen("runs.txt","a");if(!f)return 4;fprintf(f,"%u\n",duration);fclose(f);
 f=fopen("MobilePlayHelper.log","w");if(!f)return 5;
 fprintf(f,"INFO: Texture: 1080x488\n");
 for(unsigned i=0;i+1<duration;++i)fprintf(f,"INFO: %u fps\n",fps);
 fclose(f);return 0;
}
""")
cc=str(root/'work/w64devkit/bin/gcc.exe')
subprocess.run([cc,'-std=c11','-O2','-Iapp',str(folder/'driver.c'),'-lgdi32','-o',str(folder/'search.exe')],check=True)
subprocess.run([cc,'-O2','-mwindows',str(folder/'fake.c'),'-o',str(folder/'MobilePlayHelper.exe')],check=True)
for fps in (30,60):
 (folder/'runs.txt').write_text('')
 env=dict(os.environ,MPH_FAKE_FPS=str(fps))
 subprocess.run([str(folder/'search.exe')],cwd=folder,env=env,check=True,timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
 runs=[int(s) for s in (folder/'runs.txt').read_text().splitlines()]
 report=(folder/'mph-quality-result.txt').read_text(encoding='utf-16-le')
 if fps==30:
  assert runs==[36],runs
  assert '未找到经复验稳定 60 FPS' in report
 else:
  assert runs==[36]*9+[126],runs
  assert '通过复验的最高档' in report
 print(f'{fps} FPS search: PASS; runs={len(runs)}; no real device involved')
