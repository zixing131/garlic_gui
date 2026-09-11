#include "backend.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <future>

Backend::Backend(QObject *parent)
    : QObject(parent), project_(this), settings_(AppSettings::load()) {
    connect(&project_, &Project::renamed, this, [this] {
        cancelSearch();
        if (searchWarmControl_)
            searchWarmControl_->canceled = true;
        searchIndex_ = std::make_shared<SearchIndex>();
        if (fullReady_ || preparing_)
            warmSearchIndex();
    });
    connect(this, &Backend::failed, this,
            [this] { setProperty("errorCount", property("errorCount").toInt() + 1); });
    connect(this, &Backend::log, this, [this](const QString &text) {
        int n = 0;
        for (const auto &line : text.split('\n'))
            if (line.contains("warning", Qt::CaseInsensitive))
                n++;
        setProperty("warningCount", property("warningCount").toInt() + n);
    });
    connect(&background_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &Backend::prepareFinished);
    connect(&background_, &QProcess::readyReadStandardError, this, [this] {
        const auto text = QString::fromUtf8(background_.readAllStandardError());
        backgroundErrorTail_ = (backgroundErrorTail_ + text).right(4000);
        emit log(text.right(4000));
    });
    connect(&background_, &QProcess::readyReadStandardOutput, this, [this] {
        const auto output = QString::fromUtf8(background_.readAllStandardOutput());
        static const QRegularExpression pattern("Progress : (\\d+) \\((\\d+)\\)");
        auto matches = pattern.globalMatch(output);
        while (matches.hasNext()) {
            auto match = matches.next();
            const int total = match.captured(2).toInt();
            if (total > 0) emit loadProgress(tr("源码生成"), qMin(99, match.captured(1).toInt() * 100 / total));
        }
    });
    connect(&background_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            prepareFinished(-1, QProcess::CrashExit);
    });
    const QString adjacent = QCoreApplication::applicationDirPath() + "/garlic"
#ifdef Q_OS_WIN
                                                                      ".exe"
#endif
        ;
    engine_ = adjacent;
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Backend::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &Backend::readOutput);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &Backend::finish);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // FailedToStart can arrive before (or without) finished(), depending on
        // the platform.  Use the normal completion path so indexing_ is cleared
        // together with job_; otherwise busy() can remain true after reporting
        // the launch error.
        if (error == QProcess::FailedToStart && job_ != Job::None)
            finish(-1, QProcess::CrashExit);
    });
}

Backend::~Backend() {
    project_.cancelPendingWork();
    if (metadataCanceled_)
        metadataCanceled_->store(true);
    if (indexCanceled_)
        indexCanceled_->store(true);
    if (exportControl_)
        exportControl_->canceled = true;
    cancelSearch();
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    searchControl_.reset();
    searchWarmControl_.reset();
    sourceGenerating_->store(false);
    disconnect(&background_, nullptr, this, nullptr);
    background_.kill();
    background_.waitForFinished(3000);
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}

bool Backend::prepareExit(const QString &executable) {
    project_.cancelPendingWork(); cancel();
    disconnect(&background_, nullptr, this, nullptr); disconnect(&process_, nullptr, this, nullptr);
    for (auto process : {&background_, &process_}) if (process->state() != QProcess::NotRunning) {
        process->kill(); process->waitForFinished(500);
        if (process->state() != QProcess::NotRunning) return false;
    }
    // The metadata process belongs to its worker thread. Its loop observes the
    // cancellation token and reaps the process before clearing this flag.
    QElapsedTimer wait; wait.start();
    const auto engineActive = [this] {
        for (const auto &flag : metadataEngines_) if (flag->load()) return true;
        return false;
    };
    while (engineActive() && wait.elapsed() < 1500) QThread::msleep(5);
    if (engineActive()) return false;
    if (!ownedWorkspaces_.isEmpty()) {
        if (!QProcess::startDetached(executable, QStringList{"--cleanup-workspaces"} + ownedWorkspaces_)) return false;
        if (workspace_) workspace_->setAutoRemove(false);
    }
    return true;
}

QString Backend::classInput(const QString &name) const {
    return project_.info(name).value("input").toString(input_);
}
bool Backend::supportsSmali(const QString &name) const {
    const auto ext = QFileInfo(name.isEmpty() ? input_ : classInput(name)).suffix().toLower();
    if (ext == "apk" || ext == "dex" || ext == "xapk" || ext == "apks")
        return true;
    return ext == "zip" && project_.info(name).value("origin").toString().endsWith(".dex",
                                                                                Qt::CaseInsensitive);
}

bool Backend::safeClassName(const QString &name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':'))
        return false;
    for (const QChar c : name)
        if (c.unicode() < 32)
            return false;
    for (const QString &part : name.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    return true;
}

void Backend::start(Job job, const QStringList &arguments) {
    job_ = job;
    canceled_ = false;
    errorTail_.clear();
    process_.setWorkingDirectory(workspace_->path());
    applyEnvironment(process_, jobDir_);
    if (job == Job::Source && QFileInfo(arguments.value(0)).suffix().compare("apk", Qt::CaseInsensitive) == 0) {
        const auto origin = project_.info(argumentClass_).value("origin").toString();
        if (!origin.isEmpty() && origin.endsWith(".dex", Qt::CaseSensitive)) {
            auto environment = process_.processEnvironment();
            environment.insert("GARLIC_DEX_ENTRY", origin);
            process_.setProcessEnvironment(environment);
        }
    }
    if (job == Job::Index) {
        auto environment = process_.processEnvironment();
        environment.insert("GARLIC_COMPACT_INDEX", "2");
        if (directoryOnly_)
            environment.insert("GARLIC_DIRECTORY_INDEX", "names");
        process_.setProcessEnvironment(environment);
    }
    process_.setStandardOutputFile(
        QFileInfo(arguments.value(0)).suffix().compare("class", Qt::CaseInsensitive) == 0 &&
                job != Job::Index
            ? jobDir_ + "/source.java"
            : QString());
    emit busyChanged(true);
    if (job == Job::Index) {
        emit loadProgress(tr("读取类目录"), 0);
        QFile::remove(workspace_->path() + "/classes.jsonl");
        indexProducer_ = std::make_shared<std::atomic_int>(0);
        readIndex();
    }
    process_.start(engine_, arguments);
}

