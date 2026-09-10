#include "nativeanalysisdialog.h"
#include "codeeditor.h"
#include "resources.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtWidgets>

namespace {
const QList<QPair<QString, QString>> outputs = {
    {"概览", ""},          {"导出符号", ".exports"}, {"导入符号", ".imports"},
    {"函数", ".entries"}, {"函数引用", ".func_xref"}, {"字符串", ".strings"},
    {"PC 引用", ".pc_xrefs"}, {"反汇编", ".dissembly"},
    {"CFG 节点", ".cfg_nodes"}, {"CFG 边", ".cfg_edges"}};
}

NativeAnalysisDialog::NativeAnalysisDialog(const QString &engine, const QString &sourcePath,
                                           const QString &archiveEntry, QWidget *parent)
    : QDialog(parent), engine_(engine), sourcePath_(sourcePath), archiveEntry_(archiveEntry),
      workspace_(std::make_shared<QTemporaryDir>(QDir::tempPath() + "/garlic-native-XXXXXX")) {
    setObjectName("nativeAnalysisDialog");
    const auto display = archiveEntry_.isEmpty() ? QFileInfo(sourcePath_).fileName()
                                                  : QFileInfo(archiveEntry_).fileName();
    setWindowTitle(tr("Native 分析：%1").arg(display));
    resize(1120, 760);
    setMinimumSize(700, 480);
    auto root = new QVBoxLayout(this);
    auto tools = new QHBoxLayout;
    status_ = new QLabel(tr("正在准备 Native 文件…"));
    tools->addWidget(status_, 1);
    find_ = new QLineEdit;
    find_->setPlaceholderText(tr("在当前结果中查找…"));
    find_->setClearButtonEnabled(true);
    tools->addWidget(find_);
    auto next = new QPushButton(tr("下一个"));
    tools->addWidget(next);
    export_ = new QPushButton(tr("导出分析结果…"));
    export_->setEnabled(false);
    tools->addWidget(export_);
    root->addLayout(tools);
    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    root->addWidget(tabs_, 1);
    auto close = new QPushButton(tr("关闭"));
    auto footer = new QHBoxLayout;
    footer->addStretch();
    footer->addWidget(close);
    root->addLayout(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(next, &QPushButton::clicked, this, &NativeAnalysisDialog::findNext);
    connect(find_, &QLineEdit::returnPressed, this, &NativeAnalysisDialog::findNext);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        auto editor = qobject_cast<CodeEditor *>(tabs_->widget(index));
        if (!editor || editor->property("analysisLoaded").toBool())
            return;
        const auto path = editor->property("analysisOutputPath").toString();
        if (path.isEmpty())
            return;
        editor->setProperty("analysisLoaded", true);
        editor->setPlainText(tr("正在后台加载结果…"));
        QPointer<CodeEditor> target(editor);
        auto watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [watcher, target] {
            const auto bytes = watcher->result();
            watcher->deleteLater();
            if (target)
                target->setPlainText(QString::fromUtf8(bytes));
        });
        watcher->setFuture(QtConcurrent::run([path] {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QObject::tr("无法读取分析结果：%1").arg(file.errorString()).toUtf8();
            auto bytes = file.read(8 * 1024 * 1024);
            if (!file.atEnd())
                bytes += "\n\n[结果超过 8 MiB，界面只显示前 8 MiB；可导出完整文件。]\n";
            return bytes;
        }));
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &NativeAnalysisDialog::loadResults);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            status_->setText(tr("无法启动 garlic：%1").arg(process_.errorString()));
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        const auto directory = QFileDialog::getExistingDirectory(this, tr("导出 Native 分析结果"));
        if (directory.isEmpty())
            return;
        int copied = 0;
        for (const auto &output : outputs) {
            if (output.second.isEmpty())
                continue;
            const auto source = analysisPath_ + output.second;
            if (!QFileInfo(source).isFile())
                continue;
            const auto target = QDir(directory).filePath(QFileInfo(source).fileName());
            QFile::remove(target);
            if (QFile::copy(source, target))
                ++copied;
        }
        status_->setText(tr("已导出 %1 个分析文件").arg(copied));
    });
    QTimer::singleShot(0, this, &NativeAnalysisDialog::prepare);
}

void NativeAnalysisDialog::prepare() {
    if (!workspace_->isValid()) {
        status_->setText(tr("无法创建 Native 分析工作目录。"));
        return;
    }
    const auto ext = QFileInfo(archiveEntry_.isEmpty() ? sourcePath_ : archiveEntry_).suffix();
    analysisPath_ = workspace_->filePath("library." + (ext.isEmpty() ? QStringLiteral("so") : ext));
    const auto source = sourcePath_;
    const auto entry = archiveEntry_;
    const auto target = analysisPath_;
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) {
            status_->setText(error);
            return;
        }
        startAnalysis();
    });
    watcher->setFuture(QtConcurrent::run([source, entry, target] {
        if (entry.isEmpty()) {
            if (QFile::copy(source, target))
                return QString();
            return QObject::tr("无法复制 Native 文件到分析目录。");
        }
        QString error;
        const auto bytes = Resources::read(source, entry, 1024LL * 1024 * 1024, &error);
        if (!error.isEmpty())
            return error;
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return QObject::tr("无法提取 Native 文件：%1").arg(file.errorString());
        return QString();
    }));
}

void NativeAnalysisDialog::startAnalysis() {
    status_->setText(tr("正在运行 rosemary Native 分析…"));
    process_.setWorkingDirectory(workspace_->path());
    process_.start(engine_, {analysisPath_, "-n"});
}

void NativeAnalysisDialog::loadResults(int code, QProcess::ExitStatus status) {
    const auto output = QString::fromUtf8(process_.readAllStandardOutput()) +
                        QString::fromUtf8(process_.readAllStandardError());
    if (code != 0 || status != QProcess::NormalExit) {
        status_->setText(tr("Native 分析失败（退出码 %1）").arg(code));
        auto error = new QPlainTextEdit(output);
        error->setReadOnly(true);
        tabs_->addTab(error, tr("错误"));
        return;
    }
    int loaded = 0;
    for (const auto &item : outputs) {
        const auto path = item.second.isEmpty() ? QString() : analysisPath_ + item.second;
        if (!path.isEmpty() && !QFileInfo(path).isFile())
            continue;
        auto editor = new CodeEditor(false);
        editor->setReadOnly(true);
        editor->document()->setProperty("language", "text");
        if (path.isEmpty()) {
            editor->setProperty("analysisLoaded", true);
            editor->setPlainText(output);
        } else {
            editor->setProperty("analysisOutputPath", path);
            editor->setPlainText(tr("选择此标签以加载分析结果。"));
        }
        tabs_->addTab(editor, item.first);
        ++loaded;
    }
    export_->setEnabled(loaded > 1);
    status_->setText(tr("Native 分析完成 · %1 个结果视图").arg(loaded));
}

void NativeAnalysisDialog::findNext() {
    auto editor = qobject_cast<QPlainTextEdit *>(tabs_->currentWidget());
    if (!editor || find_->text().isEmpty())
        return;
    if (!editor->find(find_->text())) {
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::Start);
        editor->setTextCursor(cursor);
        editor->find(find_->text());
    }
}
