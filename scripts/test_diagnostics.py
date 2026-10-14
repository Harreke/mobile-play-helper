# SPDX-License-Identifier: Apache-2.0
"""Isolated native sidecar tests. No phone is contacted or manipulated."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[1]
os.chdir(root)
cc = root / 'work/w64devkit/bin/gcc.exe'
helper = root / 'work/mph-diagnostics-test.exe'
subprocess.run([str(cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-municode', '-mwindows', 'app/mph_diagnostics.c', '-lpsapi',
                '-lshell32', '-o', str(helper)], check=True)
sandbox = Path(tempfile.mkdtemp(prefix='诊断 tests ', dir=root / 'work'))
fake = sandbox / 'fake adb.c'
fake.write_text(r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    int detail=0;
    for(int i=0;i<argc;++i) if(!strcmp(argv[i],"detail")) detail=1;
    if(detail && getenv("TEST_HANG")) Sleep(60000);
    puts("fake adb: no device access");
    if(getenv("TEST_FLOOD")) {
        char data[8192];memset(data,'x',sizeof(data));
        for(int i=0;i<1200;++i) fwrite(data,1,sizeof(data),stdout);
    }
    return 0;
}
''')
fake_exe = sandbox / 'fake adb.exe'
subprocess.run([str(cc), str(fake), '-o', str(fake_exe)], check=True)
shutil.copy2(helper, sandbox / 'mph-diagnostics.exe')
(sandbox / 'mph-phone-probe.sh').write_text('echo fake\n')
(sandbox / 'MobilePlayHelper.log').write_text('synthetic client log\n')
logs = sandbox / 'diagnostics'
logs.mkdir()
for i in range(7):
    old = logs / f'session-20000101-00000{i}-1'
    old.mkdir()
    (old / 'memory.log').write_text('old fixture\n')
# Non-session folders/files must never be pruned.
(logs / 'user-notes').mkdir()
(logs / 'user-notes' / 'keep.txt').write_text('keep')

def run_case(code=0, **extra):
    (sandbox / 'MobilePlayHelper.log').write_text('synthetic client log\n')
    child = subprocess.Popen([sys.executable, '-c', f'import time;time.sleep(2);raise SystemExit({code})'])
    env = os.environ.copy()
    env.update(extra)
    started = time.monotonic()
    watch = subprocess.Popen([str(sandbox / 'mph-diagnostics.exe'), str(child.pid),
                              'test-serial', str(fake_exe)], env=env)
    child.wait(timeout=10)
    os.replace(sandbox / 'MobilePlayHelper.log', sandbox / 'previous-client.log')
    (sandbox / 'MobilePlayHelper.log').write_text('new session must not replace archived evidence\n')
    assert watch.wait(timeout=65) == 0
    session = sorted(logs.glob('session-*'))[-1]
    memory = (session / 'memory.log').read_text()
    assert f'CLIENT_EXIT code={code} ' in memory
    assert 'DIAGNOSTICS_FINISHED' in memory
    assert 'private_commit=' in memory and 'diagnostics_working_set=' in memory
    assert (session / 'client.log').read_text() == 'synthetic client log\n'
    assert len(list(logs.glob('session-*'))) <= 6
    assert (logs / 'user-notes' / 'keep.txt').read_text() == 'keep'
    for file in session.glob('*.log*'):
        assert file.stat().st_size < 4*1024*1024 + 1024, (file, file.stat().st_size)
    return session, time.monotonic() - started

run_case()
run_case(17)  # sidecar survives abrupt/unexpected client termination
flood, _ = run_case(TEST_FLOOD='1')
assert any('truncated_or_timeout=1' in p.read_text() for p in flood.glob('*.log*'))
hung, elapsed = run_case(TEST_HANG='1')
assert 'truncated_or_timeout=1' in (hung / 'detail.log').read_text()
assert elapsed < 55  # bounded adb commands, including final collection
print('Diagnostics PASS: normal/abnormal exit, process memory, Unicode-safe argument quoting, '
      'bounded output/rotation/retention, unknown-folder preservation, adb timeout, final evidence')
print('Fixtures:', sandbox)
