# Third-party software

MobilePlayHelper is an independent derivative of scrcpy. It is not affiliated
with or endorsed by Genymobile. The project and its original additions are
licensed under Apache-2.0. Third-party software keeps its respective licenses.
Original attribution and copyright notices must be preserved.

| Component | Version / source | License / notices |
|---|---|---|
| scrcpy client and Android server | 4.1; base commit 19c1261d2e2cbf2b5e6a71a8b64cc1dd3ede06ac | Apache-2.0; LICENSE and NOTICE |
| SDL | 3.4.12 | zlib; licenses/SDL-LICENSE.txt |
| FFmpeg libraries | 8.1.2, official scrcpy Windows build | LGPL-2.1-or-later; licenses/FFmpeg-LICENSE.md and accompanying license texts |
| libusb | 1.0.30 | LGPL-2.1-or-later; licenses/libusb-COPYING.txt |
| dav1d (used by FFmpeg) | 1.5.3 | BSD-2-Clause; licenses/dav1d-COPYING.txt |
| zlib (used by FFmpeg) | 1.3.1 (embedded inflate version) | zlib; licenses/zlib-LICENSE.txt |
| Android platform-tools Windows components | 37.0.0 | Component-specific notices in licenses/Android-platform-tools-NOTICE.txt |

The checked FFmpeg DLLs report LGPL version 2.1 or later. Their embedded
configuration does not enable GPL or nonfree components; dav1d and zlib are
explicitly enabled. Including FFmpeg's alternative license texts does not mean
this build or all of MobilePlayHelper is GPL-licensed.

## Binary releases and corresponding source

Distribute the generated third-party source ZIP, build/provenance information,
license texts and checksums alongside BOTH Windows executable and portable ZIP.
Do not distribute a binary with a different set of DLLs under this manifest.
The packaging script verifies pinned dependencies before packaging.

FFmpeg: https://ffmpeg.org/ and https://ffmpeg.org/legal.html
libusb: https://github.com/libusb/libusb
SDL: https://github.com/libsdl-org/SDL
dav1d: https://code.videolan.org/videolan/dav1d
Android platform-tools: https://developer.android.com/tools/releases/platform-tools
Upstream scrcpy: https://github.com/Genymobile/scrcpy

## Replacing shared libraries

Use the portable ZIP when experimenting with interface-compatible modified
FFmpeg or libusb libraries. Extract it to a writable folder, replace the DLLs
inside `dist/`, and run its root `MobilePlayHelper.exe`. This launcher does not
restore DLL hashes. Keep backups of your original libraries.

For the self-extracting EXE, run `--mph-extract-only`, copy its extracted runtime
to a separate writable folder, then replace the DLLs and run that folder's
`MobilePlayHelper.exe` directly. Do not restart the outer self-extracting EXE:
that entry repairs its managed cache. The normal cache location is documented
in doc/windows-distribution.md.

Users may modify library components and reverse engineer the combined program
for debugging those modifications as permitted by their applicable licenses.
No project term is intended to restrict rights granted by those licenses.

## Branding and assets

The retained scrcpy icon and images originate in the upstream tree. They identify
upstream heritage and do not imply endorsement. No additional trademark rights
are claimed. System fonts are used at runtime and are not redistributed.

Tutorial screenshots in assets/screenshots show the game 逆战：未来 on a connected
Android device. Game names, artwork and interface content remain the property of
their respective rights holders; they are not licensed as original Apache-2.0
project code. The images document interoperability and do not imply endorsement.
