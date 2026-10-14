# SPDX-License-Identifier: Apache-2.0
"""Test the real packaged EXE in isolation; never connect to a phone."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
manifest = json.loads(Path(sys.argv[2]).read_text())
with tempfile.TemporaryDirectory(prefix='MPH 中文 独立包 ') as sandbox:
    home = Path(sandbox)
    env = dict(os.environ, MPH_PORTABLE_HOME=str(home))
    def extract():
        subprocess.run([str(exe), '--mph-extract-only'], env=env, check=True, timeout=60)
    extract()
    versions = list((home / 'runtime').iterdir())
    assert len(versions) == 1
    runtime = versions[0]
    assert {p.relative_to(runtime).as_posix() for p in runtime.rglob('*') if p.is_file()} == {f['path'] for f in manifest}
    for item in manifest:
        assert hashlib.sha256((runtime / item['path']).read_bytes()).hexdigest() == item['sha256']
    profile = home / 'profiles/default-fps.mph'
    profile.write_text('# user edits must survive\n', encoding='utf-8')
    (runtime / 'dist/SDL3.dll').write_bytes(b'damaged cache')
    extract()
    assert profile.read_text() == '# user edits must survive\n'
    expected = next(f['sha256'] for f in manifest if f['path'] == 'dist/SDL3.dll')
    assert hashlib.sha256((runtime / 'dist/SDL3.dll').read_bytes()).hexdigest() == expected
    # --version exits before any device discovery. Check argument forwarding and DLL loading.
    subprocess.run([str(exe), '--version'], env=env, check=True, timeout=30)
print('Standalone package: PASS (Unicode path, manifest hashes, repair, profile preservation, native launch)')
