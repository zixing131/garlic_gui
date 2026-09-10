#include "backend.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

Backend::Backend(QObject *parent) : QObject(parent) {
    const QString adjacent = QCoreApplication::applicationDirPath() + "/garlic"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
    engine_ = QFileInfo::exists(adjacent) ? adjacent : QStandardPaths::findExecutable("garlic");
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Backend::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &Backend::readOutput);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &Backend::finish);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && busy()) {
            job_ = Job::None;
            emit busyChanged(false);
            emit failed(tr("无法启动 garlic：%1\n请在“文件 → 选择引擎”中指定本项目编译的 garlic。").arg(process_.errorString()));
        }
    });
}

Backend::~Backend() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}

bool Backend::supportsSmali() const {
    const auto ext = QFileInfo(input_).suffix().toLower();
    return ext == "apk" || ext == "dex" || ext == "xapk" || ext == "apks";
}

bool Backend::safeClassName(const QString &name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':')) return false;
    for (const QChar c : name) if (c.unicode() < 32) return false;
    for (const QString &part : name.split('/'))
        if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}

void Backend::start(Job job, const QStringList &arguments) {
    job_ = job;
    canceled_ = false;
    errorTail_.clear();
    process_.setWorkingDirectory(workspace_->path());
    process_.setStandardOutputFile(QFileInfo(input_).suffix().compare("class", Qt::CaseInsensitive) == 0
        && job != Job::Index ? jobDir_ + "/source.java" : QString());
    emit busyChanged(true);
    process_.start(engine_, arguments);
}

void Backend::open(const QString &path) {
    if (busy()) return;
    if (!QFileInfo(path).isFile()) { emit failed(tr("文件不存在：%1").arg(path)); return; }
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!QStringList{"apk", "dex", "jar", "war", "class", "xapk", "apks"}.contains(ext)) {
        emit failed(tr("请选择 APK、DEX、JAR、WAR 或 CLASS 文件。")); return;
    }
    auto workspace = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/garlic-gui-XXXXXX");
    if (!workspace->isValid()) { emit failed(tr("无法创建临时工作目录。")); return; }
    workspace_ = std::move(workspace);
    input_ = QFileInfo(path).absoluteFilePath();
    cache_.clear();
    jobDir_ = workspace_->path() + "/index";
    QDir().mkpath(jobDir_);
    start(Job::Index, {input_, "-I", workspace_->path() + "/classes.jsonl", "-o", jobDir_, "-t", "2"});
}

void Backend::request(const QString &name, bool smali) {
    if (busy() || !workspace_) return;
    if (!safeClassName(name)) { emit failed(tr("类名包含无效路径。")); return; }
    if (smali && !supportsSmali()) { emit failed(tr("Smali 仅适用于 APK / DEX。")); return; }
    const QString key = name + (smali ? ":smali" : ":java");
    if (cache_.contains(key)) { emit sourceReady(name, smali, cache_.value(key)); return; }
    currentName_ = name;
    smali_ = smali;
    jobDir_ = workspace_->path() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(jobDir_);
    QStringList args{input_, "-c", name, "-o", jobDir_, "-t", "2"};
    if (smali) args << "-s";
    start(Job::Source, args);
}

void Backend::exportSources(const QString &directory, bool smali) {
    if (busy() || !workspace_) return;
    if (smali && !supportsSmali()) return;
    if (QFileInfo::exists(directory) || !QDir().mkpath(directory)) {
        emit failed(tr("导出目标必须是可创建的新目录：%1").arg(directory)); return;
    }
    exportDir_ = directory;
    jobDir_ = directory;
    QStringList args{input_, "-o", directory, "-t", "2"};
    if (smali) args << "-s";
    start(Job::Export, args);
}

void Backend::cancel() {
    if (!busy()) return;
    canceled_ = true;
    process_.kill();
}

void Backend::readOutput() {
    QByteArray bytes = process_.readAllStandardOutput() + process_.readAllStandardError();
    bytes.replace('\b', ' ');
    const QString text = QString::fromUtf8(bytes);
    errorTail_ = (errorTail_ + text).right(4000);
    if (!text.trimmed().isEmpty()) emit log(text.right(8000));
}

void Backend::finish(int code, QProcess::ExitStatus status) {
    if (!busy()) return;
    readOutput();
    const auto completed = job_;
    job_ = Job::None;
    emit busyChanged(false);
    if (canceled_) {
        emit log(completed == Job::Export ? tr("导出已取消；目标目录保留已完成的部分文件。") : tr("任务已取消，可重新选择类重试。"));
        return;
    }
    if (code != 0 || status != QProcess::NormalExit) {
        emit failed(tr("garlic %1（退出码 %2）。\n%3%4")
            .arg(status == QProcess::CrashExit ? tr("进程崩溃") : tr("执行失败"))
            .arg(code).arg(errorTail_).arg(completed == Job::Export ? tr("\n导出目录可能包含部分结果。") : QString()));
        return;
    }
    if (completed == Job::Index) {
        QFile file(workspace_->path() + "/classes.jsonl");
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
            emit failed(tr("无法读取类索引；请使用本项目编译的 garlic 引擎。")); return;
        }
        QSet<QString> names;
        while (!file.atEnd()) {
            const auto line = file.readLine();
            const auto doc = QJsonDocument::fromJson(line);
            const auto name = doc.object().value("name").toString();
            if (!safeClassName(name)) { emit failed(tr("引擎返回了无效的类索引。")); return; }
            names.insert(name);
        }
        QStringList classes = names.values();
        classes.sort();
        if (classes.isEmpty()) { emit failed(tr("文件中没有可浏览的顶层类。")); return; }
        emit indexed(classes);
    } else if (completed == Job::Source) {
        QString path = jobDir_ + "/" + currentName_ + (smali_ ? ".smali" : ".java");
        if (QFileInfo(input_).suffix().compare("class", Qt::CaseInsensitive) == 0) path = jobDir_ + "/source.java";
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
            emit failed(tr("引擎没有生成该类的%1源码。详情见日志。").arg(smali_ ? " Smali " : " Java ")); return;
        }
        cache_.insert(currentName_ + (smali_ ? ":smali" : ":java"), path);
        emit sourceReady(currentName_, smali_, path);
    } else {
        QDirIterator files(exportDir_, {"*.java", "*.smali"}, QDir::Files, QDirIterator::Subdirectories);
        if (!files.hasNext()) { emit failed(tr("引擎没有生成源码。导出目录：%1").arg(exportDir_)); return; }
        emit exported(exportDir_);
    }
}
