# SPDX-License-Identifier: Apache-2.0
"""Verify runtime provenance and build the matching third-party source archive."""
from pathlib import Path
import hashlib
import json
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def prepare():
    lock = json.loads((ROOT / 'config/third-party-lock.json').read_text())
    cache = ROOT / 'work/compliance'
    cache.mkdir(parents=True, exist_ok=True)
    for entry in lock['archives']:
        path = cache / entry['name']
        existing = ROOT / 'work' / entry['name']
        if not path.exists() and existing.is_file():
            shutil.copyfile(existing, path)
        if not path.exists():
            temporary = path.with_suffix(path.suffix + '.download')
            with urllib.request.urlopen(entry['url'], timeout=60) as response:
                with temporary.open('wb') as output:
                    shutil.copyfileobj(response, output)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != entry['sha256']:
                raise RuntimeError('Downloaded archive hash mismatch: ' + entry['name'])
            temporary.replace(path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError('Archive hash mismatch: ' + entry['name'])
    with zipfile.ZipFile(cache / 'scrcpy-win64-v4.1.zip') as official:
        for entry in lock['runtime']:
            data = (ROOT / 'dist' / entry['name']).read_bytes()
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise RuntimeError('Runtime differs from reviewed release: ' + entry['name'])
            if data != official.read(entry['upstream_member']):
                raise RuntimeError('Runtime differs from official archive: ' + entry['name'])
    required = ['SDL-LICENSE.txt', 'libusb-COPYING.txt', 'dav1d-COPYING.txt',
                'zlib-LICENSE.txt', 'Android-platform-tools-NOTICE.txt',
                'FFmpeg-COPYING.LGPLv2.1', 'FFmpeg-LICENSE.md']
    for name in required:
        if not (ROOT / 'licenses' / name).is_file():
            raise RuntimeError('Missing license: ' + name)
    output = ROOT / 'packages'
    output.mkdir(exist_ok=True)
    target = output / 'MobilePlayHelper-third-party-sources.zip'
    temporary = target.with_suffix('.building.zip')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ['ffmpeg.tar.xz', 'libusb.tar.gz', 'dav1d.tar.gz']:
            archive.write(cache / name, 'sources/' + name)
        for path in sorted((ROOT / 'app/deps').glob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(ROOT).as_posix())
        for name in ['config/third-party-lock.json', 'doc/third-party-build.md',
                     'THIRD_PARTY_NOTICES.md', 'LICENSE', 'NOTICE',
                     'release/build_windows.sh', 'release/build_common',
                     '.github/workflows/release.yml']:
            archive.write(ROOT / name, name)
        for path in sorted((ROOT / 'licenses').iterdir()):
            if path.is_file():
                archive.write(path, path.relative_to(ROOT).as_posix())
    temporary.replace(target)
    return target


if __name__ == '__main__':
    print('Verified runtime; corresponding sources:', prepare())
