# Windows 发布与使用

双击根目录 `Build-Windows-Standalone.cmd` 或运行 `python scripts/package_windows.py`。
构建机需要 Python 3.12+、官方 scrcpy 4.1 Windows x64 运行文件和网络；首次准备工具需要下载。先运行 `python scripts/build_windows.py --runtime-dir D:\scrcpy` 可准备开发运行目录。

脚本校验固定版本依赖、重新编译项目、准备许可证和对应源码，并测试单文件包与 ZIP 便携版。成功产物位于 `packages/`：

- `MobilePlayHelper-Windows-x64.exe`：自解压单文件程序。
- `MobilePlayHelper-Windows-x64-portable.zip`：普通便携版，可替换共享库。
- `MobilePlayHelper-third-party-sources.zip`：FFmpeg、libusb、dav1d 的源代码归档、构建脚本、许可及来源信息。
- `SHA256.txt`：以上三个文件的校验值。
- `package-manifest.json`：单文件包载荷清单。

发布 Release 时将这组文件放在同一下载页，并附上对应版本的项目源码。源码 ZIP 供接收者取得和修改第三方组件，不能因为运行时不需要它就从发布页省略。未经复核的旧 `packages` 文件不代表通过当前发布流程。

## 使用

接收者使用 Windows 10/11 x64，不需要 Python、Java 或 Gradle。手机需要开启 USB 调试并授权电脑。电脑需有可用的 USB 驱动。程序没有商业代码签名。

ZIP 版请先完整解压，再运行根目录 `MobilePlayHelper.exe`，不要只复制入口文件。个人配置位于 `profiles/`，运行日志和依赖位于 `dist/`。升级时保留个人配置。

单文件版在 `%LOCALAPPDATA%\MobilePlayHelper\Standalone/` 下释放 `runtime/<版本摘要>/` 和独立的 `profiles/`。也可用 `MPH_PORTABLE_HOME` 指定其他绝对路径。每次启动校验并恢复该版本的载荷文件；已有个人配置不覆盖。画质和日志按运行版本分别保存。重复启动受互斥锁保护。

发布模板来自 `assets/default-fps.mph`，不打包开发者个人配置、运行日志、录制、ADB 授权密钥或构建工具。自动画质压力测试入口仍保持停用。

## 替换 LGPL 共享库

推荐使用 ZIP 版。关闭程序后，备份并替换 `dist/` 内接口兼容的 FFmpeg 或 libusb DLL，然后运行 ZIP 解压目录的根入口。该入口不校验或还原 DLL。

单文件版也支持 `MobilePlayHelper-Windows-x64.exe --mph-extract-only`。将释放的 `runtime/<版本摘要>/` 整个复制到另一个可写目录，在副本中替换 DLL，再运行副本内根目录的 `MobilePlayHelper.exe`。不要用外层自解压 EXE 启动这个副本。需要保留既有键位时，可将其复制到副本 `profiles/` 或用 `MPH_PROFILE` 指定路径。

详情见 [第三方许可](../THIRD_PARTY_NOTICES.md)、[构建来源](third-party-build.md) 和 [隐私说明](privacy.md)。

## 验证边界

打包测试不连接手机。单文件测试验证中文路径、哈希、缓存修复和配置保留；ZIP 测试验证许可布局、正常启动及修改后的 DLL 不被还原。测试使用可加载 DLL 的附加数据来检验替换保留行为，不代表任意第三方修改都具备 ABI 兼容性。

仅重新封装已构建版本可使用 `python scripts/package_windows.py --skip-build`。如果当前运行文件仍旧于已构建替换文件，脚本停止，避免发送过期程序。
