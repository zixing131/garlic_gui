#include "scriptdialog.h"
#include "mainwindow.h"
#include "mcpserver.h"
#include <QtWidgets>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSyntaxHighlighter>

namespace {
class ScriptHighlighter : public QSyntaxHighlighter {
  public:
    explicit ScriptHighlighter(QPlainTextEdit *editor)
        : QSyntaxHighlighter(editor->document()), editor_(editor) {
        editor->installEventFilter(this);
    }
    void setPython(bool python) { python_ = python; rehighlight(); }
  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
            rehighlight();
        return QSyntaxHighlighter::eventFilter(object, event);
    }
    void highlightBlock(const QString &text) override {
        const bool light = editor_->palette().color(QPalette::Base).lightness() > 128;
        const QColor keyword(light ? "#6530a3" : "#c4a3ff"),
            string(light ? "#267650" : "#a4e4bd"), comment(light ? "#5c7483" : "#8b9bac"),
            number(light ? "#925800" : "#e6b878"), function(light ? "#2556a8" : "#82aaff");
        static const QRegularExpression pythonWords(QStringLiteral(
            "^(?:False|None|True|and|as|assert|async|await|break|class|continue|def|del|elif|else|except|"
            "finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield)$"));
        static const QRegularExpression javascriptWords(QStringLiteral(
            "^(?:async|await|break|case|catch|class|const|continue|debugger|default|delete|do|else|export|"
            "extends|false|finally|for|from|function|if|import|in|instanceof|let|new|null|of|return|static|"
            "super|switch|this|throw|true|try|typeof|undefined|var|void|while|with|yield)$"));
        static const QRegularExpression numeric(QStringLiteral(
            "(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|[0-9][0-9_]*(?:\\.[0-9_]*)?(?:[eE][+-]?[0-9_]+)?)[njJ]?"));
        // States: JS block comment, Python triple quotes, JS template literal.
        int state = qMax(0, previousBlockState()), i = 0;
        setCurrentBlockState(0);
        auto quoted = [&](int start, int content, const QString &delimiter, int continuation) {
            int end = content;
            bool closed = false;
            while (end < text.size()) {
                if (text[end] == '\\') { end += 2; continue; }
                if (text.mid(end, delimiter.size()) == delimiter) {
                    end += delimiter.size(); closed = true; break;
                }
                ++end;
            }
            end = qMin(end, int(text.size()));
            setFormat(start, end - start, string);
            if (!closed && continuation) setCurrentBlockState(continuation);
            i = end;
        };
        while (i < text.size()) {
            const int start = i;
            if (state == 1 || (!python_ && text.mid(i, 2) == "/*")) {
                const int end = text.indexOf("*/", i + (state == 1 ? 0 : 2));
                i = end < 0 ? text.size() : end + 2;
                setFormat(start, i - start, comment);
                if (end < 0) setCurrentBlockState(1);
                state = 0; continue;
            }
            if (state >= 2) {
                const QString delimiter = state == 2 ? "'''" : state == 3 ? "\"\"\"" : "`";
                quoted(i, i, delimiter, state); state = 0; continue;
            }
            if ((python_ && text[i] == '#') || (!python_ && text.mid(i, 2) == "//")) {
                setFormat(i, text.size() - i, comment); break;
            }
            if (text[i] == '\'' || text[i] == '"' || (!python_ && text[i] == '`')) {
                const auto quote = text[i];
                const bool triple = python_ && text.mid(i, 3) == QString(3, quote);
                const int continuation = triple ? (quote == '\'' ? 2 : 3) : quote == '`' ? 4 : 0;
                quoted(i, i + (triple ? 3 : 1), QString(triple ? 3 : 1, quote), continuation);
                continue;
            }
            if (text[i].isDigit()) {
                const auto match = numeric.match(text, i, QRegularExpression::NormalMatch,
                                                 QRegularExpression::AnchorAtOffsetMatchOption);
                if (match.hasMatch()) {
                    i += match.capturedLength(); setFormat(start, i - start, number); continue;
                }
            }
            if (text[i].isLetter() || text[i] == '_' || (!python_ && text[i] == '$')) {
                while (i < text.size() && (text[i].isLetterOrNumber() || text[i] == '_' || (!python_ && text[i] == '$'))) ++i;
                const auto word = text.mid(start, i - start);
                if ((python_ ? pythonWords : javascriptWords).match(word).hasMatch())
                    setFormat(start, i - start, keyword);
                else {
                    int next = i;
                    while (next < text.size() && text[next].isSpace()) ++next;
                    if ((next < text.size() && text[next] == '(') || (python_ && start > 0 && text[start - 1] == '@'))
                        setFormat(start, i - start, function);
                }
                continue;
            }
            ++i;
        }
        if (text.isEmpty()) setCurrentBlockState(state);
    }
  private:
    QPlainTextEdit *editor_;
    bool python_ = true;
};
} // namespace

