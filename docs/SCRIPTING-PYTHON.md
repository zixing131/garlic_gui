# Garlic Python 脚本

在 GUI 打开 APK/APKS/DEX/JAR 后，选择 **导航 → 脚本执行… → Python**。
可以编辑、打开和保存 `.py` 文件；填入 JSON 参数后点击运行。日志区显示 stdout、stderr、异常和退出码。
脚本在独立进程中运行，不占用 GUI 线程。停止按钮会断开本次脚本连接并终止解释器；关闭脚本窗口也会停止执行。
默认超时 300 秒，可在设置中调整。脚本草稿、参数自动保存；日志仅保留最近内容，大量结果请写入文件。

## 解释器与依赖

**设置 → 脚本 → Python** 留空时，按 Garlic 进程继承的 PATH 查找 `python3`，再查找 `python`。
填写可执行文件绝对路径或 PATH 中的程序名，可以选择 venv 的 `bin/python` / `Scripts/python.exe` 或 Conda 环境的 Python。
不要填写 `python -m ...`、激活命令或解释器目录。需要 Python 3.8+，内置 SDK 只依赖标准库。
从 Finder/开始菜单打开的软件可能没有继承终端的 Conda/PATH，此时直接指定环境的 Python 文件。
第三方解密库需要安装到所选环境，例如在终端执行 `"/环境/python" -m pip install pycryptodome`。
Garlic 不会自动下载解释器或安装包。

运行日志会显示实际解释器路径。已保存脚本的工作目录是脚本所在目录；未保存时使用 APK 所在目录，无项目时使用用户主目录。
当前工作目录加入 Python 模块搜索路径，可以 `import helper`。用户代码通过临时文件执行，`__file__` 指向临时副本；原始路径在 `api.context['script_path']`。
脚本有当前用户的文件、网络和进程权限，只运行自己信任的代码。

## SDK

```python
from garlic import api, GarlicError

print(api.context)              # 启动时快照：inputs、class_name、selected_text、script_path、working_directory、arguments
key = api.arguments.get('key', 42)
print(api.get_status())         # 实时项目状态
print(api.get_selected_text())  # 实时编辑器选择，返回 {"text": ...}
for tool in api.tools():        # 当前版本完整 API 名称、说明、inputSchema
    print(tool['name'], tool['inputSchema'])
```

所有工具都支持 `api.call('工具名', {参数})` 或 `api.工具名(参数名=值)`。
返回值已经从 MCP content 解包成 Python dict；工具失败抛出 `GarlicError`，连接或解释器错误不会被当成成功结果。
每个 SDK 实例串行调用工具；不要并发触发源码生成或多个搜索。调用使用本次窗口的专用本地连接，**无需启用 MCP 设置或配置端口**，不会连接其他 Garlic 项目。
打开新项目后工具针对新项目执行，但 `api.context` 仍是启动快照，长脚本可用 `get_status()` 校验 inputs。

## 字符串解密示例

JSON 参数填写 `{"key":42}`：

```python
from garlic import api
encoded = [98, 79, 70, 70, 69]
plain = bytes(x ^ api.arguments.get('key', 42) for x in encoded).decode('utf-8')
print(plain)  # Hello
```

解密编辑器选中的十六进制字符串，并输出报告：

```python
from garlic import api
from pathlib import Path
text = api.context['selected_text'].strip().strip('"')
data = bytes.fromhex(text)
plain = bytes(b ^ api.arguments.get('key', 42) for b in data).decode('utf-8', errors='replace')
print(plain)
Path('decrypted.txt').write_text(plain, encoding='utf-8')
```

AES、Base64、压缩、字节运算等使用 Python 标准库或当前环境的第三方包。解密结果不会自动覆盖原始 APK 或伪装为引擎输出。

## 分析与批处理

```python
from garlic import api

# 分页迭代，不在客户端一次保存所有类。
for item in api.classes():
    if item['name'].startswith('com/example/'):
        methods = api.get_methods_of_class(class_name=item['name'])['members']
        print(item['name'], methods)

source = api.get_class_source(class_name='com/example/Decoder')['code']
print(source)
refs = api.get_xrefs_to_method(class_name='com/example/Decoder', method_name='decode',
                              method_signature='(Ljava/lang/String;)Ljava/lang/String;')
print(refs)
# 全量代码搜索可能触发后台生成；仅查符号时设置 search_in='names'。
print(api.search_classes_by_keyword(search_term='decrypt', search_in='names', count=100))
```

`api.classes(count=500)` 是 SDK 分页迭代器。其他分页工具使用 `offset` / `count`，单页最多 1000；通过返回的 total 判断是否还有数据。
繁忙状态会明确返回错误，先 `get_status()` 再稍后重试；不要在 GUI 线程等待脚本。单次长工具调用最长约 300 秒，脚本总超时需留出余量。