void Backend::open(const QString &path) { openPaths({path}); }
void Backend::openPaths(const QStringList &paths) {
    if (paths.isEmpty())
        return;
    const auto path = paths.first();
    for (const auto &p : paths)
        if (!QFileInfo(p).isFile() ||
            !QStringList{"apk", "dex", "jar", "war", "zip", "class", "xapk", "apks"}.contains(
                QFileInfo(p).suffix().toLower())) {
            emit failed(tr("不支持或不存在的文件：%1").arg(p));
            return;
        }
    if (busy())
        return;
    if (!QFileInfo(path).isFile()) {
        emit failed(tr("文件不存在：%1").arg(path));
        return;
    }
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!QStringList{"apk", "dex", "jar", "war", "zip", "class", "xapk", "apks"}.contains(ext)) {
        emit failed(tr("请选择 APK、DEX、JAR、WAR、ZIP 或 CLASS 文件。"));
        return;
    }
    auto workspace = std::shared_ptr<QTemporaryDir>(
        new QTemporaryDir(QDir::tempPath() + "/garlic-gui-XXXXXX"),
        [](QTemporaryDir *dir) { QThreadPool::globalInstance()->start([dir] { delete dir; }); });
    if (!workspace->isValid()) {
        emit failed(tr("无法创建临时工作目录。"));
        return;
    }
    cancelSearch();
    if (metadataCanceled_)
        metadataCanceled_->store(true);
    metadataPreparing_ = false;
    directoryOnly_ = paths.size() == 1 &&
        (property("fastOpen").toBool() ||
         (QStringList{"apk", "apks", "xapk", "dex"}.contains(ext) && QFileInfo(path).size() >= 32LL * 1048576));
    metadataReady_ = !directoryOnly_;
    sourcesAfterMetadata_ = false;
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    searchControl_.reset();
    searchWarmControl_.reset();
    sourceGenerating_->store(false);
    sourceGenerating_ = std::make_shared<std::atomic_bool>(false);
    ++projectGeneration_;
    ++searchGeneration_;
    searchPending_ = false;
    cancelPreparing_ = true;
    if (background_.state() != QProcess::NotRunning) {
        background_.kill();
    }
    preparing_ = false;
    sourceGenerating_->store(false);
    fullReady_ = false;
    ownedWorkspaces_ << workspace->path();
    workspace_ = std::move(workspace);
    input_ = QFileInfo(path).absoluteFilePath();
    inputs_.clear();
    for (const auto &p : paths)
        if (!inputs_.contains(QFileInfo(p).absoluteFilePath()))
            inputs_ << QFileInfo(p).absoluteFilePath();
    indexQueue_ = inputs_;
    cache_.clear();
    searchIndex_ = std::make_shared<SearchIndex>();
    project_.clearDocuments();
    cacheOrder_.clear();
    setProperty("errorCount", 0);
    setProperty("warningCount", 0);
    project_.reset(input_);
    project_.setInputs(inputs_);
    nextIndex();
}
void Backend::nextIndex() {
    activeInput_ = indexQueue_.takeFirst();
    jobDir_ = workspace_->path() + "/index";
    QDir().mkpath(jobDir_);
    start(Job::Index, {activeInput_, "-I", workspace_->path() + "/classes.jsonl", "-o", jobDir_,
                       "-t", QString::number(settings_.threads)});
}

void Backend::request(const QString &name, bool smali) {
    if (busy() || !workspace_)
        return;
    if (!safeClassName(name)) {
        emit failed(tr("类名包含无效路径。"));
        return;
    }
    if (smali && !supportsSmali(name)) {
        emit failed(tr("Smali 仅适用于 APK / DEX。"));
        return;
    }
    const QString key = name + (smali ? ":smali" : ":java");
    if (cache_.contains(key)) {
        emit sourceReady(name, smali, cache_.value(key));
        return;
    }
    currentName_ = name;
    argumentClass_ = smali ? name : project_.owner(name);
    const auto available = cachedPath(name, smali);
    if (!available.isEmpty()) {
        emit sourceReady(name, smali, available);
        return;
    }
    smali_ = smali;
    jobDir_ = workspace_->path() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(jobDir_);
    QStringList args{classInput(name),
                     "-c",
                     argumentClass_,
                     "-o",
                     jobDir_,
                     "-t",
                     QString::number(settings_.threads)};
    if (smali)
        args << "-s";
    start(Job::Source, args);
}

void Backend::exportSources(const QString &directory, bool smali) {
    if (busy() || !workspace_)
        return;
    if (smali && !supportsSmali())
        return;
    if (QFileInfo::exists(directory) || !QDir().mkpath(directory)) {
        emit failed(tr("导出目标必须是可创建的新目录：%1").arg(directory));
        return;
    }
    QSet<QString> originalPaths, renamedPaths;
    bool safePaths = false;
    for (const auto &name : project_.classes()) {
        const auto renamed = project_.renamedClass(name);
        const auto originalKey = name.normalized(QString::NormalizationForm_C).toCaseFolded();
        const auto renamedKey = renamed.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (originalPaths.contains(originalKey) || renamedPaths.contains(renamedKey) ||
            (renamed != name && originalKey == renamedKey)) safePaths = true;
        originalPaths.insert(originalKey); renamedPaths.insert(renamedKey);
    }
    if (safePaths) {
        QFile marker(directory + "/.garlic-safe-paths");
        if (!marker.open(QIODevice::WriteOnly)) { emit failed(marker.errorString()); return; }
        marker.write("1\n");
    }
    exportDir_ = directory;
    jobDir_ = directory;
    exportQueue_ = inputs_;
    QStringList args{exportQueue_.takeFirst(), "-o", directory, "-t",
                     QString::number(settings_.threads)};
    if (smali)
        args << "-s";
    start(Job::Export, args);
}

