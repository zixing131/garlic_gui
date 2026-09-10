# Garlic GUI（C++ / Qt 6）

原生桌面代码浏览器，使用本项目的 garlic C 引擎。界面和引擎通过
`QProcess` 隔离：建立类索引后，点击顶层类才生成该类的 Java 或 Smali。
索引不会执行方法反编译；JAR 索引也不读取 class 条目的内容。

## 构建和启动

需要 CMake 3.26+、C++17 编译器、Qt 6.5+ Widgets。CLI 构建仍无需 Qt，
`GARLIC_BUILD_GUI` 默认关闭。

macOS / Homebrew：

```sh
brew install qtbase
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGARLIC_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build --parallel
open build/gui/garlic-gui.app
```

也可以带输入文件启动（路径请替换为实际位置）：

```sh
./build/gui/garlic-gui.app/Contents/MacOS/garlic-gui /path/to/app.apk
```

Linux（先安装发行版的 Qt 6 开发包，如 `qt6-base-dev`）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGARLIC_BUILD_GUI=ON
cmake --build build --parallel
./build/gui/garlic-gui /path/to/app.apk
```

Windows 使用与原 garlic 引擎兼容的 MinGW 工具链和同架构 Qt 6 MinGW SDK，
给 CMake 设置 `CMAKE_PREFIX_PATH` 为 Qt 安装目录，再打开构建目录中的
`gui/garlic-gui.exe`。可以用该 Qt SDK 的 `windeployqt` 打包运行库。
Linux / Windows 的构建入口已配置，目前实际运行验证平台为 macOS arm64。

GUI 会使用同目录下的 `garlic` / `garlic.exe`（构建时自动复制）。也支持：

```sh
garlic-gui --engine /path/to/garlic /path/to/app.apk
```

“文件 → 选择引擎”可以更换引擎路径。必须使用包含 `-I` 和 `-c` 扩展的本项目
引擎，旧版 garlic 不支持按类浏览协议。

## 已实现

- 打开或拖入 APK、DEX、JAR、WAR、CLASS、XAPK、APKS。
- 包 / 顶层类树；按包名或完整类名过滤，支持中文路径和空格。
- 按类生成 Java；APK / DEX 系列可以切换 Smali。
- 多标签、Java / Smali 语法高亮、行号、当前行标记、复制、代码缩放。
- 当前文档文本查找，支持上一个 / 下一个和首尾循环。
- 导出整个输入的 Java 或 Smali，在选定位置创建独立的新目录。
- 后台引擎进程、忙碌指示、停止操作、有限长度日志及崩溃 / 启动失败提示。
- 临时磁盘缓存；重复打开同类不再次反编译；关闭或更换输入时清理。
- 最多同时打开 12 个源码标签，打开更多时移除最左侧标签；该源码仍可从磁盘
  缓存重新打开。单份超过 8 MiB 的源码提示导出后用外部编辑器阅读。

macOS 快捷键使用 Command，其他平台使用 Ctrl：打开 O、查找 F、过滤类 L、
关闭标签 W、导出 Shift+E，代码缩放使用系统的放大 / 缩小快捷键。

## 后端协议

```sh
# JSONL，每行 {"name":"demo/Main"}；Java 模式只列出顶层类。
garlic app.apk -I /tmp/classes.jsonl -o /tmp/index-output -t 2

# 只生成指定顶层类；内部类仍由 garlic 随外部类生成。
garlic app.apk -c demo/Main -o /tmp/java-output -t 2

# 原始 DEX 类也可以单独请求 Smali。
garlic app.apk -c 'demo/Main$Details' -s -o /tmp/smali-output -t 2
```

类名使用 `/` 分隔，与索引中的 `name` 一致。找不到指定类时返回非零退出码。
`.class` 文件的 Java 输出仍是 stdout，GUI 将其直接重定向到缓存文件，
不会在内存中积累全部输出。类列表从独立 JSONL 文件读取，不依赖进度日志。

## 当前边界

这是可运行的第一版浏览器，并不具备 jadx 的全部语义交互。

- APK / DEX 索引仍需要 garlic 解析 DEX 元数据；每次未命中的源码请求启动
  新进程并重新解析输入。尚无持久解析会话或跨请求 DEX 缓存，因此对大型 APK
  的解析时间和后端峰值内存并未作性能保证。
- 类树按顶层类组织，内部类在 Java 输出中显示；没有独立的内部类树节点。
- 类索引按类名去重；多 DEX / split APK 中同名类目前不能分别选择。
- 全文搜索目前仅限当前文档；未提供后台全项目源码索引、引用查找、精确声明
  跳转、重命名、Java / Smali 位置同步、资源树和调试器。
- Java 和 Smali 文本由现有 garlic 原样生成，GUI 不修复引擎已有的反编译或
  格式问题，也不保证生成结果可以重新编译 / 汇编。
- 导出会重新批量反编译输入。取消或失败时保留目标目录中的部分输出，日志会
  明确提示；正常退出只表明引擎完成且生成了源码，不代表每个方法都反编译成功。
- 当前缓存只存续于本次打开的文件，不是可恢复的持久项目。

## 验证

集成测试覆盖真实 CLASS / JAR / DEX / APK 的索引、Java / Smali 请求、内部类、
磁盘缓存、导出、Unicode 路径、单线程与双线程选择、未命中类、无效文件、
引擎缺失、取消、崩溃及无效索引。DEX / APK 样例需要 Android SDK 的 d8；不提供
`--d8` 时只生成 JVM 样例。

```sh
python3 gui/tests/make_fixtures.py build/fixtures \
  --d8 /path/to/android-sdk/build-tools/36.0.0/d8
cmake -S . -B build -DGARLIC_BUILD_GUI=ON -DGARLIC_GUI_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

测试样例由仓库内的 Java 源码生成，不依赖第三方 APK。

## macOS 分发

在同一个 Qt SDK 中执行以下命令，将动态框架和平台插件复制进 app：

```sh
/path/to/Qt/bin/macdeployqt build/gui/garlic-gui.app
```

该命令用于本地分发打包，不包含 Developer ID 签名、公证或 DMG 制作。对外
分发时还需根据所用 Qt 许可附带对应许可文本并满足其要求。