## 精确重命名（含局部变量）

```python
from garlic import api
page = api.get_source_symbols(class_name='com/example/Decoder', offset=0, count=1000)
for symbol in page['symbols']:
    # 只选择已核实的声明，不能仅凭同名文本推断符号。
    if symbol['declaration'] and '@local:' in symbol['id'] and symbol['token'] == 'x':
        print(api.rename_symbol(id=symbol['id'], new_name='decodedText'))
        break
print(api.get_aliases())
# api.undo_rename()
# api.save_project(path='/绝对路径/analysis.garlic')
```

符号偏移 start/end 是 UTF-16 单元，不能直接当 Python Unicode 字符串下标（遇到非 BMP 字符会有差异）。token 已提供，无需手动切片。
局部 ID 与本次源码对应；更改反编译选项或清理缓存后应重新读取。重命名只修改可撤销项目别名，必须显式保存项目才能持久化。

## 资源读取

```python
from garlic import api
import base64
page = api.list_resources(count=1000)  # APKS 返回合并条目
for entry in page['entries']:
    if entry['name'].endswith('.xml'):
        result = api.read_resource(path=entry.get('sourcePath', api.context['inputs'][0]),
                                   entry=entry.get('sourceEntry', entry['name']), encoding='xml')
        print(result['data'])
        break
# encoding='base64' 时可用 base64.b64decode(result['data']) 获取原始字节。
```

资源名和 sourcePath/sourceEntry 应直接取自 list_resources，不自行拼接 APKS 内嵌路径。

## 完整工具范围与限制

以运行时 tools() 返回的 inputSchema 为准，下面列出当前能力：

| 工具 | 用途 / 主要参数 |
| --- | --- |
| get_status | 输入、类数量、busy、metadata_ready、sources_ready 等 |
| fetch_current_class / get_selected_text | 当前 GUI 类、代码、选中文本 |
| get_all_classes / get_package_tree | 分页类列表、包与类数量 |
| get_class_source / get_smali_of_class | class_name 对应的 Java / Smali |
| get_methods_of_class / get_fields_of_class | 类的成员元数据，返回 members |
| get_source_symbols | class_name、offset、count；源码符号 ID、token、start/end、declaration |
| get_method_by_name | class_name、method_name、可选 method_signature；方法源码 |
| search_method_by_name | method_name；按名称查方法符号 |
| search_classes_by_keyword | search_term、search_in、regex、case_sensitive、include_resources、offset/count；search_in=code 搜源码，其他值查符号 |
| get_xrefs_to_class / method / field | class_name、method_name/field_name、重载方法的 method_signature、offset/count |
| get_call_graph | 精确方法 id、offset/count；callers/callees 和各自总数 |
| rename_class / method / field | class_name、成员名、可选 method_signature、new_name |
| rename_symbol | 精确 id、new_name；包括局部变量 |
| get_aliases / undo_rename | 查看别名 / 撤销上一次重命名 |
| navigate_to | id、可选 1-based line；GUI 导航并选中符号 |
| list_resources | 可选 path（默认当前输入）、offset/count；合并资源条目和 Manifest 元数据 |
| read_resource | entry、可选 path、encoding、limit；encoding=base64/hex/text/xml，返回 data/encoding/size |
| get_cache_stats / clear_cache | 缓存统计 / 空闲时清理缓存 |
| get_settings / set_settings | 读取设置 / 传入 settings 对象修改已支持字段 |
| save_project | path；保存项目身份和别名 |
| cancel_task | 停止前台、后台反编译与搜索 |

资源默认读取上限 1 MiB，limit 最多 4 MiB，超过上限返回错误而非伪装成完整文件；更大文件可以通过语言的文件/ZIP 库处理。
源码工具响应限制 8 MiB，单页最多 1000 项；源码符号需要先生成对应类。引用不推断反射或动态调用。
GUI 不提供任意源码覆盖或 APK 重打包 API；可将解密结果写成报告文件，或使用精确别名改善显示。
长工具调用约 300 秒超时，繁忙错误需要等待后重试，脚本总超时在设置中配置。

## 随构建分发

本文件与另一语言文档一起复制到 build/gui/。macOS app 内位于 Contents/Resources/，Windows/Linux 位于 GUI 可执行文件旁；发布包根目录同样包含两份 Markdown。
SDK 已嵌入 GUI，运行时自动注入；发布包和 build/gui/scripts/ 也保留 garlic.py / garlic.js 方便阅读。SDK 的 GARLIC_SCRIPT_* 环境变量由软件配置，不应修改或改成其他会话端点。
