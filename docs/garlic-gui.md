# Garlic GUI 0.5.7（C++ / Qt 6）

原生桌面浏览器，使用本项目的 garlic C 引擎。Qt 界面通过 `QProcess` 调用引擎，
先建立类型、成员和引用索引，点击类时生成 Java / Smali。索引读取 CLASS / DEX
元数据与引用，不执行方法反编译。JAR 索引会读取其中的 CLASS 内容。

## 构建和启动

需要 CMake 3.26+、C++17 编译器、Qt 6.5+ Widgets、Network 和 Concurrent。CLI 构建无需 Qt，
`GARLIC_BUILD_GUI` 默认关闭。

```sh
brew install qtbase
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGARLIC_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build build --parallel
open build/gui/garlic-gui.app
# 或带输入启动：
./build/gui/garlic-gui.app/Contents/MacOS/garlic-gui /path/to/app.apk
```

Linux 安装 Qt 6 开发包（如 `qt6-base-dev`），启用同一个 CMake 选项后运行
`build/gui/garlic-gui`。Windows 使用兼容 garlic 的 MinGW 和同架构 Qt 6 MinGW SDK。
实际构建、运行验证平台为 macOS arm64，其他平台尚未实测。

GUI 默认使用同目录下的 `garlic` / `garlic.exe`，构建时会自动复制。可以用
开发调试参数 `--engine /path/to/garlic` 更换；菜单始终使用内置引擎。语义功能需要使用本项目
0.5 配套引擎的丰富索引和源码位置映射，不能换成未经修改的上游 garlic。

## 浏览和分析

- 同时打开 / 拖入多个 APK、DEX、JAR、WAR、ZIP、CLASS、XAPK、APKS；支持中文和空格路径。
  “输入 → 文件”显示输入列表，“文件 → 添加文件”追加输入并重建项目索引。最近 20 个输入保存在“最近打开”。
- 左侧分为输入、源代码、资源文件、APK signature 和总览。资源目录分批加载，支持 XML / Android 二进制 XML、
  UTF-8 文本、图片、十六进制预览及原始资源导出；resources.arsc 显示资源 ID、名称、配置和值（含复杂映射）。
  图片解码、资源读取和证书解析在后台运行；图片上限 3200 万像素，资源表文本预览上限 4 MiB。
- APK signature 显示文件 SHA-256、v1 签名条目以及 v2 / v3 / v3.1 证书信息；这是解析视图，不执行完整性或证书信任验证。
  总览包含包名、版本、Application、类 / 方法 / 字段统计及 DEX 分布。
- 顶部使用 jadx 原始图标和悬停提示，提供前往 Application、AndroidManifest。类注释显示所属输入及 DEX 来源。
- 类标签右键可关闭当前、其他、左侧、右侧或全部标签。
- 包、类、内部类和成员树使用 jadx 原始图标：类 C、接口 I、枚举 E、注解 @、方法 m、字段 f；
  访问权限、抽象类、构造方法及 static / final 角标按 jadx 源码规则显示，树、标签和搜索结果一致。
  图标内嵌 1–4 倍分辨率，许可和来源见 `gui/icons/jadx/README.md`。
  成员按展开加载，名称搜索直接查询完整索引。
- 每个类只有一个外层标签，底部“代码 / Smali”切换；JVM 输入禁用 Smali。
- Java / Smali 高亮、行号、当前行、文本查找、字体缩放、多标签和导航历史。Ctrl/Cmd + F 默认显示
  当前编辑器查找栏，提供结果数、大小写、全字、正则、上下匹配和 Esc 关闭；查询和选项保存到配置。
- F12 或 Command/Ctrl + 点击跳转声明；X 查找引用；N 重命名；双击符号也可跳转声明。
  成员树和代码右键菜单也提供这些入口。
- 独立、非模态的搜索窗口，提供类名 / 方法名 / 字段名 / 代码 / 注释范围、包前缀、
  大小写和正则选项；输入后 350 ms 自动搜索，也可按回车或点击搜索。
