# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess,os
os.chdir(Path(__file__).resolve().parents[1])
cc='work/w64devkit/bin/gcc.exe'
subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','app/tests/test_mph_quality_metrics.c','-o','work/test_quality_metrics.exe'],check=True)
subprocess.run(['work/test_quality_metrics.exe'],check=True)
subprocess.run([cc,'-Wall','-Wextra','-Werror','-Iapp/src','app/tests/test_mph_mapper.c','app/src/mph_mapper.c','-lm','-o','work/test_mph_mapper.exe'],check=True)
subprocess.run(['work/test_mph_mapper.exe'],check=True)
subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-Iwork','-Iapp/src','-Iwork/SDL3-3.4.12/x86_64-w64-mingw32/include','-Iwork/ffmpeg-8.1.2','app/tests/test_mph_editor.c','app/src/mph_input.c','app/src/mph_recording.c','dist/avcodec-62.dll','dist/avformat-62.dll','dist/avutil-60.dll','app/src/mph_quality.c','app/src/mph_text.c','app/src/mph_mapper.c','dist/SDL3.dll','-ldwmapi','-lgdi32','-lcomdlg32','-lm','-o','work/test_mph_editor.exe'],check=True)
p=Path('work/editor-profile.mph');p.write_text('toggle 65\njoystick .16 .76 .10\nlook .65 .45 .2 .001\nbind Space .9 .73\n')
os.environ['MPH_PROFILE']=str(p.resolve())
os.environ['MPH_TEST_PROFILE']=str(Path('work/中文游戏.mph').resolve())
os.environ['PATH']=str(Path('dist').resolve())+os.pathsep+os.environ['PATH']
subprocess.run(['work/test_mph_editor.exe'],check=True)

# Test rollback without touching real settings beside dist/MobilePlayHelper.exe.
Path('work/quality-unit').mkdir(exist_ok=True)
subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-Iwork','-Iapp/src','-Iwork/SDL3-3.4.12/x86_64-w64-mingw32/include','-Iwork/ffmpeg-8.1.2','app/tests/test_mph_quality.c','app/src/mph_quality.c','dist/SDL3.dll','-lgdi32','-o','work/quality-unit/test_quality.exe'],check=True)
subprocess.run(['work/quality-unit/test_quality.exe'],check=True)

# Run the inherited scrcpy regression suite against the same headers/runtime.
import ast
build=Path('app/meson.build').read_text()
block=build.split('    tests = [',1)[1].split('    foreach t',1)[0].strip()
tests=ast.literal_eval('['+block)
common=[cc,'-std=c11','-D_GNU_SOURCE','-DSC_TEST','-Iwork','-Iapp/src','-Iwork/SDL3-3.4.12/x86_64-w64-mingw32/include','-Iwork/ffmpeg-8.1.2']
libs=[str(p) for p in Path('dist').glob('av*.dll')]+['dist/SDL3.dll','dist/swresample-6.dll','-lws2_32','-lm']
for name,sources in tests:
 target='work/'+name+'.exe'
 subprocess.run(common+['app/'+p for p in sources]+['app/src/compat.c']+libs+['-o',target],check=True)
 subprocess.run([target],check=True)
 print(name+': PASS',flush=True)

# Real screen geometry, including events received before video dimensions exist.
screen_source=Path('app/src/screen.c').read_text()
def geometry_function(name):
 start=screen_source.rfind('static ',0,screen_source.index(name+'('))
 brace=screen_source.index('{',start);depth=1;end=brace+1
 while depth:
  depth += (screen_source[end]=='{')-(screen_source[end]=='}');end+=1
 return screen_source[start:end]
Path('work/test_geometry_impl.h').write_text(geometry_function('is_optimal_size')+'\n'+geometry_function('compute_content_rect'))
subprocess.run(common+['app/tests/test_mph_screen_geometry.c','-o','work/test_screen_geometry.exe'],check=True)
subprocess.run(['work/test_screen_geometry.exe'],check=True)