void Backend::cancel() {
    if (metadataCanceled_)
        metadataCanceled_->store(true);
    metadataPreparing_ = false;
    sourcesAfterMetadata_ = false;
    if (indexCanceled_)
        indexCanceled_->store(true);
    if (exportControl_)
        exportControl_->canceled = true;
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    if (postprocessing_) {
        postprocessing_ = false;
        emit busyChanged(false);
        emit log(tr("已取消导出处理，保留部分结果。"));
    }
    cancelSearch();
    ++projectGeneration_;
    if (indexing_) {
        indexing_ = false;
        emit busyChanged(false);
    }
    ++searchGeneration_;
    searchPending_ = false;
    emit preparationChanged(false);
    if (preparing_) {
        cancelPreparing_ = true;
        background_.kill();
    }
    if (!busy())
        return;
    canceled_ = true;
    process_.kill();
}

void Backend::readOutput() {
    QByteArray bytes = process_.readAllStandardOutput() + process_.readAllStandardError();
    bytes.replace('\b', ' ');
    const QString text = QString::fromUtf8(bytes);
    errorTail_ = (errorTail_ + text).right(4000);
    if (job_ == Job::Index) {
        static const QRegularExpression progress("GARLIC_INDEX_PROGRESS (\\d+) (\\d+)");
        auto matches = progress.globalMatch(errorTail_);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int total = match.captured(2).toInt();
            if (total > 0) emit loadProgress(tr("读取 DEX"), match.captured(1).toInt() * 100 / total);
        }
    }
    if (!text.trimmed().isEmpty())
        emit log(text.right(8000));
}