- 名称结果直接查询索引；代码生成和搜索在后台运行，已完成文件的结果逐批出现。
  匹配文本高亮，结果可复制、双击定位，支持保持窗口、停止、加载更多和加载所有。
  首批上限 50 条，可增加到 10000 条；MCP 默认上限 1000 条。停止搜索保留结果，
  后台生成可通过主窗口“停止”中断。资源文本索引尚未提供，对应选项不可选。
- 搜索显示已扫描文件数、未生成及超出大小限制的文件数，后台生成失败时保留
  可搜索的部分源码；更改查询会取消旧查询，避免混入旧结果。
- DEX 引用按类 / 字段 / 方法完整签名索引，保留来源方法和字节码偏移；重载方法
  分开处理。引用列表双击定位来源方法声明，尚无字节码偏移到 Java 调用行的映射。
  JVM 引用来自常量池，提供来源类，不能声称是完整的方法级调用图。
  X 打开独立引用窗口，后台加载源码，逐批显示节点与高亮代码行，可复制全部、停止及保持窗口。
  有源码符号位置时直接跳到调用行，缺少位置时显示来源声明或字节码信息。

## 乱码修复

DEX 和 CLASS 字符串使用 Modified UTF-8，其中 NUL 和 UTF-16 代理项不能直接
当作普通 UTF-8 交给 Qt。Kotlin `@Metadata(d1=...)` 又含有大量控制字符，因此
旧输出容易显示替代字符，旧注解转义还会生成无效的 `\0A`。

引擎现在统一处理注解、字符串常量和 Smali 字符串：正常中文保留，NUL / 控制
字符、引号、反斜杠和代理项正确转义。设置中的“Unicode 字符转义”可进一步把
非 ASCII 字符转义；也可仅隐藏代码视图中的 Kotlin Metadata。已有缓存需要
重新打开输入后生成。

## 重命名和项目

支持类、方法、字段的项目别名，支持撤销最近 100 次操作。标识基于原始类名、
成员名及描述符，不会对整段文本做全局替换；字符串和注释保留。带映射的成员
调用和声明会同步更新，Java / Smali、已打开标签、搜索和导出共享同一份别名。
构造方法保持 JVM / DEX 的 `<init>` 身份，通过重命名类更改 Java 显示名称。

“保存项目”生成 `.garlic.json`，保存全部输入路径、大小、修改时间及别名；“打开
项目”重新索引输入并恢复别名。退出前需保存希望保留的别名。项目文件不包含
APK 和生成的源码，不会修改、重打包或签名原 APK。

导出会在选定位置建立新目录，重新生成当前模式的全部源码并应用别名；类文件名
随别名更新，同时附带项目映射。取消 / 失败保留部分输出。别名是分析辅助功能，
不保证导出的整个工程能重新编译或汇编。

## 设置

“文件 → 首选项”提供实际接入的设置：

| 分类 | 设置 |
| --- | --- |
| 反编译 | 每任务线程数 1–16、排除包前缀、自动后台生成、Unicode 转义、Metadata / 引擎注释显示、名称反混淆、控制流简化、常量调度器反平坦化 |
| 缓存 | 按类源码缓存 MiB、最多打开标签数、单文件查看 / 搜索大小、实时磁盘用量、异步清理缓存 |
| 界面 | 字号、自动换行、深色 / 浅色界面与代码区同步，包括高亮、行号和当前行 |
| 快捷键 | 编辑已有菜单和导航操作的快捷键 |
| MCP | 启用当前项目服务、stdio / HTTP 传输、HTTP 端口、复制客户端配置 |

线程数直接传给 garlic 的 `-t`，前台请求和后台生成可能各占一个任务。包排除
和 Unicode 修改后重新打开输入生效。全项目搜索源码不计入按类缓存上限，关闭
项目时清理。默认按类缓存 2048 MiB、12 个标签、单文件限制 8 MiB。

