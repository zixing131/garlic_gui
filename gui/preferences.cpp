#include "mainwindow.h"
#include "mcpserver.h"
#include "theme.h"
#include "scriptdialog.h"
#include "localization.h"
#include "hexviewer.h"
#include <QtConcurrent>
#include <QtWidgets>

namespace {
class PreferenceItemDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem clean(option);
        clean.state &= ~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(p, clean, index);
    }
};
} // namespace
void MainWindow::applySettings(const AppSettings &settings, bool preserveAnalysis) {
    backend_.configure(settings, !preserveAnalysis);
    qApp->setFont(settings.interfaceFont());
    for (auto widget : QApplication::allWidgets()) {
        if (auto code = qobject_cast<CodeEditor *>(widget))
            code->setFont(settings.codeFont(code->property("monoFont").toBool()));
        else if (dynamic_cast<HexViewer *>(widget)) widget->setFont(settings.codeFont(true));
        else if (widget->objectName() == "scriptCode" || widget->objectName() == "scriptOutput")
            widget->setFont(settings.codeFont());
    }
    memoryLabel_->setVisible(settings.showMemory);
    for (int i = 0; i < tabs_->count(); i++)
        if (auto page = qobject_cast<ClassView *>(tabs_->widget(i)))
            page->applySettings(settings);
        else if (auto code = tabs_->widget(i)->findChild<CodeEditor *>())
            code->setTheme(settings.theme == "light");
    if (!qApp->property("garlicDarkStyle").isValid())
        qApp->setProperty("garlicDarkStyle", garlicStyleSheet());
    const QString dark = qApp->property("garlicDarkStyle").toString();
    if (settings.theme == "light") {
        QPalette lightPalette;
        lightPalette.setColor(QPalette::Window, QColor("#f5f7fa"));
        lightPalette.setColor(QPalette::WindowText, QColor("#243446"));
        lightPalette.setColor(QPalette::Base, Qt::white);
        lightPalette.setColor(QPalette::AlternateBase, QColor("#f0f4f8"));
        lightPalette.setColor(QPalette::Text, QColor("#243446"));
        lightPalette.setColor(QPalette::Button, QColor("#e8edf3"));
        lightPalette.setColor(QPalette::ButtonText, QColor("#243446"));
        lightPalette.setColor(QPalette::PlaceholderText, QColor("#697e8f"));
        lightPalette.setColor(QPalette::Highlight, QColor("#c5dff5"));
        lightPalette.setColor(QPalette::HighlightedText, QColor("#173a59"));
        lightPalette.setColor(QPalette::ToolTipBase, Qt::white);
        lightPalette.setColor(QPalette::ToolTipText, QColor("#243446"));
        lightPalette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8a98a3"));
        lightPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8a98a3"));
        qApp->setPalette(lightPalette);
        QString light = dark;
        light.replace("#243446", "#e8edf3");
        const QList<QPair<QString, QString>> colors = {
            {"#17212d", "#f5f7fa"}, {"#1b2937", "#e8edf3"}, {"#14202b", "#eef2f6"},
            {"#111b26", "#ffffff"}, {"#dce5ee", "#243446"}, {"#e0ebf3", "#243446"},
            {"#8c9dad", "#617183"}, {"#95a8b9", "#53677a"}, {"#b2c3d3", "#34475a"},
            {"#a4e4bd", "#267650"}, {"#304354", "#dae6f0"}, {"#28534d", "#c5dff5"},
            {"#38635b", "#b9d8ef"}, {"#354454", "#b8c7d5"}, {"#2a3949", "#d0d9e2"}};
        for (const auto &c : colors)
            light.replace(c.first, c.second);
        qApp->setStyleSheet(light);
    } else {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#17212d"));
        palette.setColor(QPalette::WindowText, QColor("#dce5ee"));
        palette.setColor(QPalette::Base, QColor("#111b26"));
        palette.setColor(QPalette::Text, QColor("#dce5ee"));
        palette.setColor(QPalette::Button, QColor("#243446"));
        palette.setColor(QPalette::ButtonText, QColor("#dce5ee"));
        palette.setColor(QPalette::PlaceholderText, QColor("#8195a7"));
        palette.setColor(QPalette::Highlight, QColor("#285f59"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#637386"));
        qApp->setPalette(palette);
        qApp->setStyleSheet(dark);
    }
    QString error;
    if (settings.mcpEnabled) {
        if (!mcp_->start(&error)) {
            logs_->appendPlainText(error);
            status_->setText(error);
        }
    } else
        mcp_->stop();
    for (auto action : findChildren<QAction *>())
        if (!action->text().isEmpty()) {
            const auto shortcut = QSettings().value("shortcuts-v3/" + action->objectName(),
                QSettings().value("shortcuts-v2/" + action->property("shortcutSource").toString()));
            if (shortcut.isValid())
                action->setShortcut(QKeySequence(shortcut.toString()));
        }
    auto hint = findChild<QLabel *>("shortcutHint");
    if (hint) {
        QStringList keys;
        for (auto action : findChildren<QAction *>())
            if (action->text() == tr("查找引用") || action->text() == tr("重命名") ||
                action->text() == tr("跳转到声明"))
                keys << action->shortcut().toString(QKeySequence::NativeText) + " " +
                            action->text();
        hint->setText(tr("Java / Smali 底部切换 · ") + keys.join(" · "));
    }
    updateBusy();
}
void MainWindow::settingsDialog() {
    AppSettings settings = backend_.settings();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Garlic 首选项"));
    dialog.resize(850, 680);
    auto root = new QVBoxLayout(&dialog);
    auto navigation = new QListWidget;
    navigation->setObjectName("preferencesNavigation");
    navigation->setFixedWidth(160);
    navigation->setItemDelegate(new PreferenceItemDelegate(navigation));
    navigation->setStyleSheet("QListWidget::item { padding: 10px 14px; } QListWidget::item:focus { "
                              "outline: none; border: none; }");
    auto tabs = new QStackedWidget;
    auto body = new QHBoxLayout;
    body->addWidget(navigation);
    body->addWidget(tabs, 1);
    root->addLayout(body, 1);
    connect(navigation, &QListWidget::currentRowChanged, tabs, &QStackedWidget::setCurrentIndex);
    auto page = [&](const QString &name) {
        auto widget = new QWidget;
        auto form = new QFormLayout(widget);
        form->setVerticalSpacing(16);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(widget);
        tabs->addWidget(scroll);
        navigation->addItem(name);
        if (navigation->currentRow() < 0)
            navigation->setCurrentRow(0);
        return form;
    };
    auto spin = [](QFormLayout *form, const QString &label, int value, int min, int max) {
        auto box = new QSpinBox;
        box->setRange(min, max);
        box->setValue(value);
        form->addRow(label, box);
        return box;
    };
    auto check = [](QFormLayout *form, const QString &label, bool value) {
        auto box = new QCheckBox(label);
        box->setChecked(value);
        form->addRow(box);
        return box;
    };
    auto decompile = page(tr("反编译"));
    auto threads = spin(decompile, tr("每个反编译任务的线程数"), settings.threads, 1, 16);
    auto excluded = new QPlainTextEdit;
    excluded->setPlaceholderText(tr("每行一个包前缀，例如 com.example.library"));
    excluded->setPlainText(settings.excluded.join('\n'));
    excluded->setMaximumHeight(100);
    decompile->addRow(tr("排除的包"), excluded);
    auto background = check(decompile, tr("打开文件后自动后台生成项目源码"), settings.background);
    auto deobfuscate = check(decompile, tr("函数和变量名称反混淆（含类名）"), settings.deobfuscate);
    deobfuscate->setObjectName("deobfuscate");
    deobfuscate->setToolTip(tr("为混淆的类、函数、字段和局部变量生成可读名称。"));
    auto strings = check(decompile, tr("字符串反混淆"), settings.deobfuscateStrings);
    strings->setObjectName("deobfuscateStrings");
    strings->setToolTip(tr("静态还原可确定的字符串表达式，并显示 decoded 注释；不会执行目标解密函数。"));
    auto numberFormat = new QComboBox;
    numberFormat->setObjectName("numberFormat");
    numberFormat->addItem("auto", "auto");
    numberFormat->addItem(tr("十进制"), "decimal");
    numberFormat->addItem(tr("十六进制"), "hex");
    numberFormat->setCurrentIndex(qMax(0, numberFormat->findData(settings.numberFormat)));
    decompile->addRow(tr("数值格式化"), numberFormat);
    auto controlFlow = check(decompile, tr("控制流整理（DEX 跳转链和共享代码块）"), settings.simplifyControlFlow);
    controlFlow->setObjectName("simplifyControlFlow");
    auto unflatten = check(decompile, tr("反控制流平坦化（DEX 常量状态 switch 调度器）"), settings.unflatten);
    unflatten->setObjectName("unflatten");
    unflatten->setToolTip(tr("恢复 const → goto → switch 的确定跳转；复杂状态计算和带异常处理的方法保持原样。"));
    decompile->addRow(new QLabel(tr("反混淆选项在重新打开文件后生效；生成别名不会恢复原始名称。")));
    auto unicode =
        check(decompile, tr("Unicode 字符转义（保留正常中文可不勾选）"), settings.escapeUnicode);
    auto metadata = check(decompile, tr("显示 Kotlin Metadata 注解"), settings.showMetadata);
    auto notice = check(decompile, tr("显示反编译器头部与类说明注释"), settings.showNotice);
    auto note = new QLabel(
        tr("控制字符始终合法转义。线程数会传给 garlic。\n排除包和 Unicode "
           "设置修改后，重新打开输入文件生效。\n前台按类请求和后台索引是独立任务，可能同时运行。"));
    note->setWordWrap(true);
    decompile->addRow(note);
    auto cache = page(tr("缓存"));
    auto cacheMode = new QComboBox;
    cacheMode->setObjectName("cacheMode");
    cacheMode->addItem(tr("磁盘缓存"), "disk");
    cacheMode->addItem(tr("内存缓存"), "memory");
    cacheMode->setCurrentIndex(settings.cacheMode == "memory" ? 1 : 0);
    cache->addRow(tr("源码 / 索引缓存模式"), cacheMode);
    auto cacheLimit = spin(cache, tr("按类源码缓存上限（MiB）"), settings.cacheMiB, 16, 4096);
    auto indexLimit = spin(cache, tr("索引 / 源码总容量（GiB）"), settings.indexCacheGiB, 1, 1024);
    indexLimit->setObjectName("indexCacheGiB");
    auto indexDirectory = new QLineEdit(settings.indexDirectory);
    indexDirectory->setObjectName("indexDirectory");
    indexDirectory->setPlaceholderText(IndexCache::directory(AppSettings()));
    auto indexRow = new QHBoxLayout; indexRow->addWidget(indexDirectory);
    auto chooseIndexDirectory = new QPushButton(tr("选择…")); indexRow->addWidget(chooseIndexDirectory);
    cache->addRow(tr("索引缓存目录"), indexRow);
    connect(chooseIndexDirectory, &QPushButton::clicked, &dialog, [this, indexDirectory] {
        auto selected = QFileDialog::getExistingDirectory(this, tr("选择索引缓存目录"), indexDirectory->text());
        if (!selected.isEmpty()) indexDirectory->setText(selected);
    });
    auto indexHint = new QLabel(tr("磁盘模式保存完整索引、JSONL 和已生成源码；空间不足时删除创建时间最早的缓存。\n更改目录不搬迁旧缓存，可选回旧目录清理。"));
    indexHint->setWordWrap(true); cache->addRow(indexHint);
    auto clearIndexes = new QPushButton(tr("清除目录中的索引和源码缓存"));
    clearIndexes->setObjectName("clearIndexes"); cache->addRow(clearIndexes);
    connect(clearIndexes, &QPushButton::clicked, &dialog, [this, indexDirectory] {
        auto target = backend_.settings(); target.indexDirectory = indexDirectory->text().trimmed();
        backend_.clearIndexes(IndexCache::directory(target));
    });
    auto indexUsage = new QLabel; indexUsage->setObjectName("indexCacheUsage"); cache->addRow(indexUsage);
    auto indexScanning = std::make_shared<bool>(false);
    auto refreshIndexes = [this, indexDirectory, indexUsage, indexScanning, &dialog] {
        if (*indexScanning) return;
        *indexScanning = true;
        auto target = backend_.settings(); target.indexDirectory = indexDirectory->text().trimmed();
        auto watcher = new QFutureWatcher<QJsonObject>(&dialog);
        connect(watcher, &QFutureWatcher<QJsonObject>::finished, &dialog, [this, watcher, indexUsage, indexScanning] {
            auto stats = watcher->result(); watcher->deleteLater(); *indexScanning = false;
            indexUsage->setText(tr("持久缓存：%1 个索引 / %4 组源码 · %2 GiB%3").arg(stats.value("entries").toInt())
                .arg(stats.value("bytes").toDouble() / (1024 * 1024 * 1024), 0, 'f', 2)
                .arg(backend_.indexCacheWriting() ? tr(" · 正在后台保存…") : QString()).arg(stats.value("sourceEntries").toInt()));
        });
        watcher->setFuture(QtConcurrent::run([target] { return IndexCache::stats(target); }));
    };
    connect(indexDirectory, &QLineEdit::textChanged, &dialog, refreshIndexes);
    connect(&backend_, &Backend::indexCacheChanged, &dialog, refreshIndexes);
    refreshIndexes();
    auto maxTabs = spin(cache, tr("最多打开的类标签"), settings.maxTabs, 1, 64);
    auto hexPreview = spin(cache, tr("十六进制预览页大小（KiB）"), settings.hexPreviewKiB, 1, 16384);
    auto sourceLimit =
        spin(cache, tr("单文件查看 / 搜索大小限制（MiB）"), settings.sourceMiB, 1, 64);
    cache->addRow(
        new QLabel(tr("内存模式读取后删除按类临时文件；全项目搜索工作文件仍使用临时磁盘。")));
    auto clear = new QPushButton(tr("清理当前源码缓存"));
    auto usage = new QLabel(tr("正在统计磁盘缓存…"));
    usage->setObjectName("cacheUsage");
    cache->addRow(usage);
    auto timer = new QTimer(&dialog);
    auto scanning = std::make_shared<bool>(false);
    auto refreshUsage = [this, usage, scanning, &dialog] {
        if (*scanning)
            return;
        *scanning = true;
        auto path = backend_.workspacePath();
        const auto stats = backend_.cacheStats();
        const double memoryBytes = stats.value("mode").toString() == "memory"
                                       ? stats.value("source_bytes").toDouble()
                                       : 0.;
        auto task = new QFutureWatcher<QPair<qint64, qint64>>(&dialog);
        connect(task, &QFutureWatcher<QPair<qint64, qint64>>::finished, &dialog,
                [task, usage, scanning, memoryBytes] {
                    usage->setText(QObject::tr("源码磁盘缓存：%1 MiB · 内存缓存：%2 "
                                               "MiB\n保留的类索引：%3 MiB（不属于源码缓存）")
                                       .arg(task->result().first / 1048576., 0, 'f', 2)
                                       .arg(memoryBytes / 1048576., 0, 'f', 2)
                                       .arg(task->result().second / 1048576., 0, 'f', 2));
                    *scanning = false;
                    task->deleteLater();
                });
        task->setFuture(QtConcurrent::run([path] {
            qint64 n = 0, index = 0;
            if (!path.isEmpty()) {
                QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    auto relative = QDir(path).relativeFilePath(it.filePath());
                    if (relative == "classes.jsonl" || relative.startsWith("index/"))
                        index += it.fileInfo().size();
                    else
                        n += it.fileInfo().size();
                }
            }
            return QPair<qint64, qint64>{n, index};
        }));
    };
    connect(timer, &QTimer::timeout, &dialog, refreshUsage);
    timer->start(2000);
    refreshUsage();
    cache->addRow(clear);
    auto updateClear = [this, clear] {
        clear->setEnabled(!backend_.busy() && !backend_.preparing());
    };
    updateClear();
    connect(&backend_, &Backend::busyChanged, &dialog, updateClear);
    connect(&backend_, &Backend::preparationChanged, &dialog, updateClear);
    connect(&backend_, &Backend::busyChanged, &dialog, refreshUsage);
    connect(clear, &QPushButton::clicked, &dialog, [this, usage] {
        usage->setText(tr("正在清理源码缓存…"));
        backend_.clearCache();
    });
    auto appearance = page(tr("界面"));
    auto language = new QComboBox;
    language->setObjectName("interfaceLanguage");
    for (const auto &entry : Localization::languages()) language->addItem(entry.second, entry.first);
    language->setCurrentIndex(qMax(0, language->findData(settings.language)));
    appearance->addRow(tr("语言"), language);
    auto languageNote = new QLabel(tr("语言更改在重启软件后生效。可在 languages 目录添加语言文件。"));
    languageNote->setWordWrap(true); appearance->addRow(languageNote);
    auto fontPicker = [&](const QString &label, const QFont &value, const QString &name, const QString &configuredFamily) {
        auto row = new QHBoxLayout;
        auto family = new QComboBox; family->setObjectName(name + "Family");
        family->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        family->setMinimumContentsLength(14);
        family->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        family->addItem(tr("系统默认"), QString());
        for (const auto &name : QFontDatabase::families()) family->addItem(name, name);
        if (!configuredFamily.isEmpty() && family->findData(configuredFamily) < 0)
            family->addItem(configuredFamily, configuredFamily);
        family->setCurrentIndex(qMax(0, family->findData(configuredFamily)));
        auto size = new QSpinBox; size->setObjectName(name + "Size"); size->setRange(8, 48);
        size->setValue(qMax(8, value.pointSize()));
        row->addWidget(family, 1); row->addWidget(size); appearance->addRow(label, row);
        return qMakePair(family, size);
    };
    const auto uiFont = fontPicker(tr("UI 字体 / 字号"), settings.interfaceFont(), "uiFont", settings.uiFontFamily);
    const auto editorFont = fontPicker(tr("编辑器字体 / 字号"), settings.codeFont(), "editorFont", settings.editorFontFamily);
    const auto monoFont = fontPicker(tr("Smali / Hex 字体 / 字号"), settings.codeFont(true), "monoFont", settings.monoFontFamily);
    auto font = editorFont.second;
    auto memory = check(appearance, tr("显示内存占用（当前 / 可用 / 峰值）"), settings.showMemory);
    auto wrap = check(appearance, tr("代码自动换行"), settings.wordWrap);
    auto theme = new QComboBox;
    theme->addItem(tr("深色"), "dark");
    theme->addItem(tr("浅色"), "light");
    theme->setCurrentIndex(settings.theme == "light" ? 1 : 0);
    appearance->addRow(tr("界面主题"), theme);
    auto shortcuts = page(tr("快捷键"));
    QList<QPair<QAction *, QKeySequenceEdit *>> edits;
    QSet<QString> seen;
    for (auto action : findChildren<QAction *>())
        if (!action->shortcut().isEmpty() && !seen.contains(action->text())) {
            seen.insert(action->text());
            auto edit = new QKeySequenceEdit(action->shortcut());
            shortcuts->addRow(action->text(), edit);
            edits.append({action, edit});
        }
    auto mcp = page("MCP");
    auto enabled = check(mcp, tr("启用当前 GUI 项目的 MCP 服务"), settings.mcpEnabled);
    auto transport = new QComboBox;
    transport->addItem("stdio", "stdio");
    transport->addItem("HTTP (Streamable HTTP)", "http");
    transport->setCurrentIndex(settings.mcpTransport == "http" ? 1 : 0);
    mcp->addRow(tr("传输模式"), transport);
    auto host = new QLineEdit(settings.mcpHost);
    host->setObjectName("mcpHost");
    host->setPlaceholderText("127.0.0.1 / 0.0.0.0 / ::");
    mcp->addRow(tr("HTTP 绑定 IP"), host);
    auto port = spin(mcp, tr("HTTP 端口"), settings.mcpPort, 1024, 65535);
    auto config = new QPlainTextEdit;
    config->setReadOnly(true);
    config->setPlainText(QJsonDocument(mcp_->clientConfig()).toJson(QJsonDocument::Indented));
    mcp->addRow(tr("客户端配置"), config);
    auto updateConfig = [this, config, transport, port, host] {
        port->setEnabled(transport->currentData() == "http");
        QJsonObject value;
        if (transport->currentData() == "http") {
            QString ip = host->text().trimmed();
            if (ip == "0.0.0.0" || ip == "::")
                ip = "127.0.0.1";
            if (ip.contains(':'))
                ip = "[" + ip + "]";
            value = {{"mcpServers",
                      QJsonObject{
                          {"garlic",
                           QJsonObject{
                               {"url", QString("http://%1:%2/mcp").arg(ip).arg(port->value())}}}}}};
        } else
            value = {{"mcpServers",
                      QJsonObject{{"garlic",
                                   QJsonObject{{"command", QCoreApplication::applicationFilePath()},
                                               {"args", QJsonArray{"--mcp", "--socket",
                                                                   mcp_->endpoint()}}}}}}};
        config->setPlainText(QJsonDocument(value).toJson(QJsonDocument::Indented));
    };
    connect(transport, &QComboBox::currentIndexChanged, &dialog, updateConfig);
    connect(port, &QSpinBox::valueChanged, &dialog, updateConfig);
    connect(host, &QLineEdit::textChanged, &dialog, updateConfig);
    updateConfig();
    auto copy = new QPushButton(tr("复制配置"));
    mcp->addRow(copy);
    connect(copy, &QPushButton::clicked, &dialog,
            [config] { QApplication::clipboard()->setText(config->toPlainText()); });
    auto mcpNote = new QLabel(tr(
        "stdio 自动发现已启用的 GUI；HTTP 无 auth，绑定 0.0.0.0 "
        "可供局域网访问。\n局域网客户端需将配置 URL 的 IP 换成本机局域网 IP。\nGUI 需保持打开；AI "
        "的重命名会同步到当前项目，可撤销。"));
    mcpNote->setWordWrap(true);
    mcp->addRow(mcpNote);
    auto scripting = page(tr("脚本"));
    auto interpreter = [&](const QString &label, const QString &value, bool python) {
        auto row = new QWidget; auto layout = new QHBoxLayout(row); layout->setContentsMargins(0, 0, 0, 0);
        auto field = new QLineEdit(value); field->setObjectName(python ? "pythonPath" : "nodePath");
        field->setPlaceholderText(tr("自动：") + ScriptDialog::interpreter({}, python));
        auto browse = new QPushButton(tr("浏览…")); layout->addWidget(field, 1); layout->addWidget(browse);
        connect(browse, &QPushButton::clicked, &dialog, [&, field] {
            auto path = QFileDialog::getOpenFileName(&dialog, tr("选择解释器可执行文件"), field->text());
            if (!path.isEmpty()) field->setText(path);
        });
        scripting->addRow(label, row); return field;
    };
    auto pythonPath = interpreter("Python", settings.pythonPath, true);
    auto nodePath = interpreter("JavaScript / Node.js", settings.nodePath, false);
    auto scriptTimeout = spin(scripting, tr("脚本超时（秒）"), settings.scriptTimeout, 1, 86400);
    auto scriptNote = new QLabel(tr("留空按当前进程 PATH 查找 python3/python 或 node/nodejs。\n可选择 venv/Conda 的 python 可执行文件；不要填写 shell 命令。\n脚本使用该解释器安装的包，并具有当前用户的文件与网络权限。"));
    scriptNote->setWordWrap(true); scripting->addRow(scriptNote);
    auto capabilities = page(tr("引擎能力"));
    auto supported = new QLabel(
        tr("已接入：线程数、包排除、后台生成、Unicode 转义、\n注解显示、缓存、代码外观、快捷键和 "
           "MCP。\n\njadx 的 AUTO / SIMPLE / FALLBACK、变量反混淆、\nKotlin "
           "名称恢复、强制访问修饰符、常量替换、\n匿名类 / 方法 / Lambda 内联策略、finally / "
           "switch\n恢复策略、dx/d8 输入转换、数值格式和类型迭代次数，\n目前没有可直接复用的 "
           "garlic 运行时选项。\n这些处理仍使用 garlic 自身的默认流程。"));
    supported->setWordWrap(true);
    capabilities->addRow(supported);
    int navigationWidth = 160;
    for (int i = 0; i < navigation->count(); ++i)
        navigationWidth = qMax(navigationWidth, navigation->fontMetrics().horizontalAdvance(navigation->item(i)->text()) + 40);
    navigation->setFixedWidth(navigationWidth);
    dialog.resize(qMax(850, navigationWidth + 640), 680);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                                        QDialogButtonBox::RestoreDefaults);
    buttons->button(QDialogButtonBox::Save)->setText(tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("恢复默认设置"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog,
            [&] {
                AppSettings defaults;
                pythonPath->clear(); nodePath->clear(); scriptTimeout->setValue(defaults.scriptTimeout);
                threads->setValue(defaults.threads);
                cacheMode->setCurrentIndex(0);
                memory->setChecked(defaults.showMemory);
                host->setText("127.0.0.1");
                cacheLimit->setValue(defaults.cacheMiB);
                indexLimit->setValue(defaults.indexCacheGiB); indexDirectory->clear();
                maxTabs->setValue(defaults.maxTabs);
                sourceLimit->setValue(defaults.sourceMiB);
                hexPreview->setValue(defaults.hexPreviewKiB);
                language->setCurrentIndex(qMax(0, language->findData(defaults.language)));
                for (const auto &entry : {qMakePair(uiFont, defaults.interfaceFont()),
                                         qMakePair(editorFont, defaults.codeFont()),
                                         qMakePair(monoFont, defaults.codeFont(true))}) {
                    entry.first.first->setCurrentIndex(0);
                    entry.first.second->setValue(qMax(8, entry.second.pointSize()));
                }
                excluded->clear();
                background->setChecked(false);
                deobfuscate->setChecked(false);
                strings->setChecked(false);
                numberFormat->setCurrentIndex(0);
                controlFlow->setChecked(false);
                unflatten->setChecked(false);
                unicode->setChecked(false);
                metadata->setChecked(true);
                notice->setChecked(true);
                wrap->setChecked(false);
                enabled->setChecked(false);
                transport->setCurrentIndex(0);
                port->setValue(8650);
                theme->setCurrentIndex(0);
                for (const auto &entry : edits)
                    entry.second->setKeySequence(
                        QKeySequence(entry.first->property("defaultShortcut").toString()));
            });
    if (dialog.exec() != QDialog::Accepted)
        return;
    settings.pythonPath = pythonPath->text().trimmed(); settings.nodePath = nodePath->text().trimmed();
    settings.scriptTimeout = scriptTimeout->value();
    settings.cacheMode = cacheMode->currentData().toString();
    settings.showMemory = memory->isChecked();
    settings.mcpHost = host->text().trimmed();
    settings.threads = threads->value();
    settings.cacheMiB = cacheLimit->value();
    settings.indexCacheGiB = indexLimit->value(); settings.indexDirectory = indexDirectory->text().trimmed();
    settings.maxTabs = maxTabs->value();
    settings.hexPreviewKiB = hexPreview->value();
    settings.sourceMiB = sourceLimit->value();
    settings.fontSize = font->value();
    settings.language = language->currentData().toString();
    settings.uiFontFamily = uiFont.first->currentData().toString(); settings.uiFontSize = uiFont.second->value();
    settings.editorFontFamily = editorFont.first->currentData().toString();
    settings.monoFontFamily = monoFont.first->currentData().toString(); settings.monoFontSize = monoFont.second->value();
    settings.excluded = excluded->toPlainText().split('\n', Qt::SkipEmptyParts);
    settings.background = background->isChecked();
    settings.deobfuscate = deobfuscate->isChecked();
    settings.deobfuscateStrings = strings->isChecked();
    settings.numberFormat = numberFormat->currentData().toString();
    settings.simplifyControlFlow = controlFlow->isChecked();
    settings.unflatten = unflatten->isChecked();
    settings.escapeUnicode = unicode->isChecked();
    settings.showMetadata = metadata->isChecked();
    settings.showNotice = notice->isChecked();
    settings.wordWrap = wrap->isChecked();
    settings.theme = theme->currentData().toString();
    settings.mcpTransport = transport->currentData().toString();
    settings.mcpPort = port->value();
    settings.mcpEnabled = enabled->isChecked();
    for (const auto &edit : edits) {
        edit.first->setShortcut(edit.second->keySequence());
        QSettings().setValue("shortcuts-v3/" + edit.first->objectName(),
                             edit.second->keySequence().toString());
    }
    const auto previous = backend_.settings();
    const bool analysisChanged = previous.deobfuscate != settings.deobfuscate ||
        previous.deobfuscateStrings != settings.deobfuscateStrings || previous.numberFormat != settings.numberFormat ||
        previous.simplifyControlFlow != settings.simplifyControlFlow || previous.unflatten != settings.unflatten;
    settings.save();
    applySettings(settings, analysisChanged);
    if (analysisChanged && !backend_.input().isEmpty()) {
        if (QMessageBox::question(this, tr("重建索引"),
                tr("反混淆、数值格式或控制流选项已更改。是否立即重建当前文件索引？取消后可手动重建。"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            backend_.rebuildIndex();
    }
    if (!backend_.input().isEmpty())
        refreshAliases();
}
