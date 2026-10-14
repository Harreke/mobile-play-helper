# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import re,subprocess,shutil,os,argparse,hashlib,urllib.request,tarfile

root=Path(__file__).resolve().parents[1];os.chdir(root)
parser=argparse.ArgumentParser(description="Build the MobilePlayHelper Windows x64 client")
parser.add_argument('--runtime-dir',default='dist',help='Official scrcpy 4.1 Windows x64 folder (ADB, server, DLLs)')
args=parser.parse_args()
out=root/'dist';out.mkdir(exist_ok=True);Path('work').mkdir(exist_ok=True)
assets=[
 ('toolchain.exe','https://github.com/skeeto/w64devkit/releases/download/v2.10.0/w64devkit-x64-2.10.0.7z.exe','18d0a4c71a166f8401ab6305781bec5882b40b5e06ba9807c61cb5f3b3c6325e'),
 ('sdl.tar.gz','https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz','ea8071241b934e1feec0337f7d78807a5004de9a500dba1942aaf615a988d7a2'),
 ('ffmpeg.tar.xz','https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz','464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c'),
 ('libusb.h','https://raw.githubusercontent.com/libusb/libusb/v1.0.30/libusb/libusb.h','a61260ab145b051b86df2b0575956f01810190abe2c7df6aca831c33bdc8082c')]
for name,url,digest in assets:
 path=Path('work')/name
 if not path.exists():
  print('Downloading',url,flush=True)
  req=urllib.request.Request(url,headers={'User-Agent':'MobilePlayHelper-build'})
  with urllib.request.urlopen(req) as source,path.open('wb') as target: shutil.copyfileobj(source,target)
 if hashlib.sha256(path.read_bytes()).hexdigest()!=digest: raise RuntimeError('SHA256 mismatch: '+name)
if not Path('work/w64devkit/bin/gcc.exe').exists(): subprocess.run(['work/toolchain.exe','-y','-owork'],check=True)
for archive,folder in [('sdl.tar.gz','SDL3-3.4.12'),('ffmpeg.tar.xz','ffmpeg-8.1.2')]:
 if not (Path('work')/folder).exists():
  with tarfile.open(Path('work')/archive) as t: t.extractall('work',filter='data')
runtime=Path(args.runtime_dir).resolve()
required=['adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll','scrcpy-server','SDL3.dll','avcodec-62.dll','avformat-62.dll','avutil-60.dll','swresample-6.dll','libusb-1.0.dll','scrcpy.png','disconnected.png']
for name in required:
 p=runtime/name
 if not p.is_file(): raise FileNotFoundError(f'Missing official scrcpy 4.1 runtime file: {p}')
 if p.resolve()!=(out/name).resolve(): shutil.copy2(p,out/name)
Path('work/config.h').write_text('''#define SCRCPY_VERSION "4.1"
#define PREFIX "."
#define PORTABLE 1
#define DEFAULT_LOCAL_PORT_RANGE_FIRST 27183
#define DEFAULT_LOCAL_PORT_RANGE_LAST 27199
#define HAVE_USB 1
#define HAVE_STRDUP 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
''')
Path('work/ffmpeg-8.1.2/libavutil/avconfig.h').write_text('#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n')
s=Path('app/meson.build').read_text().split('feature_test_macros')[0]
sources=['app/'+x for x in re.findall("'([^']+\\.c)'",s)]+['app/src/util/command.c','app/src/sys/win/file.c','app/src/sys/win/process.c']
Path('work/libusb-1.0').mkdir(exist_ok=True)
shutil.copy2('work/libusb.h','work/libusb-1.0/libusb.h')
sources += [str(p) for p in Path('app/src/usb').glob('*.c')]
subprocess.run([str(root/'work/w64devkit/bin/windres.exe'),'-i','scrcpy-windows.rc','-o',str(root/'work/mph-resource.o')],cwd=root/'app',check=True)
cmd=['work/w64devkit/bin/gcc.exe','-std=c11','-O2','-DNDEBUG','-g','-Wall','-Wextra','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L','-D_WIN32_WINNT=0x0600','-DWINVER=0x0600','-Iwork','-Iapp/src','-Iwork/SDL3-3.4.12/x86_64-w64-mingw32/include','-Iwork/ffmpeg-8.1.2',*sources,*map(str,out.glob('av*.dll')),str(out/'swresample-6.dll'),str(out/'SDL3.dll'),str(out/'libusb-1.0.dll'),'work/mph-resource.o','-mwindows','-ldwmapi','-lgdi32','-lcomdlg32','-lws2_32','-lmingw32','-lm','-o',str(root/'work/MobilePlayHelper-new.exe')]
subprocess.run(cmd,check=True)

try:
 shutil.copy2(root/'work/MobilePlayHelper-new.exe',out/'MobilePlayHelper.exe')
except PermissionError:
 print('App is running; compiled replacement is work/MobilePlayHelper-new.exe')

shutil.copy2('LICENSE',out/'LICENSE.txt')
shutil.copy2('NOTICE',out/'NOTICE.txt')
print('Built:',out/'MobilePlayHelper.exe')

subprocess.run(['work/w64devkit/bin/gcc.exe','-O2','-Wall','-Wextra','-municode','-mwindows','app/mph_launcher.c','work/mph-resource.o','-o','work/MobilePlayHelper-launcher-new.exe'],check=True)
print('GUI entry:',root/'MobilePlayHelper.exe')

try:
 shutil.copy2('work/MobilePlayHelper-launcher-new.exe','MobilePlayHelper.exe')
except PermissionError:
 print('Launcher is running; replacement is ready in work/')

subprocess.run(['work/w64devkit/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode','-mwindows','app/mph_diagnostics.c','-lpsapi','-lshell32','-o','work/mph-diagnostics-new.exe'],check=True)
shutil.copy2('work/mph-diagnostics-new.exe',out/'mph-diagnostics.exe')
# Android shell requires LF, independent of the Windows checkout newline setting.
(out/'mph-phone-probe.sh').write_bytes(Path('scripts/mph-phone-probe.sh').read_text(encoding='utf-8').encode('utf-8'))

subprocess.run(['work/w64devkit/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-municode','-mwindows','app/mph_quality_test.c','-lgdi32','-o','work/mph-quality-test-new.exe'],check=True)
shutil.copy2('work/mph-quality-test-new.exe',out/'mph-quality-test.exe')