void Backend::finish(int code, QProcess::ExitStatus status) {
    if (!busy())
        return;
    readOutput();
    const auto completed = job_;
    job_ = Job::None;
    if (completed == Job::Index && indexProducer_)
        indexProducer_->store(code == 0 && status == QProcess::NormalExit && !canceled_ ? 1 : -1);
    // Do not leave the window busy while the streaming index reader notices a failed producer.
    // The reader still owns parsing cleanup and ignores the failed partial index.
    if (completed == Job::Index &&
        (canceled_ || code != 0 || status != QProcess::NormalExit))
        indexing_ = false;
    if (completed != Job::Index || !indexing_)
        emit busyChanged(false);
    if (canceled_) {
        emit log(completed == Job::Export ? tr("导出已取消；目标目录保留已完成的部分文件。")
                                          : tr("任务已取消，可重新选择类重试。"));
        return;
    }
    if (code != 0 || status != QProcess::NormalExit) {
        emit failed(
            tr("garlic %1（退出码 %2）。\n%3%4")
                .arg(status == QProcess::CrashExit ? tr("进程崩溃") : tr("执行失败"))
                .arg(code)
                .arg(errorTail_)
                .arg(completed == Job::Export ? tr("\n导出目录可能包含部分结果。") : QString()));
        return;
    }
    if (completed == Job::Index)
        return;
    if (completed == Job::Source) {
        QString path = jobDir_ + "/" + argumentClass_ + (smali_ ? ".smali" : ".java");
        if (QFileInfo(classInput(currentName_)).suffix().compare("class", Qt::CaseInsensitive) == 0)
            path = jobDir_ + "/source.java";
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
            emit failed(
                tr("引擎没有生成该类的%1源码。详情见日志。").arg(smali_ ? " Smali " : " Java "));
            return;
        }
        if (settings_.cacheMode == "memory") {
            if (QFileInfo(path).size() > qint64(settings_.sourceMiB) * 1048576) {
                emit failed(tr("源码超过单文件大小限制，请在设置中提高限制后重试。"));
                const QString directory = jobDir_;
                auto workspace = workspace_;
                QThreadPool::globalInstance()->start(
                    [directory, workspace] { QDir(directory).removeRecursively(); });
                return;
            }
            const QString name = currentName_, key = name + (smali_ ? ":smali" : ":java");
            const bool mode = smali_;
            const QString directory = jobDir_;
            auto snapshot = project_.snapshot();
            auto workspace = workspace_;
            auto watcher = new QFutureWatcher<SourceDocument>(this);
            postprocessing_ = true;
            emit busyChanged(true);
            connect(watcher, &QFutureWatcher<SourceDocument>::finished, this,
                    [this, watcher, name, key, mode] {
                        auto doc = watcher->result();
                        watcher->deleteLater();
                        project_.cacheDocument(key, doc);
                        cache_[key] = "memory:" + key;
                        cacheOrder_.removeAll(key);
                        cacheOrder_.append(key);
                        qint64 bytes = 0;
                        for (const auto &k : cacheOrder_)
                            bytes += project_.documentBytes(k);
                        while (bytes > qint64(settings_.cacheMiB) * 1048576 &&
                               cacheOrder_.size() > 1) {
                            auto old = cacheOrder_.takeFirst();
                            bytes -= project_.documentBytes(old);
                            project_.removeDocument(old);
                            cache_.remove(old);
                        }
                        postprocessing_ = false;
                        emit sourceReady(name, mode, "memory:" + key);
                        emit busyChanged(false);
                    });
            watcher->setFuture(
                QtConcurrent::run([snapshot, name, mode, path, directory, workspace] {
                    auto doc = snapshot->document(name, mode, path, false);
                    QDir(directory).removeRecursively();
                    return doc;
                }));
            return;
        }
        cache_.insert(currentName_ + (smali_ ? ":smali" : ":java"), path);
        cacheOrder_.removeAll(currentName_ + (smali_ ? ":smali" : ":java"));
        cacheOrder_.append(currentName_ + (smali_ ? ":smali" : ":java"));
        qint64 total = 0;
        for (const auto &p : cache_)
            total += QFileInfo(p).size();
        while (total > qint64(settings_.cacheMiB) * 1024 * 1024 && cacheOrder_.size() > 1) {
            const auto key = cacheOrder_.takeFirst();
            const auto old = cache_.take(key);
            total -= QFileInfo(old).size();
            QFile::remove(old);
            QString map = old;
            map.chop(key.endsWith(":smali") ? 6 : 5);
            QFile::remove(map + ".map.json");
        }
        emit sourceReady(currentName_, smali_, path);
    } else {
        if (inputs_.size() > 1 && QFileInfo(process_.arguments().value(0)).suffix() == "class") {
            for (const auto &name : project_.classes())
                if (classInput(name) == process_.arguments().value(0)) {
                    QString dest = Project::sourcePath(exportDir_, name, ".java");
                    QDir().mkpath(QFileInfo(dest).absolutePath());
                    QFile::remove(dest);
                    QFile::rename(exportDir_ + "/source.java", dest);
                    break;
                }
        }
        if (!exportQueue_.isEmpty()) {
            auto args = process_.arguments();
            args[0] = exportQueue_.takeFirst();
            start(Job::Export, args);
            return;
        }
        QDirIterator files(exportDir_, {"*.java", "*.smali"}, QDir::Files,
                           QDirIterator::Subdirectories);
        if (!files.hasNext()) {
            emit failed(tr("引擎没有生成源码。导出目录：%1").arg(exportDir_));
            return;
        }
        if (project_.hasAliases()) {
            postprocessing_ = true;
            emit busyChanged(true);
            exportControl_ = std::make_shared<SearchControl>();
            auto control = exportControl_;
            auto snapshot = project_.snapshot();
            const auto directory = exportDir_, input = input_;
            const int generation = projectGeneration_, limit = settings_.sourceMiB;
            const bool exportSmali = process_.arguments().contains("-s");
            auto watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcher<QString>::finished, this,
                    [this, watcher, generation, directory, control] {
                        auto error = watcher->result();
                        watcher->deleteLater();
                        if (generation != projectGeneration_ || control->canceled)
                            return;
                        postprocessing_ = false;
                        emit busyChanged(false);
                        if (error.isEmpty())
                            emit exported(directory);
                        else
                            emit failed(error);
                    });
            watcher->setFuture(
                QtConcurrent::run([snapshot, directory, input, limit, exportSmali, control] {
                    QSet<QString> done;
                    for (const auto &name : snapshot->classes()) {
                        if (control->canceled)
                            return QString();
                        const QString owner = exportSmali ? name : snapshot->owner(name);
                        if (done.contains(owner))
                            continue;
                        done.insert(owner);
                        const auto suffix = exportSmali ? QString(".smali") : QString(".java");
                        const QString oldPath = snapshot->inputs().size() == 1 && QFileInfo(input).suffix() == "class"
                            ? directory + "/source" + suffix : Project::sourcePath(directory, owner, suffix);
                        if (!QFileInfo::exists(oldPath))
                            continue;
                        if (QFileInfo(oldPath).size() > qint64(limit) * 1024 * 1024) {
                            return QStringLiteral("导出文件超过别名处理限制，已保留原始输出：%1")
                                .arg(oldPath);
                        }
                        const auto document = snapshot->document(owner, exportSmali, oldPath);
                        const auto renamed = snapshot->renamedClass(owner);
                        const QString target = renamed == owner ? oldPath : Project::sourcePath(directory, renamed, suffix);
                        QDir().mkpath(QFileInfo(target).absolutePath());
                        QSaveFile file(target);
                        if (!file.open(QIODevice::WriteOnly) ||
                            file.write(document.text.toUtf8()) < 0 || !file.commit()) {
                            return QStringLiteral("无法保存重命名后的导出文件：%1").arg(target);
                        }
                        if (target != oldPath)
                            QFile::remove(oldPath);
                        QString map = oldPath;
                        map.chop(exportSmali ? 6 : 5);
                        QFile::remove(map + ".map.json");
                    }
                    return QString();
                }));
            return;
        }
        emit exported(exportDir_);
    }
}

