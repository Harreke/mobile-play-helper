# Third-party build provenance

The Windows DLLs, Android server, ADB components and icons are copied unmodified
from the official scrcpy 4.1 Windows x64 archive. Exact hashes and archive URLs
are recorded in config/third-party-lock.json. ADB files were also compared with
the pinned platform-tools 37.0.0 archive. These comparisons establish origin;
they do not claim that a local rebuild is bit-for-bit reproducible.

FFmpeg source 8.1.2 and libusb source 1.0.30 are shipped as original archives
in the corresponding-source ZIP, together with dav1d 1.5.3 and the upstream
build scripts. No local patches were applied to these distributed libraries.
The upstream build scripts and release workflow document toolchain setup.
System build packages in that workflow are not fully version-pinned.

The FFmpeg DLL reports LGPL version 2.1 or later. Its embedded configure line is:

```text
--prefix=/home/runner/work/scrcpy/scrcpy/app/deps/work/install/win64-cross-shared --pkg-config-flags=--static --extra-cflags='-O2 -fPIC' --disable-programs --disable-doc --disable-swscale --disable-avfilter --disable-network --disable-everything --disable-vulkan --disable-vaapi --disable-vdpau --enable-swresample --enable-libdav1d --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=av1 --enable-decoder=vp8 --enable-decoder=vp9 --enable-decoder=libdav1d --enable-decoder=pcm_s16le --enable-decoder=opus --enable-decoder=aac --enable-decoder=flac --enable-decoder=png --enable-protocol=file --enable-demuxer=image2 --enable-parser=png --enable-zlib --enable-muxer=matroska --enable-muxer=mp4 --enable-muxer=opus --enable-muxer=flac --enable-muxer=wav --disable-avdevice --disable-static --enable-shared --enable-cross-compile --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-gcc --target-os=mingw32 --arch=x86_64
```

Zlib's embedded inflate string identifies version 1.3.1. Its license is supplied.
SDL's official development package and upstream build scripts identify 3.4.12.

For an updated binary release, regenerate and review provenance instead of
merely editing hashes. Include the third-party source archive on the same
Release download page as the executables. Retain access to older matching
source archives while their binaries remain available.
