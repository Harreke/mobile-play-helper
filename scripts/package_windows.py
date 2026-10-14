# SPDX-License-Identifier: Apache-2.0
"""Build a native self-contained Windows executable. No personal data is packed."""
from pathlib import Path
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
import subprocess
import sys
import zipfile
from prepare_release_licenses import prepare

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
parser = argparse.ArgumentParser()
parser.add_argument('--skip-build', action='store_true', help='Package the current verified build')
args = parser.parse_args()
source_archive = prepare()
if not args.skip_build:
    subprocess.run([sys.executable, 'scripts/build_windows.py'], check=True)
# The build script leaves replacements in work if an executable is in use.
for fresh, deployed in [('work/MobilePlayHelper-new.exe', 'dist/MobilePlayHelper.exe'),
                        ('work/MobilePlayHelper-launcher-new.exe', 'MobilePlayHelper.exe')]:
    if Path(fresh).exists() and Path(fresh).read_bytes() != Path(deployed).read_bytes():
        raise SystemExit('Close MobilePlayHelper and build again: the deployed executable is older than the build.')

stage = ROOT / 'work/standalone-package'
stage.mkdir(parents=True, exist_ok=True)
out = ROOT / 'packages'
out.mkdir(exist_ok=True)
runtime = ['MobilePlayHelper.exe', 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll',
           'SDL3.dll', 'avcodec-62.dll', 'avformat-62.dll', 'avutil-60.dll',
           'swresample-6.dll', 'libusb-1.0.dll', 'scrcpy-server', 'scrcpy.png',
           'disconnected.png', 'mph-diagnostics.exe', 'mph-phone-probe.sh', 'mph-quality-test.exe']
files = {f'dist/{name}': ROOT / 'dist' / name for name in runtime}
files.update({'MobilePlayHelper.exe': ROOT / 'MobilePlayHelper.exe',
              'default-fps.mph': ROOT / 'assets/default-fps.mph',
              'LICENSE.txt': ROOT / 'LICENSE', 'NOTICE.txt': ROOT / 'NOTICE'})
for path in sorted((ROOT / 'licenses').iterdir()):
    if path.is_file():
        files['licenses/' + path.name] = path
for name in ['README.md', 'README.scrcpy.md', 'doc/mapping-format.md',
             'doc/recording-diagnostics.md', 'doc/memory-diagnostics.md',
             'doc/mobileplayhelper-architecture.md',
             'THIRD_PARTY_NOTICES.md', 'doc/privacy.md', 'doc/windows-distribution.md',
             'doc/third-party-build.md', 'config/third-party-lock.json',
             'scripts/mcp_server.py', 'config/mcp.example.json', 'doc/mcp.md']:
    files[name] = ROOT / name

for path in sorted((ROOT / 'assets/screenshots').glob('*.png')):
    files['assets/screenshots/' + path.name] = path

cabinet = C.WinDLL('cabinet', use_last_error=True)
cabinet.CreateCompressor.argtypes = [W.DWORD, C.c_void_p, C.POINTER(W.HANDLE)]
cabinet.CreateCompressor.restype = W.BOOL
cabinet.Compress.argtypes = [W.HANDLE, C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
cabinet.Compress.restype = W.BOOL
cabinet.CloseCompressor.argtypes = [W.HANDLE]

def compress(data):
    handle = W.HANDLE()
    if not cabinet.CreateCompressor(4, None, C.byref(handle)):
        raise C.WinError(C.get_last_error())
    try:
        source = C.create_string_buffer(data)
        required = C.c_size_t()
        cabinet.Compress(handle, source, len(data), None, 0, C.byref(required))
        if C.get_last_error() != 122 or not required.value:
            raise C.WinError(C.get_last_error())
        target = C.create_string_buffer(required.value)
        if not cabinet.Compress(handle, source, len(data), target, len(target), C.byref(required)):
            raise C.WinError(C.get_last_error())
        return target.raw[:required.value]
    finally:
        cabinet.CloseCompressor(handle)

manifest = []
resources = []
entries = []
for resource_id, (relative, path) in enumerate(sorted(files.items()), 100):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    manifest.append({'path': relative, 'bytes': len(data), 'sha256': digest})
    packed = stage / f'{resource_id}.bin'
    packed.write_bytes(compress(data))
    resources.append(f'{resource_id} RCDATA "{packed.as_posix()}"')
    name = json.dumps(relative.replace('/', '\\'))
    entries.append(f'    {{L{name}, {resource_id}, {len(data)}, "{digest}"}},')
version = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:24]
(stage / 'portable_manifest.h').write_text(
    f'#define MPH_PAYLOAD_VERSION L"{version}"\nstatic const struct payload_file payload[] = {{\n'
    + '\n'.join(entries) + '\n};\n', encoding='utf-8')
(stage / 'payload.rc').write_text('\n'.join(resources), encoding='utf-8')
cc = ROOT / 'work/w64devkit/bin/gcc.exe'
subprocess.run([str(cc.with_name('windres.exe')), '-i', str(stage / 'payload.rc'),
                '-o', str(stage / 'payload.o')], check=True)
temporary = out / 'MobilePlayHelper-Windows-x64.building.exe'
subprocess.run([str(cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-municode', '-mwindows',
                '-I' + str(stage), 'app/mph_portable.c', str(stage / 'payload.o'), 'work/mph-resource.o',
                '-lcabinet', '-lbcrypt', '-lshell32', '-o', str(temporary)], check=True)
# Validate the exact file before replacing the last successful release.
(stage / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
subprocess.run([sys.executable, 'scripts/test_package.py', str(temporary), str(stage / 'manifest.json')], check=True)
release = out / 'MobilePlayHelper-Windows-x64.exe'
temporary.replace(release)
(out / 'package-manifest.json').write_text(json.dumps({'version': version, 'files': manifest}, indent=2), encoding='utf-8')
portable = out / 'MobilePlayHelper-Windows-x64-portable.zip'
portable_temporary = portable.with_suffix('.building.zip')
with zipfile.ZipFile(portable_temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
    for relative, path in sorted(files.items()):
        archive.write(path, relative)
    archive.write(ROOT / 'assets/default-fps.mph', 'profiles/default-fps.mph')
subprocess.run([sys.executable, 'scripts/test_portable_package.py', str(portable_temporary)], check=True)
portable_temporary.replace(portable)
(out / 'SHA256.txt').write_text(''.join(
    hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name + '\n'
    for path in [release, portable, source_archive]), encoding='utf-8')
print(f'Packaged: {release} ({release.stat().st_size / 1048576:.1f} MiB)', flush=True)