void Backend::applyEnvironment(QProcess &process, const QString &directory) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("GARLIC_DIRECTORY_INDEX");
    env.remove("GARLIC_DEX_ENTRY");
    env.remove("GARLIC_MAP_PACK");
    env.remove("GARLIC_JAVA_PACK");
    env.insert("GARLIC_SOURCE_MAP_DIR", directory);
    if (workspace_)
        env.insert("GARLIC_APK_CACHE_DIR", workspace_->path() + "/apk-cache");
    env.insert("GARLIC_SAFE_SOURCE_PATHS", QFileInfo::exists(directory + "/.garlic-safe-paths") ? "1" : "0");
    env.insert("GARLIC_ESCAPE_UNICODE", settings_.escapeUnicode ? "1" : "0");
    env.insert("GARLIC_SIMPLIFY_CONTROL_FLOW", settings_.simplifyControlFlow ? "1" : "0");
    env.insert("GARLIC_UNFLATTEN", settings_.unflatten ? "1" : "0");
    env.insert("GARLIC_DEOBFUSCATE", settings_.deobfuscate ? "1" : "0");
    env.insert("GARLIC_EXCLUDED_PACKAGES", settings_.excluded.join(';').replace('.', '/'));
    process.setProcessEnvironment(env);
}
void Backend::configure(const AppSettings &settings) {
    const bool engineChange = settings_.escapeUnicode != settings.escapeUnicode ||
                              settings_.excluded != settings.excluded ||
                              settings_.simplifyControlFlow != settings.simplifyControlFlow ||
                              settings_.unflatten != settings.unflatten ||
                              settings_.deobfuscate != settings.deobfuscate ||
                              settings_.cacheMode != settings.cacheMode;
    settings_ = settings;
    if (engineChange && !busy() && !preparing_ && !metadataPreparing_)
        clearCache();
}
QString Backend::cachedPath(const QString &name, bool smali) const {
    const auto direct = cache_.value(name + (smali ? ":smali" : ":java"));
    if (direct.startsWith("memory:") || QFileInfo::exists(direct))
        return direct;
    if (fullReady_ && !smali && workspace_) {
        const auto directory = workspace_->path() + "/all-java";
        const auto path = inputs_.size() == 1 && QFileInfo(input_).suffix() == "class"
            ? directory + "/source.java" : Project::sourcePath(directory, project_.owner(name), ".java");
        if (QFileInfo::exists(path) || QFileInfo::exists(directory + "/java-sources.bin"))
            return path;
    }
    return {};
}
QJsonObject Backend::cacheStats() const {
    qint64 bytes = 0;
    for (auto i = cache_.cbegin(); i != cache_.cend(); ++i)
        bytes += i.value().startsWith("memory:") ? project_.documentBytes(i.key())
                                                 : QFileInfo(i.value()).size();
    return {{"cached_documents", cache_.size()},
            {"source_bytes", double(bytes)},
            {"mode", settings_.cacheMode},
            {"full_project_ready", fullReady_},
            {"preparing", preparing_}};
}
void Backend::clearCache() {
    if (busy() || preparing_ || metadataPreparing_) {
        emit log(tr("请先停止正在运行的任务，再清理源码缓存。"));
        return;
    }
    ++searchGeneration_;
    cancelSearch();
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    cache_.clear();
    searchIndex_ = std::make_shared<SearchIndex>();
    project_.clearDocuments();
    cacheOrder_.clear();
    fullReady_ = false;
    emit cacheCleared();
    if (!workspace_)
        return;
    clearing_ = true;
    emit busyChanged(true);
    auto workspace = workspace_;
    auto task = new QFutureWatcher<QStringList>(this);
    connect(task, &QFutureWatcher<QStringList>::finished, this, [this, task] {
        const auto failures = task->result();
        task->deleteLater();
        clearing_ = false;
        emit busyChanged(false);
        if (failures.isEmpty())
            emit log(tr("源码缓存已清理；类索引保留，已打开标签重新选择时按需加载。"));
        else
            emit failed(tr("部分缓存删除失败（可能被占用）：\n%1").arg(failures.join('\n')));
    });
    task->setFuture(QtConcurrent::run([workspace] {
        QStringList failures;
        QDir dir(workspace->path());
        for (const auto &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (name != "index" && !QDir(dir.filePath(name)).removeRecursively())
                failures << dir.filePath(name);
        return failures;
    }));
}
void Backend::prepareSources() {
    if (!metadataReady_) {
        sourcesAfterMetadata_ = true;
        prepareMetadata();
        return;
    }
    if (clearing_ || preparing_ || fullReady_ || !workspace_ || project_.classCount() == 0)
        return;
    if (background_.state() != QProcess::NotRunning) {
        QTimer::singleShot(20, this, &Backend::prepareSources);
        return;
    }
    const QString directory = workspace_->path() + "/all-java";
    if (QDir(directory).exists()) {
        const auto previous = directory + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().rename(directory, previous)) {
            emit failed(tr("无法重建源码目录，请重新打开项目。"));
            return;
        }
        auto workspace = workspace_;
        QThreadPool::globalInstance()->start(
            [workspace, previous] { QDir(previous).removeRecursively(); });
    }
    QDir().mkpath(directory);
    QFile marker(directory + "/.garlic-safe-paths");
    if (!marker.open(QIODevice::WriteOnly)) {
        emit failed(tr("无法创建源码缓存标记：%1").arg(marker.errorString()));
        return;
    }
    marker.write("1\n"); marker.close();
    cancelPreparing_ = false;
    preparing_ = true;
    emit loadProgress(tr("源码生成"), 0);
    sourceGenerating_->store(true);
    emit preparationChanged(true);
    applyEnvironment(background_, directory);
    auto environment = background_.processEnvironment();
    environment.insert("GARLIC_MAP_PACK", "1");
    background_.setProcessEnvironment(environment);
    background_.setWorkingDirectory(workspace_->path());
    background_.setStandardOutputFile(
        QFileInfo(input_).suffix() == "class" ? directory + "/source.java" : QString());
    backgroundQueue_ = inputs_;
    nextBackground();
    if (!searchControl_)
        warmSearchIndex();
}
void Backend::nextBackground() {
    backgroundErrorTail_.clear();
    const auto input = backgroundQueue_.takeFirst();
    const auto directory = workspace_->path() + "/all-java";
    background_.setStandardOutputFile(
        QFileInfo(input).suffix() == "class" ? directory + "/source.java" : QString());
    auto environment = background_.processEnvironment();
    const auto suffix = QFileInfo(input).suffix().toLower();
    if (QStringList{"apk", "apks", "xapk", "dex"}.contains(suffix))
        environment.insert("GARLIC_JAVA_PACK", "1");
    else environment.remove("GARLIC_JAVA_PACK");
    background_.setProcessEnvironment(environment);
    background_.start(engine_, {input, "-o", directory, "-t", QString::number(settings_.threads)});
}
void Backend::prepareFinished(int code, QProcess::ExitStatus status) {
    if (!preparing_)
        return;
    if (code == 0 && inputs_.size() > 1 &&
        QFileInfo(background_.arguments().value(0)).suffix() == "class") {
        const auto input = background_.arguments().value(0);
        for (const auto &name : project_.classes())
            if (classInput(name) == input) {
                QString dest = Project::sourcePath(workspace_->path() + "/all-java", name, ".java");
                QDir().mkpath(QFileInfo(dest).absolutePath());
                QFile::remove(dest);
                QFile::rename(workspace_->path() + "/all-java/source.java", dest);
                break;
            }
    }
    if (code == 0 && status == QProcess::NormalExit && !cancelPreparing_ &&
        !backgroundQueue_.isEmpty()) {
        nextBackground();
        return;
    }
    preparing_ = false;
    sourceGenerating_->store(false);
    emit preparationChanged(false);
    if (cancelPreparing_) {
        searchPending_ = false;
        emit log(tr("已停止后台反编译，重新搜索可重试。"));
        return;
    }
    if (code != 0 || status != QProcess::NormalExit) {
        searchPending_ = false;
        const auto details = (backgroundErrorTail_ +
                              QString::fromUtf8(background_.readAllStandardError())).right(4000).trimmed();
        const auto reason = status == QProcess::CrashExit
            ? tr("引擎异常退出（代码 %1）").arg(code)
            : tr("引擎退出码 %1").arg(code);
        emit log(tr("后台反编译失败：%1\n引擎：%2\n输入：%3\n%4")
                     .arg(reason, engine_, background_.arguments().value(0),
                          details.isEmpty() ? background_.errorString() : details));
        emit failed(tr("后台源码生成失败：%1。已生成的源码仍可搜索，重新搜索可重试。%2")
                        .arg(reason, details.isEmpty() ? QString() : "\n" + details));
        return;
    }
    fullReady_ = true;
    emit projectSourcesReady();
    if ((!searchControl_ || searchControl_->canceled) &&
        (!searchWarmControl_ || searchWarmControl_->canceled))
        warmSearchIndex();
}