QString ScriptDialog::interpreter(const QString &configured, bool python) {
    if (!configured.trimmed().isEmpty()) {
        const auto path = configured.trimmed();
        return QFileInfo(path).isAbsolute() ? path : QStandardPaths::findExecutable(path);
    }
    for (const auto &name : python ? QStringList{"python3", "python"} : QStringList{"node", "nodejs"}) {
        const auto path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) return path;
    }
    return {};
}
ScriptDialog::ScriptDialog(MainWindow *window, const QString &host) : QDialog(window), window_(window), host_(host.isEmpty() ? QCoreApplication::applicationFilePath() : host), server_(new McpServer(window, this)) {
    Q_INIT_RESOURCE(scripts);
    setObjectName("scriptDialog"); setWindowTitle(tr("脚本执行 — Python / JavaScript")); resize(960, 760);
    setAttribute(Qt::WA_DeleteOnClose);
    auto layout = new QVBoxLayout(this);
    auto row = new QHBoxLayout;
    language_ = new QComboBox; language_->setObjectName("scriptLanguage");
    language_->addItem("Python", "python"); language_->addItem("JavaScript (Node.js)", "javascript");
    row->addWidget(language_);
    auto open = new QPushButton(tr("打开脚本…")), save = new QPushButton(tr("保存脚本…"));
    row->addWidget(open); row->addWidget(save);
    run_ = new QPushButton(tr("运行")); run_->setObjectName("runScript");
    stop_ = new QPushButton(tr("停止")); stop_->setObjectName("stopScript"); stop_->setEnabled(false);
    row->addWidget(run_); row->addWidget(stop_);
    auto docs = new QPushButton(tr("API 文档")); row->addWidget(docs); layout->addLayout(row);
    file_ = new QLineEdit; file_->setObjectName("scriptFile"); file_->setReadOnly(true); file_->setPlaceholderText(tr("未保存脚本；运行时使用当前 APK 所在目录")); layout->addWidget(file_);
    arguments_ = new QLineEdit(QSettings().value("scripts/arguments", "{}").toString());
    arguments_->setObjectName("scriptArguments"); arguments_->setPlaceholderText(tr("脚本参数 JSON，例如 {\"key\": 42}")); layout->addWidget(arguments_);
    auto split = new QSplitter(Qt::Vertical);
    code_ = new QPlainTextEdit; code_->setObjectName("scriptCode"); code_->setLineWrapMode(QPlainTextEdit::NoWrap);
    code_->setFont(window_->backend()->settings().codeFont());
    auto highlighter = new ScriptHighlighter(code_);
    output_ = new QPlainTextEdit; output_->setObjectName("scriptOutput"); output_->setReadOnly(true); output_->setMaximumBlockCount(2000);
    output_->setFont(window_->backend()->settings().codeFont());
    split->addWidget(code_); split->addWidget(output_); split->setStretchFactor(0, 3); split->setStretchFactor(1, 1); layout->addWidget(split);
    code_->setPlainText(QSettings().value("scripts/python", "from garlic import api\nprint(api.get_status())\nprint([tool['name'] for tool in api.tools()])\n").toString());
    connect(language_, &QComboBox::currentIndexChanged, this, [this, highlighter] {
        saveDraft(); draftLanguage_ = language_->currentData().toString(); file_->clear();
        highlighter->setPython(draftLanguage_ == "python");
        code_->setPlainText(QSettings().value("scripts/" + draftLanguage_, draftLanguage_ == "python"
            ? "from garlic import api\nprint(api.get_status())\n"
            : "console.log(await garlic.get_status());\nconsole.log((await garlic.tools()).map(tool => tool.name));\n").toString());
    });
    connect(open, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, tr("打开脚本"), file_->text(), "Scripts (*.py *.js *.cjs);;All files (*)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1048576) { append(tr("无法读取脚本（最大 4 MiB）：") + file.errorString()); return; }
        saveDraft(); language_->setCurrentIndex(path.endsWith(".py", Qt::CaseInsensitive) ? 0 : 1);
        code_->setPlainText(QString::fromUtf8(file.readAll())); file_->setText(path);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, tr("保存脚本"), file_->text().isEmpty() ? (draftLanguage_ == "python" ? "script.py" : "script.js") : file_->text());
        if (path.isEmpty()) return;
        QSaveFile file(path); const auto bytes = code_->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { append(file.errorString()); return; }
        file_->setText(path); saveDraft();
    });
    connect(docs, &QPushButton::clicked, this, [this] {
        const auto name = draftLanguage_ == "python" ? "SCRIPTING-PYTHON.md" : "SCRIPTING-JAVASCRIPT.md";
        auto path = QCoreApplication::applicationDirPath() + '/' + name;
#ifdef Q_OS_MAC
        path = QCoreApplication::applicationDirPath() + "/../Resources/" + name;
#endif
        if (!QFileInfo::exists(path)) path = QCoreApplication::applicationDirPath() + "/../" + name;
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) append(tr("文档位置：") + path);
    });
    connect(run_, &QPushButton::clicked, this, &ScriptDialog::run);
    connect(stop_, &QPushButton::clicked, this, &ScriptDialog::stop);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { append(stdoutDecoder_(process_.readAllStandardOutput().right(65536))); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { append(stderrDecoder_(process_.readAllStandardError().right(65536))); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        append(process_.errorString());
        if (process_.state() == QProcess::NotRunning) { timeout_.stop(); killTimer_.stop(); server_->stop(); run_->setEnabled(true); stop_->setEnabled(false); language_->setEnabled(true); }
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        timeout_.stop(); killTimer_.stop();
        append(tr("\n[结束] exit=%1%2").arg(code).arg(status == QProcess::CrashExit ? tr("，进程已终止") : QString()));
        run_->setEnabled(true); stop_->setEnabled(false); language_->setEnabled(true);
        server_->stop();
    });
    timeout_.setSingleShot(true); killTimer_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { append(tr("脚本超时，正在停止…")); stop(); });
    connect(&killTimer_, &QTimer::timeout, &process_, &QProcess::kill);
}
ScriptDialog::~ScriptDialog() {
    saveDraft(); server_->stop();
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(1000); }
}
void ScriptDialog::closeEvent(QCloseEvent *event) {
    saveDraft(); server_->stop();
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(500); }
    QDialog::closeEvent(event);
}
void ScriptDialog::saveDraft() {
    QSettings().setValue("scripts/" + draftLanguage_, code_->toPlainText().left(4 * 1048576));
    QSettings().setValue("scripts/arguments", arguments_->text());
}
void ScriptDialog::append(const QString &text) {
    // Bound even a script that prints one enormous line without newlines.
    if (output_->document()->characterCount() > 256 * 1024) output_->setPlainText(output_->toPlainText().right(128 * 1024));
    auto cursor = output_->textCursor(); cursor.movePosition(QTextCursor::End); cursor.insertText(text); output_->setTextCursor(cursor); output_->ensureCursorVisible();
}
void ScriptDialog::stop() { server_->stop(); process_.terminate(); killTimer_.start(750); }
void ScriptDialog::run() {
    if (process_.state() != QProcess::NotRunning) return;
    killTimer_.stop(); timeout_.stop(); saveDraft(); output_->clear();
    stdoutDecoder_ = QStringDecoder(QStringDecoder::Utf8); stderrDecoder_ = QStringDecoder(QStringDecoder::Utf8);
    QJsonParseError parse;
    auto args = QJsonDocument::fromJson(arguments_->text().toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !args.isObject()) { append(tr("脚本参数必须是 JSON 对象。")); return; }
    const auto settings = window_->backend()->settings();
    const bool python = draftLanguage_ == "python";
    const auto runtime = interpreter(python ? settings.pythonPath : settings.nodePath, python);
    if (runtime.isEmpty()) { append(tr("未找到 %1，请在设置 → 脚本中指定解释器可执行文件。").arg(python ? "Python" : "Node.js")); return; }
    QString error;
    temporary_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/garlic-script-XXXXXX");
    if (!temporary_->isValid()) { append(tr("无法创建脚本工作目录。")); return; }
    auto write = [this](const QString &name, const QByteArray &bytes) {
        QFile file(temporary_->filePath(name)); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QFile sdk(python ? ":/scripts/garlic.py" : ":/scripts/garlic.js");
    if (!sdk.open(QIODevice::ReadOnly) || !write(python ? "garlic.py" : "garlic.js", sdk.readAll())) { append(tr("无法写入脚本 SDK。")); return; }
    const auto work = !file_->text().isEmpty() ? QFileInfo(file_->text()).absolutePath()
        : !window_->backend()->input().isEmpty() ? QFileInfo(window_->backend()->input()).absolutePath() : QDir::homePath();
    const QJsonObject context{{"inputs", QJsonArray::fromStringList(window_->backend()->inputs())},
        {"class_name", window_->selectedClass()}, {"selected_text", window_->editor() ? window_->editor()->textCursor().selectedText() : QString()},
        {"arguments", args.object()}, {"script_path", file_->text()}, {"working_directory", work}};
    if (!write("context.json", QJsonDocument(context).toJson()) || !write("script.py", code_->toPlainText().toUtf8())) { append(tr("无法写入脚本。")); return; }
    if (!server_->start(&error, true)) { append(error); return; }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("GARLIC_SCRIPT_HOST", host_);
    environment.insert("GARLIC_SCRIPT_SOCKET", server_->endpoint());
    environment.insert("GARLIC_SCRIPT_CONTEXT", temporary_->filePath("context.json"));
    environment.insert("GARLIC_SCRIPT_JS", temporary_->filePath("garlic.js"));
    environment.insert("PYTHONIOENCODING", "utf-8");
    if (python && !settings.pythonPath.isEmpty()) environment.remove("PYTHONHOME");
    environment.insert("PATH", QFileInfo(runtime).absolutePath() + QDir::listSeparator() + environment.value("PATH"));
    environment.insert("PYTHONPATH", temporary_->path() + QDir::listSeparator() + work + QDir::listSeparator() + environment.value("PYTHONPATH"));
    process_.setProcessEnvironment(environment); process_.setWorkingDirectory(work);
    append(tr("[运行] %1\n工作目录：%2\n").arg(runtime, work));
    run_->setEnabled(false); stop_->setEnabled(true); language_->setEnabled(false);
    timeout_.start(settings.scriptTimeout * 1000);
    if (python) { process_.start(runtime, {"-u", temporary_->filePath("script.py")}); process_.closeWriteChannel(); }
    else {
        process_.start(runtime, {"-"});
        process_.write("require(process.env.GARLIC_SCRIPT_JS);\n(async () => {\n" + code_->toPlainText().toUtf8() + "\n})().then(() => garlic.close(), error => { console.error(error.stack || error); garlic.close(); process.exitCode = 1; });\n");
        process_.closeWriteChannel();
    }
}