反控制流平坦化参考 [eShard D810 的常量状态追踪和调度器边重定向](https://www.eshard.com/blog/d810-a-journey-into-control-flow-unflattening)，采用独立实现。
勾选后，在 DEX 模拟和 CFG 构造前，将紧邻常量赋值的 `goto → packed/sparse-switch`
改为已知分支（含默认分支），保留状态赋值。支持 const/4、const/16、const、const/high16；
存在其他显式入边、异常处理或不能证明状态值的路径保持原样。它不处理 native OLLVM、
JVM 调度器、加密状态或多指令状态计算，也不会修改原始 APK。设置持久保存，修改后源码缓存失效。
CLI 可用 `GARLIC_UNFLATTEN=1` 启用，默认关闭。

“反混淆”同时启用静态常量处理（CLI：`GARLIC_DEOBFUSCATE=1`）。参考
[D810 的表达式化简思路](https://www.eshard.com/blog/d810-deobfuscation-ida-pro)，独立实现
32 位整数加减乘除、取余、位运算和移位折叠，以及仅针对局部整数读取的 `x ^ x` / `x - x` 化简。
遵循 Java 溢出与移位规则，除零保持原样；不执行目标方法、不加载目标类，不做浮点或未知调用推测。
静态字符串支持 ASCII 字符数组构造、substring、concat、replace 的嵌套组合。
还原结果以 `/* decoded: "…" */` 显示，保留原表达式和对象身份，避免影响 `==` 或异常行为。
未知自定义加密算法、运行时密钥、非 ASCII 索引语义及解密循环尚不支持；不能保证自动还原任意混淆字符串。
分析有递归深度、节点/字节预算和 64 KiB 字符串上限。

方法的代码区和类树右键菜单提供“复制为 Frida Hook”与“复制为 Xposed Hook”。模板按
原始 JVM/DEX 签名生成，支持重载、构造函数、内部类和数组；不会使用显示别名。
Frida 调用保存的 overload，Xposed 模板放入模块的 `handleLoadPackage` 并使用其 classLoader。
静态初始化器不生成模板。此功能只复制文本，不注入或执行 Hook。
测试用 `fixtures/Flattened.smali` 及其已组装的 `flattened.dex`（smali 3.0.9）覆盖循环、
负数稀疏键、默认分支和未知状态；CI 重新编译输出 Java 并执行返回值断言。

jadx 截图中的 AUTO / SIMPLE / FALLBACK、变量反混淆、Kotlin 名称恢复、常量替换、
访问修饰符策略、匿名类 / 方法 / Lambda 内联、finally / switch 恢复策略、dx/d8
转换、数值格式和类型迭代次数，没有可直接复用的 garlic 运行时选项。设置页采用左侧横向文字导航，“引擎能力”说明这些差异；当前继续使用 garlic 本身的处理流程，没有无效开关。

## MCP

工具划分参考 [jadx-mcp-server](https://github.com/zinja-coder/jadx-mcp-server)，由
本项目以 C++ 实现，与当前 GUI 项目共享数据，并非使用 jadx 的 Java 后端。

1. 在 GUI 打开输入文件，进入“首选项 → MCP”，勾选启用并保存。
2. 选择 stdio 或 HTTP，从同一页复制对应配置，加入 MCP 客户端配置。
3. 保持 GUI 打开。客户端启动的是同一个可执行文件的 `--mcp` 桥接模式。

示例（以设置页生成的实际路径和 socket 为准）：

```json
{
  "mcpServers": {
    "garlic": {
      "command": "/absolute/path/Garlic.app/Contents/MacOS/garlic-gui",
      "args": ["--mcp", "--socket", "garlic-gui-xxxxxxxxxxxxxxxx"]
    }
  }
}
```

stdio 使用当前用户可访问的本地套接字。HTTP 模式在 `127.0.0.1:8650/mcp`（端口可设置）
提供 Streamable HTTP 的 POST / JSON 响应，GET 返回 405。不需要 auth。绑定 IP 可设为 `0.0.0.0` 或 `::`，局域网客户端使用本机局域网 IP。
stdio 使用用户目录的绝对套接字和端点发现文件，客户端临时目录不同或旧 socket 参数不再阻止连接。
服务仍检查 HTTP Origin 和请求大小。两种传输共享当前 GUI 的工具与项目。
stdio 输出仅有 JSON-RPC，
日志写 stderr。支持 initialize、ping、tools/list 和 tools/call；源码 / 搜索 / 引用
请求异步处理，超时 300 秒。大型项目可重试查询已生成的源码。

工具包括：

- 当前界面：`fetch_current_class`、`get_selected_text`。
- 结构：`get_all_classes`、`get_package_tree`、`get_methods_of_class`、`get_fields_of_class`。
- 源码：`get_class_source`、`get_smali_of_class`、`get_method_by_name`。
- 搜索：`search_method_by_name`、`search_classes_by_keyword`（符号或代码）。
- 引用：`get_xrefs_to_class`、`get_xrefs_to_method`、`get_xrefs_to_field`。
- 修改：`rename_class`、`rename_method`、`rename_field`、`undo_rename`、`save_project`。
- 管理：`get_cache_stats`、`clear_cache`、`get_settings`、`set_settings`、`cancel_task`。

完整参数由 tools/list 返回。重载方法需要提供 `signature`，否则返回歧义错误。
类名支持 `demo.Main` 或 `demo/Main`；返回的符号 ID 始终保留原始身份。AI 重命名
会即时更新 GUI，可撤销。资源、调试器、局部变量 / 包重命名工具尚未实现，未对
客户端宣称支持。

## 引擎协议

```sh
# JSONL 类型、成员、引用索引，包含内部类
./build/garlic app.apk -I /tmp/classes.jsonl -o /tmp/index-output -t 2
# 按顶层类生成 Java；内部类随父类生成
GARLIC_SOURCE_MAP_DIR=/tmp/java-output \
  ./build/garlic app.apk -c demo/Main -o /tmp/java-output -t 2
# 按原始 DEX 类生成 Smali
./build/garlic app.apk -c 'demo/Main$Details' -s -o /tmp/smali-output -t 2
```

索引每行包含 `name`、`kind`、`flags`、`inner`、`methods`、`fields`、`refs`。
符号例如 `Ldemo/Main;->greet(I)Ljava/lang/String;`。可选环境变量：
`GARLIC_SOURCE_MAP_DIR` 生成 `.map.json`（UTF-8 字节范围），
`GARLIC_ESCAPE_UNICODE=1` 开启 Unicode 转义，`GARLIC_EXCLUDED_PACKAGES`
为分号分隔的 `/` 形式包前缀。找不到指定类时退出码非零。

## 边界

- 每个未命中的请求仍启动新引擎进程并重新解析输入，没有持久 DEX 解析会话；
  索引保存在 GUI 内存，按类源码可选内存或磁盘缓存。
- 索引解析、源码符号映射、引用查询、搜索及导出别名处理移到工作线程；类型树
  分批构建，成员按展开加载。超过 512 KiB 的代码暂不做语法高亮，保留查看、
  查找与导航，以避免整篇同步高亮阻塞窗口。这些措施不能保证任意损坏输入都
  无性能问题；CLI 崩溃仍以独立进程处理。
- 方法 / 字段映射来自引擎发射的表达式，Java 类型定位还辅以导入和类型上下文
  解析；复杂转换、动态调用、反射及局部变量没有完整语义保障。不会保证覆盖
  jadx 所有引用与重构能力，也不自动重命名整个方法覆写族。
- 多 DEX / split APK 中同名类仍按类名合并；无法分别选择重复定义。
- 内部类 Java 页面显示所属顶层类源码，Smali 是独立原始类；没有两种代码行号同步。
- 调试器、资源文本全局搜索、通用反混淆、包 / 局部变量重命名尚未实现；目前提供名称别名和上述有限 DEX 控制流处理。
- Garlic 原有的反编译错误可能保留。进程成功和源码生成不等于每个方法恢复成功。
  全项目搜索只覆盖成功生成且未超过大小限制的文件。

## 验证与分发

六组集成测试覆盖真实 CLASS / JAR / DEX / APK，包含编码、类型图标元数据、内部类、重载
方法映射与重命名、字段引用、Java / Smali 导出别名、项目保存恢复、正则搜索、
底部标签、导航、取消 / 崩溃 / 无效索引，以及 MCP 协议和真实 stdio 桥接。

```sh
python3 gui/tests/make_fixtures.py build/fixtures \
  --d8 /path/to/android-sdk/build-tools/36.0.0/d8
cmake -S . -B build -DGARLIC_BUILD_GUI=ON -DGARLIC_GUI_TESTS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

不提供 `--d8` 时仅生成 JVM 样例。样例来自仓库内 Java 源码。
macOS 用同一 Qt SDK 的 `macdeployqt` 打包 app，包含 Qt 运行库和平台插件；这是
本地分发包，不包含 Developer ID 签名 / 公证。对外分发需满足所使用的 Qt 许可。


## GitHub Actions 发布

工作流位于 `.github/workflows/gui-release.yml`，包含六个原生构建：

| 平台 | 架构 | 构建环境 |
| --- | --- | --- |
| macOS | ARM64、x64 | macos-15 / macos-15-intel，Qt 6.8.3 |
| Windows | ARM64、x64 | windows-11-arm / windows-2022，MSYS2 CLANGARM64 / CLANG64 |
| Linux | ARM64、x64 | ubuntu-24.04-arm / ubuntu-24.04，Qt 6.8.3 |

普通提交、PR 和手动运行只生成 Actions artifacts；推送 `gui-v*` 标签后，六个
构建及测试均成功才发布同名 GitHub Release，并附上六个 ZIP 和 `SHA256SUMS.txt`。
工作流仅在发布任务授予 `contents: write`，不需要另存个人访问令牌。

```sh
# 在已验证的提交上发布，推送时会运行六个平台的构建：
git tag gui-v0.5.0
git push origin main gui-v0.5.0
```

打包由 `scripts/package_gui.py` 完成：包含匹配架构的 GUI、garlic 引擎、Qt
框架 / 插件及依赖，打包前验证架构并运行启动与真实 JAR 索引检查。macOS 用
`ditto` 保存框架符号链接，进行临时签名；Windows 递归收集 MSYS2 DLL，并在不含
SDK 的 PATH 下启动验证；Linux 解压后运行 `Garlic` 启动脚本，要求 Ubuntu 24.04
或兼容 glibc 版本，系统仍需提供图形会话与显卡驱动。

Windows ARM64 没有可嵌入的 rosemary ELF 分析库时禁用该可选功能；Java / DEX
反编译和 GUI 不依赖该库。Windows / macOS 发布包未包含商业代码签名或公证。
本地已验证 macOS ARM64；其他平台实际构建结果以 Actions 为准。

工作流环境参考 [GitHub 托管 runner 文档](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)、
[Qt 安装 Action](https://github.com/jurplel/install-qt-action) 和
[MSYS2 ARM64 文档](https://www.msys2.org/docs/arm64/)。

可以额外运行本机大项目响应测试（输入文件不会提交到仓库）：

```sh
GARLIC_TEST_REAL_APK=/path/to/app.apk ctest --test-dir build -R interaction --output-on-failure
```

0.4 增加多输入合并、资源预览、引用窗口、标签关闭菜单、HTTP MCP 鉴权与通知测试。
本地可设置 `GARLIC_TEST_REAL_APK=/path/to/app.apk` 运行真实 Manifest / ARSC / 签名解析、
合成 Lambda Java 反编译及事件循环响应测试。发布矩阵包含 macOS、Windows、Linux 各自的 arm64 / x64。
Windows 显式链接 winpthread；Windows ARM64 使用 Temurin 21；Qt 6.8 Linux x64 使用 linux_gcc_64；
Linux 打包仅对 ELF 文件运行 ldd。`gui-v*` 标签在六个平台都成功后发布 ZIP 和 SHA256SUMS。

独立 CLASS / DEX / PE 以 `rb` 读取并检查短读，避免 Windows 文本模式的 CRLF / Ctrl-Z 转换破坏输入。

## 0.5 更新

- 引用采用后台预热的倒排索引，先展示引用位置和已有源码，按需加载选中来源，避免逐类启动反编译进程。
  本机 4932 类 APK 的首批结果约 281 ms，重复索引查询低于 1 ms；实际耗时取决于项目和机器。
- X / N 默认快捷键使用新版配置键，迁移时丢弃旧版本默认值残留；菜单、首页提示与设置同步。
  只读编辑器关闭输入法合成输入，成员树和代码区均支持快捷键。
- 默认应用图标由 GPT 生成，PNG、ICO、ICNS 随三平台打包；提示词见 [图标说明](../gui/icons/app/README.md)。
  资源采用 jadx 的 XML、Manifest、DEX、图片、音频、视频、字体、压缩包等文件类型图标。
- 工具栏更紧凑，代码搜索框增加左边距；Java 类型、调用、常量、基本类型和 Smali 寄存器、XML 标签属性分别高亮。
- 缓存页提供内存 / 磁盘模式，显示当前源码内存与磁盘用量。内存模式仍会使用引擎临时文件，
  全项目源码搜索工作文件仍在磁盘，关闭项目清理，不计入按类缓存上限。
- 可选状态栏显示 GUI 与活动引擎进程的当前内存、系统可用内存和峰值（子进程峰值为定时采样）。
- 树过滤保留展开状态，选中节点消失时回退到前一个可见位置。搜索范围、正则、大小写、
  自动搜索、包限制、保持窗口和引用保持窗口等选项保存到配置。
- Manifest 未声明 Application 时遍历继承关系：一个候选直接打开，多个候选供选择；没有候选时说明使用系统默认 Application。
- 总览增加输入、代码来源、原生库 ABI、指令单位、缓存 / 生成进度和已观察到的错误警告。
  签名页增加 v1 证书、版本、序列号、有效期、RSA 模数和指数、算法 OID、三种指纹以及未被 v1 清单覆盖的文件。
  签名页仍是解析结果，未进行完整的 APK 密码学签名验证，不宣称“验证成功”。

## 0.5.1 大索引与 Windows 路径修复

- 取消整份类索引的 256 MiB 限制，后台按 JSONL 记录读取及合并，支持取消。文件读取错误与 JSON 格式错误分别报告，格式错误附行号。
- 本机 `base.apk` 的 660.5 MiB 索引、52,791 个类验证成功，索引生成与加载合计约 10.4 秒；内存使用仍随类和引用数量增长。
- Windows 引擎内嵌 UTF-8 活动代码页 manifest，统一 CRT 参数、文件路径、环境变量与 miniz 路径编码；不要求修改系统语言。适用于 Windows 10 1903+ / Windows 11。
  实现依据 [Microsoft UTF-8 活动代码页文档](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)。
- 创建目录不再截断为 256 字节，保留中文路径完整 UTF-8 字节。所有平台的 CLI 测试覆盖中文输入、索引、输出、映射目录和超过 256 字节的完整路径；同时验证 257 MiB 索引与损坏索引拒绝。


## 0.5.2 JAR、资源目录和响应性修复

- 修复 JVM 未识别属性没有推进内存读取位置，以及栈合流后的 `dup` 被错误当作赋值表达式导致的崩溃。
  添加合法扩展属性和 JVM 栈循环回归样本；真实 JAR 的 46 个类逐类与 8 线程完整导出均通过。
- 清理源码缓存会释放项目源码和已打开标签持有的内容，重新选择标签时按需加载。
  磁盘删除在后台进行，失败会列出路径。设置分别显示源码磁盘用量、内存源码用量和保留的类索引；
  类索引服务于目录、跳转和引用，不属于源码缓存。正在运行分析时需先停止任务。
- `resources.arsc` 可按需展开为包、`res/values[-配置]` 目录和分类 XML 文件，支持查看与导出。
  包含字符串、数组、样式、复数、ID 等条目；未识别的配置以 `config-十六进制` 保留。
  这是资源表的可读视图，不保证导出 XML 能直接用于重打包，未解析的属性引用仍保留数值 ID。
- 资源表解析、总览统计移到后台；成员与资源节点按时间片插入，目录过滤增加输入防抖。
- 移除已被独立搜索、引用窗口替代的空白停靠面板。项目搜索和 X 查找引用继续可用。
- 主窗口两侧随窗口宽度重新分配空间；搜索/引用结果列跟随宽度，窄搜索窗口的范围选项自动换行布局。


## 0.5.3 导航、包目录和搜索缓存

- 工具栏新增主 Activity（位于 Application 左侧）、与编辑器同步、展开显示代码包。
  主 Activity 按同一 intent-filter 的 MAIN + LAUNCHER / LEANBACK_LAUNCHER 查找，支持 activity-alias 目标类；多个入口可选。
- 与编辑器同步会清除阻挡当前文件的过滤条件，展开并选中源码或资源节点。
- “展开显示代码包”默认勾选，保持平铺完整包名；取消勾选按包层级显示目录，状态保存到配置。
- 鼠标后退/前进侧键复用现有导航历史；首选项列表去除焦点边框并保留键盘操作。
- 搜索使用 64 MiB LRU 源码预处理缓存和三字符 Bloom 预筛选索引。
  命中文件仍完整匹配；短查询与正则不会因预筛选丢失结果。
  文件大小/修改时间、代码/注释范围和重命名映射参与缓存键；切换项目或清理源码缓存释放索引。
  首次全文搜索仍需后台生成源码，后续搜索复用已生成文件和缓存。


## 0.5.4 资源目录与大型输入加载

- `resources.arsc` 直接展开为 `res/values-*`，不再插入包名目录。
  解析语言、地区、脚本、布局方向、屏幕尺寸、夜间模式、密度等 Android 配置限定符，
  替代此前的 `config-十六进制` 目录。多资源包使用 XML 文件名后缀区分，避免同名内容冲突。
- GUI 类索引按引用来源分组编码，保留全部引用和字节码偏移；普通 CLI 索引格式保持兼容。
  引擎生成索引与 GUI 后台读取同时进行，完整校验后发布项目，目录分批显示。
- 修复 APKS 内 `splits/base-master.apk` 等子目录中的 APK 被跳过的问题。
- 本机 macOS ARM64 实测（文件系统缓存已预热，52,791 个类）：
  `base.apk` 后端索引从约 10.2 秒降到约 4.0 秒，完整目录约 4.6 秒；
  `K PLUS_5.22.2.apks` 完整目录约 4.7 秒。
  指标是打开至完整类目录可用，不包括全项目源码反编译；其他硬件、冷磁盘与更大输入耗时会不同。

## 0.5.5 编辑器语义导航与 ZIP 输入

- 包显示开关改为明确的选中状态，并保留上次选择；Fusion 焦点轮廓统一关闭，避免树、标签和工具按钮出现虚线框。
- 选中标识符会同步高亮同一编辑器中的全部出现位置。字段成员由索引定位，双击 `this.field` / `super.field` 可跳转定义；
  局部变量则回到当前源码中最近的类型声明。字段、类、方法和类型都可用 X 打开引用窗口。
- 从索引的 extends / implements 关系恢复 Java 展示层的 `@Override` 标记，并在注释中标出被实现的父类或接口方法。
- ZIP 现在是可打开输入：根目录或子目录的 DEX、嵌套 APK 和 CLASS 会被索引并按需反编译；压缩包内资源、输入列表和总览仍可浏览。
  ZIP 不含可支持字节码时显示“无类被加载，没有什么可以反编译”。

## 0.5.6 全项目搜索加速

- 全项目搜索的待生成源码队列改为线性轮询，避免大型项目中反复从顺序容器删除元素造成平方级开销。
- 每个已处理源码保留紧凑的 8192 位三字符过滤器；即使完整文档被 64 MiB LRU 缓存淘汰，后续查询也能在不访问磁盘的情况下排除大多数文件。
- Java 展示层补充的 `@Override // 父类或接口.方法` 建立一次性内存索引，`@Override` 可按“代码”搜索，后面的来源说明可按“注释”搜索，并直接跳转到对应方法。
- 本机 macOS ARM64 对 52,791 个类的 `base.apk` 实测：首次缺失词查询包含全部源码生成约 15.5 秒；源码与搜索索引就绪后，第二次全项目缺失词查询约 0.2 秒。

## 0.5.7 调用图、Native 分析与搜索预热

- 导航菜单和工具栏新增“查看函数调用图”（`G`）。以当前方法为中心，在后台展开调用者和被调用者，支持 1–5 层深度、方向筛选、拖动画布，双击节点跳转到声明；为保证大项目响应，单图最多显示 150 个函数。
- 可直接打开 `.so` / `.dylib`，也可在 APK 资源树中点击 Native 库。GUI 在临时目录异步提取并调用 garlic/rosemary，提供导入导出符号、函数入口、函数引用、字符串、PC 引用、反汇编和 CFG 结果；大型结果按标签异步加载，完整文件可以另行导出。
- garlic 的 `-n` 模式现在会实际写出 rosemary 的全部分析文件，供 GUI 和命令行共同使用。没有内置 rosemary 库的平台会显示明确的分析失败信息。
- 项目后台反编译时同步预热全文搜索过滤器，并缓存扁平符号列表。52,791 类的 `base.apk` 实测，索引就绪后的缺失词、常见代码词和符号查询分别约 0.21、0.16 和 0.79 秒；基准测试会直接检查三个查询都不超过 1 秒。
- 紧凑引用索引改为显式、带边界检查的 JSON 读取，窗口异步目录测试等待目标节点发布，修复 Qt 6.8/macOS x64 Actions 中的两个偶发失败。

### Native ELF 与 Ghidra 伪代码

可以直接打开 ELF（包括无扩展名文件），也可以从资源树分析 SO。ELF 头、架构、入口和节表在后台独立读取，即使 Native 后端不可用也能查看。

Native 窗口中选择“Ghidra 伪代码”，点击“配置 Ghidra…”选择安装目录，然后重新分析。需要自行安装 Ghidra、该版本要求的 JDK（通过 JAVA_HOME 或 PATH），以及对应平台的 Ghidra native decompiler。应用不会捆绑 Ghidra，也不会执行目标文件。参考 [Ghidra Headless API](https://ghidra.re/ghidra_docs/api/ghidra/app/util/headless/AnalyzeHeadless.html)。

分析在独立进程进行，可停止；每个函数最多 10 秒，脚本最多 120 秒、2000 个函数、约 16 MiB 输出，整个进程最多 240 秒。达到限制会在结果中说明。界面按需读取前 8 MiB，可导出完整生成结果。伪代码属于静态分析结果，不保证等同于原始源码。

Windows x64 发布包显式收集 Rosemary DLL 的 GCC 运行库；临时 DLL 创建和加载使用 Unicode Windows API。Windows ARM64 目前没有仓库配套的 Rosemary 库，可使用独立 ELF 概览或配置兼容运行环境的 Ghidra；这不代表已经提供 ARM64 原生 Rosemary。Windows 修复尚需实际 Windows 机器验证。

### 算法助手样本开关对比（2026-09-10）

用户提供 APK 的 SHA-256：`febdee12bc13c0f510553e9c3cb49a286fc58415098fe0834b6e583dd49336d3`。
对 `com/junge/algorithmAidePro/` 下 106 个类分别启用和关闭 `GARLIC_DEOBFUSCATE`、`GARLIC_UNFLATTEN`、`GARLIC_SIMPLIFY_CONTROL_FLOW`，每次按类反编译，所有进程正常退出。

- 13 个类输出发生变化，观察到计算常量简化。
- 两组输出均有 432 处 `switch`，本样本的主要平坦化未消除。
- 两组均没有 `decoded:` 标记，现有字符串恢复未命中。

样本包含自定义字符串状态计算后进入 switch 的分发器，以及依赖静态字段的分支。现有常量状态分发器还原不能覆盖这种模式。输出差异只证明转换发生，不是语义等价验证；未执行 APK 或其中的解密方法。不要把启用选项理解为所有混淆均可自动还原。