void Backend::warmSearchIndex() {
    if (!workspace_ || project_.classCount() == 0)
        return;
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    searchWarmControl_ = std::make_shared<SearchControl>();
    const auto control = searchWarmControl_;
    const auto snapshot = project_.snapshot();
    const auto index = searchIndex_;
    const auto directory = workspace_->path() + "/all-java";
    const bool single = inputs_.size() == 1 && QFileInfo(input_).suffix() == "class";
    SearchOptions options;
    options.query = "__garlic_background_search_index_probe__";
    options.code = options.comments = true;
    options.indexOnly = true;
    options.limit = 1;
    options.sourceMiB = settings_.sourceMiB;
    auto generating = sourceGenerating_;
    auto events = std::make_shared<SearchEvents>();
    auto watcher = new QFutureWatcher<SearchResult>(this);
    connect(watcher, &QFutureWatcher<SearchResult>::finished, this, [this, watcher, index, control] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (searchWarmControl_ == control)
            searchWarmControl_.reset();
        if (index != searchIndex_ || control->canceled || result.canceled || !result.error.isEmpty())
            return;
        if (fullReady_) {
            index->ready = true;
            emit searchIndexReady();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [snapshot, options, directory, single, control, generating, events, index] {
            snapshot->overrideAnnotations();
            return searchProject(snapshot, options, directory, single, generating, control, events, 0,
                          index, true);
        }));
}

void Backend::cancelSearch(int request) {
    if (request >= 0 && request != searchGeneration_)
        return;
    if (searchControl_)
        searchControl_->canceled = true;
}
void Backend::search(const QString &query, bool regex, bool caseSensitive) {
    SearchOptions options;
    options.query = query;
    options.regex = regex;
    options.caseSensitive = caseSensitive;
    const int request = search(options);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &Backend::searchCompleted, this,
                          [this, request, connection](int id, const SearchResult &result) {
                              if (id != request)
                                  return;
                              disconnect(*connection);
                              if (!result.error.isEmpty())
                                  emit failed(result.error);
                              else if (!result.canceled)
                                  emit searchFinished(result.hits, result.truncated);
                          });
}
int Backend::search(const SearchOptions &options) {
    cancelSearch();
    if (searchWarmControl_)
        searchWarmControl_->canceled = true;
    const int request = ++searchGeneration_;
    searchControl_ = std::make_shared<SearchControl>();
    const auto control = searchControl_;
    if (!metadataReady_ && (options.classes || options.methods || options.fields || options.code || options.comments)) {
        prepareMetadata();
        const auto workspace = workspace_;
        auto timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer, options, request, control, workspace] {
            if (control->canceled || workspace != workspace_) {
                timer->deleteLater();
                SearchResult result; result.canceled = true;
                emit searchCompleted(request, result);
            } else if (metadataReady_) {
                timer->deleteLater();
                runSearch(options, request, control);
            } else if (!metadataPreparing_) {
                timer->deleteLater();
                SearchResult result; result.error = tr("成员与引用索引准备未完成，可重试。");
                emit searchCompleted(request, result);
            }
        });
        timer->start(50);
    } else runSearch(options, request, control);
    return request;
}
void Backend::runSearch(const SearchOptions &options, int request,
                        const std::shared_ptr<SearchControl> &control) {
    if ((options.code || options.comments) && !fullReady_ && !options.query.isEmpty() &&
        searchExpression(options).isValid())
        prepareSources();
    auto snapshot = project_.snapshot();
    auto index = searchIndex_;
    auto generating = sourceGenerating_;
    auto workspace = workspace_;
    auto events = std::make_shared<SearchEvents>();
    connect(events.get(), &SearchEvents::batch, this, &Backend::searchBatch);
    connect(events.get(), &SearchEvents::progress, this, &Backend::searchProgress);
    auto watcher = new QFutureWatcher<SearchResult>(this);
    connect(watcher, &QFutureWatcher<SearchResult>::finished, this, [this, watcher, request] {
        auto result = watcher->result();
        watcher->deleteLater();
        emit searchCompleted(request, result);
        if (request == searchGeneration_) {
            searchControl_.reset();
            if (fullReady_ && !searchIndex_->ready &&
                (!searchWarmControl_ || searchWarmControl_->canceled))
                warmSearchIndex();
        }
    });
    auto settings = options;
    settings.sourceMiB = settings_.sourceMiB;
    const bool single = inputs_.size() == 1 && QFileInfo(input_).suffix() == "class";
    watcher->setFuture(QtConcurrent::run(
        [snapshot, settings, control, generating, workspace, single, events, request, index] {
            return searchProject(snapshot, settings,
                                 workspace ? workspace->path() + "/all-java" : QString(), single,
                                 generating, control, events, request, index, true);
        }));
}

