# SPDX-License-Identifier: Apache-2.0
"""Check portable layout and prove its launcher preserves a replacement LGPL DLL."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import zipfile

with tempfile.TemporaryDirectory(prefix='MPH portable 中文 ') as directory:
    root = Path(directory)
    with zipfile.ZipFile(sys.argv[1]) as archive:
        names = set(archive.namelist())
        assert 'profiles/default-fps.mph' in names
        assert 'licenses/libusb-COPYING.txt' in names
        assert 'licenses/Android-platform-tools-NOTICE.txt' in names
        assert 'THIRD_PARTY_NOTICES.md' in names
        assert {'scripts/mcp_server.py', 'config/mcp.example.json', 'doc/mcp.md',
                'README.md', 'assets/screenshots/04-mapping-enabled.png'} <= names
        assert not any(name.endswith(('.log', '.csv', '.mp4')) for name in names)
        archive.extractall(root)
    # An overlay is ignored by the PE loader but changes the file hash. It tests
    # that the normal launcher runs without restoring a user-supplied DLL.
    library = root / 'dist/avutil-60.dll'
    replaced = library.read_bytes() + b'MPH library replacement preservation test'
    library.write_bytes(replaced)
    env = dict(os.environ, MPH_DIAGNOSTICS_OFF='1')
    subprocess.run([str(root / 'MobilePlayHelper.exe'), '--version'],
                   env=env, check=True, timeout=30)
    assert library.read_bytes() == replaced
print('Portable ZIP: PASS (layout, notices, native launch, modified DLL preserved)')
