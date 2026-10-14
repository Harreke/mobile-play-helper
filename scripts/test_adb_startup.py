# SPDX-License-Identifier: Apache-2.0
"""Verify client startup with fake ADB; never connect to or reset real devices."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="adb-startup-", dir=root / "work") as tmp:
    home = Path(tmp)
    for source in [root / "dist/MobilePlayHelper.exe", *(root / "dist").glob("*.dll")]:
        shutil.copy2(source, home / source.name)
    source = home / "fake_adb.c"
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    FILE *f=fopen("adb-calls.txt","a");
    if(!f || argc<2) return 9;
    fprintf(f,"%s\n",argv[1]);fclose(f);
    if(!strcmp(argv[1],"start-server")) return getenv("FAIL_START")?1:0;
    if(!strcmp(argv[1],"devices")) {puts("List of devices attached\n");return 0;}
    return 9;
}
''', encoding="utf-8")
    fake = home / "fake-adb.exe"
    subprocess.run([str(root / "work/w64devkit/bin/gcc.exe"), "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(fake)], check=True)
    for fail in (False, True):
        # This test checks ADB ordering, not Windows device enumeration. The real
        # SDL video backend may wait on driver signature checks during shutdown.
        env = dict(os.environ, ADB=str(fake), MPH_BENCHMARK="1",
                   SDL_VIDEODRIVER="dummy")
        env.pop("FAIL_START", None)
        if fail:
            env["FAIL_START"] = "1"
        calls = home / "adb-calls.txt"
        calls.unlink(missing_ok=True)
        result = subprocess.run([str(home / "MobilePlayHelper.exe"),
                                 "--no-video", "--no-audio"], cwd=home, env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW,
                                timeout=20, capture_output=True)
        assert result.returncode != 0  # deliberately no device
        assert calls.read_text(encoding="utf-8").splitlines() == (
            ["start-server"] if fail else ["start-server", "devices"])
        log = (home / "MobilePlayHelper.log").read_text(encoding="utf-8")
        assert "ensuring ADB server is running" in log
        assert ("ADB server ready; discovering devices" in log) != fail
        if fail:
            assert "Could not start adb server" in log
print("ADB startup: PASS (start before discovery; abort on startup failure)")