void Backend::prepareMetadata() {
    if (metadataReady_ || metadataPreparing_ || !workspace_ || indexing_)
        return;
    metadataPreparing_ = true;
    metadataCanceled_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = metadataCanceled_;
    metadataEngines_.removeIf([](const auto &flag) { return !flag->load(); });
    const auto engineRunning = std::make_shared<std::atomic_bool>(true);
    metadataEngines_.append(engineRunning);
    const auto workspace = workspace_;
    const auto input = input_;
    const auto engine = engine_;
    const auto settings = settings_;
    const int generation = projectGeneration_;
    auto snapshot = project_.snapshot();
    snapshot->setCancellationToken(canceled);
    const int totalClasses = snapshot->classCount();
    QPointer<Backend> self(this);
    emit loadProgress(tr("成员与引用"), 0);
    QProcess environmentSource;
    applyEnvironment(environmentSource, workspace->path() + "/index");
    auto environment = environmentSource.processEnvironment();
    environment.insert("GARLIC_COMPACT_INDEX", "2");
    emit preparationChanged(true);
    emit log(tr("类目录已打开，正在后台准备成员与引用索引…"));
    using Result = QPair<std::shared_ptr<Project>, QString>;
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this,
        [this, watcher, generation, canceled] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (generation != projectGeneration_ || canceled->load())
                return;
            metadataPreparing_ = false;
            emit preparationChanged(false);
            if (!result.second.isEmpty()) {
                emit failed(tr("成员与引用索引准备失败：%1；可重新查询以重试。").arg(result.second));
                return;
            }
            project_.replaceData(*result.first, true);
            metadataReady_ = true;
            emit metadataCompleted();
            emit log(tr("成员与引用索引已就绪。"));
            if (settings_.background || sourcesAfterMetadata_)
                prepareSources();
        });
    watcher->setFuture(QtConcurrent::run([snapshot, workspace, input, engine, settings,
                                         environment, canceled, self, generation, totalClasses, engineRunning]() -> Result {
        struct EngineStopped { std::shared_ptr<std::atomic_bool> flag; ~EngineStopped() { flag->store(false); } } stopped{engineRunning};
        if (canceled->load()) return {{}, QStringLiteral("canceled")};
        // A canceled process may still be exiting when the user retries. Never
        // reuse its output files, even within the same project workspace.
        QTemporaryDir attempt(workspace->path() + "/index/details-XXXXXX");
        if (!attempt.isValid()) return {{}, QStringLiteral("Cannot create metadata workspace")};
        const auto path = attempt.path() + "/metadata.jsonl";
        const auto errorPath = attempt.path() + "/metadata.stderr";
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(workspace->path());
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(errorPath);
        process.start(engine, {input, "-I", path, "-o", attempt.path(),
                               "-t", QString::number(settings.threads)});
        if (!process.waitForStarted())
            return {{}, process.errorString()};
        QFile file(path);
        QByteArray pending;
        int count = 0, lastPercent = -1;
        QElapsedTimer timing; timing.start();
        auto progress = [&](const QString &phase, int percent) {
            if (self) QMetaObject::invokeMethod(self, [self, generation, phase, percent] {
                if (self && self->projectGeneration_ == generation)
                    emit self->loadProgress(phase, percent);
            }, Qt::QueuedConnection);
        };
        while (true) {
            if (canceled->load()) {
                process.kill(); process.waitForFinished(); engineRunning->store(false);
                return {{}, QStringLiteral("canceled")};
            }
            if (!file.isOpen())
                file.open(QIODevice::ReadOnly);
            if (!file.isOpen() || file.atEnd()) {
                process.waitForFinished(5);
                // A small input may create and finish its index during the wait.
                if (!file.isOpen()) file.open(QIODevice::ReadOnly);
                if (process.state() != QProcess::NotRunning)
                    continue;
                if (file.isOpen() && !file.atEnd())
                    continue;
                if (pending.isEmpty())
                    break;
            } else {
                pending += file.readLine();
                if (!pending.endsWith('\n'))
                    continue;
            }
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(pending, &error);
            pending.clear();
            if (error.error != QJsonParseError::NoError || !doc.isObject() ||
                !Backend::safeClassName(doc.object().value("name").toString())) {
                process.kill(); process.waitForFinished();
                return {{}, QStringLiteral("Invalid metadata index")};
            }
            auto entry = doc.object();
            entry["input"] = input;
            if (!entry.contains("origin")) entry["origin"] = QFileInfo(input).fileName();
            snapshot->addClass(entry);
            ++count;
            int percent = qMin(99, count * 100 / qMax(1, totalClasses));
            if (percent != lastPercent) {
                lastPercent = percent; progress(QObject::tr("成员与引用"), percent);
            }
            // Pump process state without imposing one millisecond per 256 classes.
            if (count % 1024 == 0) {
                process.waitForFinished(0);
                if (process.state() == QProcess::NotRunning) engineRunning->store(false);
            }
        }
        engineRunning->store(false);
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !count) {
            QFile errors(errorPath); errors.open(QIODevice::ReadOnly);
            return {{}, QString("Engine exit %1: %2").arg(process.exitCode())
                .arg(QString::fromUtf8(errors.readAll().right(4000)))};
        }
        if (canceled->load()) return {{}, QStringLiteral("canceled")};
        const auto parsedMs = timing.elapsed();
        progress(QObject::tr("构建查询索引"), 0);
        if (settings.deobfuscate) snapshot->deobfuscateNames();
        const auto aliasesMs = timing.elapsed();
        progress(QObject::tr("构建查询索引"), 33);
        // Both indexes read immutable metadata and own separate synchronization.
        auto references = std::async(std::launch::async, [snapshot] { snapshot->xrefs(QString()); });
        snapshot->symbols();
        snapshot->aliasVersion();
        progress(QObject::tr("构建查询索引"), 66);
        references.get();
        if (canceled->load()) return {{}, QStringLiteral("canceled")};
        if (qEnvironmentVariableIsSet("GARLIC_PROFILE_LOAD"))
            qInfo() << "Metadata parse / aliases / queries ms:" << parsedMs << aliasesMs - parsedMs << timing.elapsed() - aliasesMs;
        progress(QObject::tr("构建查询索引"), 100);
        return {snapshot, {}};
    }));
}

