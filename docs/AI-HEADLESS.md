# AI 无头模式与 MCP 自动接入

给 AI 软件目录和 APK/APKS 路径后，让它先阅读本 Markdown 文档，按以下步骤接入。
无头模式不创建窗口、不需要显示器，直接加载输入并提供与 GUI 共用的分析工具。

1. 在软件目录定位可执行文件：macOS 发布包为 `Garlic.app/Contents/MacOS/garlic-gui`，
   Windows 为 `garlic-gui.exe`，Linux 为 `garlic-gui`。源码构建的 macOS 路径是
   `build/gui/garlic-gui.app/Contents/MacOS/garlic-gui`。保留同目录的 `garlic` 引擎及 Qt 依赖。
2. 使用绝对路径生成 MCP 配置（路径包含空格时加引号）：

   ```sh
   "/软件目录/Garlic.app/Contents/MacOS/garlic-gui" --headless \
     --apk "/APK目录/app.apks" --threads 8 \
     --deobfuscate --simplify-control-flow --unflatten --print-mcp-config
   ```

3. 将输出的 `mcpServers.garlic` 项合并到 AI 客户端的 MCP 配置，保留原有服务。
   支持 JSON 配置的客户端可直接采用以下格式；其他客户端将 `command` 和 `args`
   映射到其 MCP 配置格式。无需先手动启动软件，客户端连接时会自动启动并打开指定 APK。

   ```json
   {
     "mcpServers": {
       "garlic": {
         "command": "/软件目录/Garlic.app/Contents/MacOS/garlic-gui",
         "args": ["--headless", "--apk", "/APK目录/app.apks", "--threads", "8",
                  "--deobfuscate", "--simplify-control-flow", "--unflatten"]
       }
     }
   }
   ```

4. 完成 MCP `initialize`，通过 `tools/list` 获取工具。调用 `get_status`，等待
   `class_count > 0` 且 `busy = false`，再调用 `get_all_classes`、`get_class_source`、
   `get_methods_of_class`、引用查询和搜索工具。类名使用列表返回的准确大小写。
   大型 APKS 的类列表先就绪，完整元数据稍后完成；依赖成员/引用的工具会等待元数据。
   暂时繁忙时稍后重试，不要反复重启进程。

可直接给 AI 的指令：

> 软件目录是 `/软件目录`，APK 路径是 `/APK目录/app.apk`。请读取软件目录下的
> README，定位可执行文件，用无头模式参数生成 MCP 配置并接入你的 MCP 客户端，
> 自动加载这个 APK，确认索引状态后开始分析。如果当前客户端不能动态加载 MCP，
> 请保存配置并说明需要重新加载客户端，不要声称已经连接成功。

参数说明：`--apk` 也可替换为单个位置参数；`--engine PATH` 指定引擎；
`--threads 1..64`（默认 4）；`--deobfuscate`（名称）、`--deobfuscate-strings`（字符串，独立开关）、`--simplify-control-flow`、`--unflatten`
分别开启反混淆、控制流简化和去平坦化；`--background` 开启全量源码预生成
（默认按需生成，适合快速接入）；`--headless --help` 查看完整参数。
每个无头进程拥有独立工作区，不覆盖桌面偏好或 GUI 的 MCP 发现记录。
标准输出只承载 MCP JSON，分析日志写入标准错误；客户端关闭标准输入后会退出并清理工作区。
GUI 选中文本/当前类工具在无头模式不可用，请明确传入 `class_name`。

需要常驻 HTTP 服务时，用 `--headless --apk PATH --http-port 8650` 启动，
连接 `http://127.0.0.1:8650/mcp`；端口 `0` 自动分配，实际 URL 写入标准错误。
HTTP 模式持续运行，结束分析后停止进程。`--mcp` 单独使用仍是已有 GUI 的桥接模式；
自动启动分析必须包含 `--headless`。客户端配置文件位置及热加载能力由客户端决定。


构建后本文件位于 build/gui/；macOS 应用包内为 Contents/Resources/，Windows/Linux 为可执行文件同目录；发布包根目录也提供 AI-HEADLESS.md。

整数输出格式：`--number-format auto|decimal|hex`，默认 `auto`；两个反混淆开关默认关闭。
