# Garlic JavaScript 脚本（Node.js）

在 GUI 打开项目后，选择 **导航 → 脚本执行… → JavaScript (Node.js)**。
支持编辑、打开和保存 `.js` / `.cjs`，JSON 参数、日志、停止与超时控制。SDK 自动注入全局变量 `garlic`。
用户代码在 async 函数中执行，可以直接使用 `await`。这不是浏览器沙箱，没有 DOM；使用 Node.js 标准库和 CommonJS `require()`。

## 执行环境

**设置 → 脚本 → JavaScript / Node.js** 留空时按当前进程 PATH 查找 `node` / `nodejs`，也可指定可执行文件绝对路径。
建议 Node.js 18 或更高版本；不需要安装额外的 Garlic npm 包。第三方 npm 包应安装在脚本目录下。
工作目录是已保存脚本的目录，未保存脚本时是 APK 所在目录，无项目时是用户主目录。
可以 `require('./helper.cjs')`、`require('node:crypto')`、`require('node:fs')`。脚本从标准输入载入，不能再用 stdin 交互读入数据；请使用 JSON 参数。
脚本有当前用户的文件、网络和进程权限，只运行可信代码。解释器和依赖不会自动下载。

默认超时 300 秒，可在设置中调整。停止或关闭窗口会终止该脚本并断开专用连接；GUI 分析操作可另外使用 `cancel_task` 停止。
草稿与参数自动保存；日志有容量限制，批量结果请自行写文件。

## SDK 与异步调用

```javascript
console.log(garlic.context);    // 启动快照：inputs/class_name/selected_text/script_path/working_directory/arguments
console.log(garlic.arguments);
console.log(await garlic.get_status());
console.log(await garlic.get_selected_text());
for (const tool of await garlic.tools()) {
  console.log(tool.name, tool.inputSchema);
}
```

所有工具都支持 `await garlic.call('工具名', {参数})` 或 `await garlic.工具名({参数})`。
返回值为已解包的 JavaScript 对象；工具失败会抛出 Error，并显示在日志中。
**必须 await 每次调用**，不要启动未等待的后台任务；脚本 async 函数完成后 SDK 连接关闭。
SDK 串行请求，避免覆盖全局搜索和抢占引擎。当前窗口使用独立本地连接，无需启用 MCP 或配置端口。
脚本执行中切换项目会改变工具目标；context 是快照，长任务可使用 get_status 检查 inputs。

## 字符串解密

JSON 参数：`{"key":42}`。

```javascript
const key = garlic.arguments.key ?? 42;
const plain = Buffer.from([98, 79, 70, 70, 69].map(x => x ^ key)).toString('utf8');
console.log(plain); // Hello
```

选中文本包含十六进制字符串时：

```javascript
const fs = require('node:fs');
const encoded = garlic.context.selected_text.trim().replace(/^"|"$/g, '');
if (!/^(?:[0-9a-fA-F]{2})+$/.test(encoded)) throw new Error('请选择偶数长度的十六进制字符串');
const decoded = Buffer.from(Buffer.from(encoded, 'hex').map(x => x ^ (garlic.arguments.key ?? 42)));
console.log(decoded.toString('utf8'));
fs.writeFileSync('decrypted.txt', decoded);
```

AES/HMAC 等可使用 `node:crypto`；Base64 使用 `Buffer.from(value, 'base64')`。解密结果不会自动改写 APK。

## 分页、源码、引用与重命名

```javascript
for await (const item of garlic.classes()) {
  if (item.name.startsWith('com/example/')) console.log(item.name);
}
const source = await garlic.get_class_source({class_name: 'com/example/Decoder'});
console.log(source.code);
console.log(await garlic.get_xrefs_to_method({class_name: 'com/example/Decoder', method_name: 'decode',
  method_signature: '(Ljava/lang/String;)Ljava/lang/String;'}));
console.log(await garlic.search_classes_by_keyword({search_term: 'decrypt', search_in: 'names', count: 100}));
```

`garlic.classes(count=500)` 是异步分页迭代器；其他工具使用 offset/count，单页最多 1000。代码搜索会按需触发全量生成。
精确重命名局部变量：

```javascript
const page = await garlic.get_source_symbols({class_name: 'com/example/Decoder', count: 1000});
const variable = page.symbols.find(s => s.declaration && s.id.includes('@local:') && s.token === 'x');
if (variable) await garlic.rename_symbol({id: variable.id, new_name: 'decodedText'});
console.log(await garlic.get_aliases());
// await garlic.undo_rename();
// await garlic.save_project({path: '/绝对路径/analysis.garlic'});
```

符号 offset 使用 UTF-16 单元，与 JavaScript 字符串索引一致；重新反编译后应重新获取 ID。
重命名是可撤销的项目别名，需要 save_project 才能持久化，不修改原 APK。

## 合并资源

```javascript
const page = await garlic.list_resources({count: 1000});
const entry = page.entries.find(e => e.name.endsWith('.xml'));
if (entry) {
  const result = await garlic.read_resource({path: entry.sourcePath ?? garlic.context.inputs[0],
    entry: entry.sourceEntry ?? entry.name, encoding: 'xml'});
  console.log(result.data);
}
```

不要自行猜测拆分包路径，使用条目中的 sourcePath/sourceEntry。

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