void Backend::readIndex() {

    indexing_ = true;
    emit busyChanged(true);
    const int generation = projectGeneration_;
    const auto workspace = workspace_;
    const auto input = activeInput_;
    const auto producer = indexProducer_;
    using IndexResult = QPair<std::shared_ptr<Project>, QString>;
    auto watcher = new QFutureWatcher<IndexResult>(this);
    connect(watcher, &QFutureWatcher<IndexResult>::finished, this,
            [this, watcher, generation, producer] {
                auto result = watcher->result();
                watcher->deleteLater();
                if (generation != projectGeneration_)
                    return;
                indexing_ = false;
                if (producer->load() < 0) {
                    emit busyChanged(false);
                    return;
                }
                if (!result.second.isEmpty()) {
                    // The parser can reject a row before the producer exits.
                    // Retire the job first so failed observers can safely reopen.
                    job_ = Job::None;
                    producer->store(-1);
                    process_.kill();
                    if (process_.state() != QProcess::NotRunning)
                        process_.waitForFinished(3000);
                    emit busyChanged(false);
                    emit failed(result.second);
                    return;
                }
                project_.replaceData(*result.first);
                if (!indexQueue_.isEmpty()) {
                    nextIndex();
                    return;
                }
                emit indexed(project_.classes());
                if (generation != projectGeneration_)
                    return;
                emit busyChanged(false);
                if (directoryOnly_)
                    QTimer::singleShot(0, this, &Backend::prepareMetadata);
                else if (settings_.background)
                    QTimer::singleShot(0, this, &Backend::prepareSources);
            });
    auto project = project_.snapshot();
    indexCanceled_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = indexCanceled_;
    const bool directoryOnly = directoryOnly_;
    const bool deobfuscate = settings_.deobfuscate && !directoryOnly_;
    const bool finalInput = indexQueue_.isEmpty() && !directoryOnly_;
    watcher->setFuture(QtConcurrent::run([workspace, input, project, canceled, producer, deobfuscate, finalInput, directoryOnly] {
        QElapsedTimer timings;
        timings.start();
        QFile file(workspace->path() + "/classes.jsonl");
        while (!file.open(QIODevice::ReadOnly)) {
            if (canceled->load() || producer->load() != 0)
                return IndexResult{{}, QObject::tr("无法打开类索引：%1").arg(file.errorString())};
            QThread::msleep(5);
        }
        qint64 lineNumber = 0;
        QByteArray pending;
        while (true) {
            if (canceled->load())
                return IndexResult{{}, QObject::tr("索引读取已取消。")};
            if (file.atEnd()) {
                if (producer->load() == 0) {
                    QThread::msleep(5);
                    continue;
                }
                if (pending.isEmpty())
                    break;
            } else
                pending += file.readLine();
            if (!pending.endsWith('\n') && producer->load() == 0)
                continue;
            auto line = std::move(pending);
            pending.clear();
            ++lineNumber;
            if (file.error() != QFileDevice::NoError)
                return IndexResult{{},
                                   QObject::tr("类索引读取失败（第 %1 行）：%2")
                                       .arg(lineNumber)
                                       .arg(file.errorString())};
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return IndexResult{{},
                                   QObject::tr("类索引格式错误（第 %1 行）：%2")
                                       .arg(lineNumber)
                                       .arg(error.errorString())};
            auto object = document.object();
            if (!Backend::safeClassName(object.value("name").toString()))
                return IndexResult{{}, QStringLiteral("引擎返回了无效的类索引。")};
            object["input"] = input;
            if (!object.contains("origin"))
                object["origin"] = QFileInfo(input).fileName();
            if (directoryOnly) project->addDirectoryClass(object);
            else project->addClass(object);
        }
        if (project->classCount() == 0)
            return IndexResult{{}, QStringLiteral("无类被加载，没有什么可以反编译。")};
        const auto parseMs = timings.restart();
        if (deobfuscate)
            project->deobfuscateNames();
        // Publish a queryable project: the first X query must not wait behind a
        // whole-project warm-up lock after the UI has announced indexing complete.
        if (finalInput && !canceled->load()) {
            project->xrefs(QString());
            const auto refsMs = timings.restart();
            project->symbols();
            if (qEnvironmentVariableIsSet("GARLIC_PROFILE_LOAD"))
                qInfo() << "Index stream / references / symbols ms:" << parseMs << refsMs << timings.elapsed();
        }
        return IndexResult{project, {}};
    }));
}
