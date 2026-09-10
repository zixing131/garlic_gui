# Garlic GUI 0.3（C++ / Qt 6）

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
`--engine /path/to/garlic` 或“文件 → 选择引擎”更换。语义功能需要使用本项目
0.3 配套引擎的丰富索引和源码位置映射，不能换成未经修改的上游 garlic。

## 浏览和分析

- 打开 / 拖入 APK、DEX、JAR、WAR、CLASS、XAPK、APKS；支持中文和空格路径。
- 包、类、内部类和成员树；类 / 抽象类 / 注解均使用 C（以颜色区分），接口 I、枚举 E、方法 m、
  字段 f 使用不同的颜色 / 图标。成员按展开加载，名称搜索直接查询完整索引。
- 每个类只有一个外层标签，底部“代码 / Smali”切换；JVM 输入禁用 Smali。
- Java / Smali 高亮、行号、当前行、文本查找、字体缩放、多标签和导航历史。
- F12 或 Command/Ctrl + 点击跳转声明；Shift+F12 查找引用；F2 重命名。
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

“保存项目”生成 `.garlic.json`，保存原输入路径、大小、修改时间及别名；“打开
项目”重新索引输入并恢复别名。退出前需保存希望保留的别名。项目文件不包含
APK 和生成的源码，不会修改、重打包或签名原 APK。

导出会在选定位置建立新目录，重新生成当前模式的全部源码并应用别名；类文件名
随别名更新，同时附带项目映射。取消 / 失败保留部分输出。别名是分析辅助功能，
不保证导出的整个工程能重新编译或汇编。

## 设置

“文件 → 首选项”提供实际接入的设置：

| 分类 | 设置 |
| --- | --- |
| 反编译 | 每任务线程数 1–16、排除包前缀、自动后台生成、Unicode 转义、Metadata / 引擎注释显示 |
| 缓存 | 按类源码缓存 MiB、最多打开标签数、单文件查看 / 搜索大小、清理缓存 |
| 界面 | 字号、自动换行、深色 / 浅色界面与代码区同步，包括高亮、行号和当前行 |
| 快捷键 | 编辑已有菜单和导航操作的快捷键 |
| MCP | 启用当前项目服务、复制客户端配置 |

线程数直接传给 garlic 的 `-t`，前台请求和后台生成可能各占一个任务。包排除
和 Unicode 修改后重新打开输入生效。全项目搜索源码不计入按类缓存上限，关闭
项目时清理。默认按类缓存 256 MiB、12 个标签、单文件限制 8 MiB。

jadx 截图中的 AUTO / SIMPLE / FALLBACK、变量反混淆、Kotlin 名称恢复、常量替换、
访问修饰符策略、匿名类 / 方法 / Lambda 内联、finally / switch 恢复策略、dx/d8
转换、数值格式和类型迭代次数，没有可直接复用的 garlic 运行时选项。设置页采用左侧横向文字导航，“引擎能力”说明这些差异；当前继续使用 garlic 本身的处理流程，没有无效开关。

## MCP

工具划分参考 [jadx-mcp-server](https://github.com/zinja-coder/jadx-mcp-server)，由
本项目以 C++ 实现，与当前 GUI 项目共享数据，并非使用 jadx 的 Java 后端。

1. 在 GUI 打开输入文件，进入“首选项 → MCP”，勾选启用并保存。
2. 从同一页复制 stdio 配置，加入 MCP 客户端配置。
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

传输使用当前用户可访问的本地套接字，不监听 TCP 端口。stdio 输出仅有 JSON-RPC，
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
  尚未完成大型 APK 的性能基准。索引保存在 GUI 内存，源码主要在临时磁盘缓存。
- 索引解析、源码符号映射、引用查询、搜索及导出别名处理移到工作线程；类型树
  分批构建，成员按展开加载。超过 512 KiB 的代码暂不做语法高亮，保留查看、
  查找与导航，以避免整篇同步高亮阻塞窗口。这些措施不能保证任意损坏输入都
  无性能问题；CLI 崩溃仍以独立进程处理。
- 方法 / 字段映射来自引擎发射的表达式，Java 类型定位还辅以导入和类型上下文
  解析；复杂转换、动态调用、反射及局部变量没有完整语义保障。不会保证覆盖
  jadx 所有引用与重构能力，也不自动重命名整个方法覆写族。
- 多 DEX / split APK 中同名类仍按类名合并；无法分别选择重复定义。
- 内部类 Java 页面显示所属顶层类源码，Smali 是独立原始类；没有两种代码行号同步。
- 资源树、调试器、反混淆引擎、包 / 局部变量重命名尚未实现。
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
git tag gui-v0.3.0
git push origin main gui-v0.3.0
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
