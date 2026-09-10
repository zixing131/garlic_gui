#include "backend.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>

Backend::Backend(QObject *parent)
    : QObject(parent), project_(this), settings_(AppSettings::load()) {
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
        emit log(QString::fromUtf8(background_.readAllStandardError()).right(4000));
    });
    connect(&background_, &QProcess::readyReadStandardOutput, this,
            [this] { background_.readAllStandardOutput(); });
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
    if (job == Job::Index) {
        auto environment = process_.processEnvironment();
        environment.insert("GARLIC_COMPACT_INDEX", "1");
        process_.setProcessEnvironment(environment);
    }
    process_.setStandardOutputFile(
        QFileInfo(arguments.value(0)).suffix().compare("class", Qt::CaseInsensitive) == 0 &&
                job != Job::Index
            ? jobDir_ + "/source.java"
            : QString());
    emit busyChanged(true);
    if (job == Job::Index) {
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
                    QString dest = exportDir_ + "/" + name + ".java";
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
        if (!project_.aliases().isEmpty()) {
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
                        const QString oldPath =
                            directory + "/" +
                            (snapshot->inputs().size() == 1 && QFileInfo(input).suffix() == "class"
                                 ? QString("source")
                                 : owner) +
                            (exportSmali ? ".smali" : ".java");
                        if (!QFileInfo::exists(oldPath))
                            continue;
                        if (QFileInfo(oldPath).size() > qint64(limit) * 1024 * 1024) {
                            return QStringLiteral("导出文件超过别名处理限制，已保留原始输出：%1")
                                .arg(oldPath);
                        }
                        const auto document = snapshot->document(owner, exportSmali, oldPath);
                        const auto renamed = snapshot->renamedClass(owner);
                        const QString target = renamed == owner
                                                   ? oldPath
                                                   : QFileInfo(oldPath).absolutePath() + "/" +
                                                         renamed.section('/', -1) +
                                                         (exportSmali ? ".smali" : ".java");
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
    env.insert("GARLIC_SOURCE_MAP_DIR", directory);
    env.insert("GARLIC_ESCAPE_UNICODE", settings_.escapeUnicode ? "1" : "0");
    env.insert("GARLIC_SIMPLIFY_CONTROL_FLOW", settings_.simplifyControlFlow ? "1" : "0");
    env.insert("GARLIC_EXCLUDED_PACKAGES", settings_.excluded.join(';').replace('.', '/'));
    process.setProcessEnvironment(env);
}
void Backend::configure(const AppSettings &settings) {
    const bool engineChange = settings_.escapeUnicode != settings.escapeUnicode ||
                              settings_.excluded != settings.excluded ||
                              settings_.simplifyControlFlow != settings.simplifyControlFlow ||
                              settings_.cacheMode != settings.cacheMode;
    settings_ = settings;
    if (engineChange && !busy() && !preparing_)
        clearCache();
}
QString Backend::cachedPath(const QString &name, bool smali) const {
    const auto direct = cache_.value(name + (smali ? ":smali" : ":java"));
    if (direct.startsWith("memory:") || QFileInfo::exists(direct))
        return direct;
    if (fullReady_ && !smali && workspace_) {
        const auto path =
            workspace_->path() + "/all-java/" +
            (inputs_.size() == 1 && QFileInfo(input_).suffix() == "class" ? QString("source")
                                                                          : project_.owner(name)) +
            ".java";
        if (QFileInfo::exists(path))
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
    if (busy() || preparing_) {
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
    if (clearing_ || preparing_ || fullReady_ || !workspace_ || project_.classes().isEmpty())
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
    cancelPreparing_ = false;
    preparing_ = true;
    sourceGenerating_->store(true);
    emit preparationChanged(true);
    applyEnvironment(background_, directory);
    background_.setWorkingDirectory(workspace_->path());
    background_.setStandardOutputFile(
        QFileInfo(input_).suffix() == "class" ? directory + "/source.java" : QString());
    backgroundQueue_ = inputs_;
    nextBackground();
    if (!searchControl_)
        warmSearchIndex();
}
void Backend::nextBackground() {
    const auto input = backgroundQueue_.takeFirst();
    const auto directory = workspace_->path() + "/all-java";
    background_.setStandardOutputFile(
        QFileInfo(input).suffix() == "class" ? directory + "/source.java" : QString());
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
                QString dest = workspace_->path() + "/all-java/" + name + ".java";
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
        emit failed(tr("后台源码生成失败；可停止后重新搜索，已打开的代码仍可浏览。"));
        return;
    }
    fullReady_ = true;
    emit projectSourcesReady();
    if ((!searchControl_ || searchControl_->canceled) &&
        (!searchWarmControl_ || searchWarmControl_->canceled))
        warmSearchIndex();
}

void Backend::warmSearchIndex() {
    if (!workspace_ || project_.classes().isEmpty())
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
    options.limit = 1;
    options.sourceMiB = settings_.sourceMiB;
    auto generating = sourceGenerating_;
    auto events = std::make_shared<SearchEvents>();
    QThreadPool::globalInstance()->start(
        [snapshot, options, directory, single, control, generating, events, index] {
            searchProject(snapshot, options, directory, single, generating, control, events, 0,
                          index, true);
        });
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
    if ((options.code || options.comments) && !fullReady_ && !options.query.isEmpty() &&
        searchExpression(options).isValid())
        prepareSources();
    auto snapshot = project_.snapshot();
    auto control = searchControl_;
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
        if (request == searchGeneration_)
            searchControl_.reset();
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
    return request;
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
                    emit busyChanged(false);
                    emit failed(result.second);
                    return;
                }
                project_.replaceData(*result.first);
                if (!indexQueue_.isEmpty()) {
                    nextIndex();
                    return;
                }
                const auto referenceSnapshot = project_.snapshot();
                QThreadPool::globalInstance()->start([referenceSnapshot] {
                    referenceSnapshot->xrefs(QString());
                    referenceSnapshot->symbols();
                    referenceSnapshot->overrideAnnotations();
                });
                emit indexed(project_.classes());
                emit busyChanged(false);
                if (settings_.background)
                    QTimer::singleShot(0, this, &Backend::prepareSources);
            });
    auto project = project_.snapshot();
    indexCanceled_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = indexCanceled_;
    const bool deobfuscate = settings_.deobfuscate;
    watcher->setFuture(QtConcurrent::run([workspace, input, project, canceled, producer, deobfuscate] {
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
            project->addClass(object);
        }
        if (project->classes().isEmpty())
            return IndexResult{{}, QStringLiteral("无类被加载，没有什么可以反编译。")};
        if (deobfuscate)
            project->deobfuscateNames();
        return IndexResult{project, {}};
    }));
}
