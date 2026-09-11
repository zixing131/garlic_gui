#pragma once
#include "project.h"
#include "search.h"
#include "settings.h"
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>

class Backend : public QObject {
    Q_OBJECT
  public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;
    Project *project() { return &project_; }
    const Project *project() const { return &project_; }
    AppSettings settings() const { return settings_; }
    void configure(const AppSettings &settings);
    QHash<QString, QString> cachedSources() const { return cache_; }
    QString cachedPath(const QString &name, bool smali) const;
    void prepareSources();
    bool preparing() const { return preparing_; }
    bool projectReady() const { return fullReady_; }
    bool searchReady() const { return searchIndex_->ready.load(); }
    bool metadataReady() const { return metadataReady_; }
    bool metadataPreparing() const { return metadataPreparing_; }
    void prepareMetadata();
    void search(const QString &query, bool regex = false, bool caseSensitive = false);
    int search(const SearchOptions &options);
    void cancelSearch(int request = -1);
    void clearCache();
    QJsonObject cacheStats() const;
    void setEngine(const QString &path) { engine_ = path; }
    QString engine() const { return engine_; }
    QList<qint64> workerPids() const { QList<qint64> result; for (const auto *p : {&process_, &background_}) if (p->processId()) result << p->processId(); return result; }
    QString input() const { return input_; }
    QStringList inputs() const { return inputs_; }
    QString classInput(const QString &name) const;
    QString workspacePath() const { return workspace_ ? workspace_->path() : QString(); }
    bool busy() const { return job_ != Job::None || indexing_ || postprocessing_ || clearing_; }
    bool supportsSmali(const QString &name = {}) const;
    void open(const QString &path);
    void openPaths(const QStringList &paths);
    void request(const QString &name, bool smali);
    void exportSources(const QString &directory, bool smali);
    void cancel();
    static bool safeClassName(const QString &name);
  signals:
    void searchBatch(int request, const QJsonArray &hits);
    void searchProgress(int request, int scanned, int total);
    void searchCompleted(int request, const SearchResult &result);
    void projectSourcesReady();
    void searchIndexReady();
    void cacheCleared();
    void preparationChanged(bool active);
    void searchFinished(const QJsonArray &results, bool truncated);
    void indexed(const QStringList &classes);
    void metadataCompleted();
    void sourceReady(const QString &name, bool smali, const QString &path);
    void busyChanged(bool busy);
    void log(const QString &text);
    void failed(const QString &message);
    void exported(const QString &directory);

  private:
    enum class Job { None, Index, Source, Export };
    void start(Job job, const QStringList &arguments);
    void finish(int code, QProcess::ExitStatus status);
    void readIndex();
    std::shared_ptr<std::atomic_int> indexProducer_;
    void readOutput();
    void prepareFinished(int code, QProcess::ExitStatus status);
    void warmSearchIndex();
    void runSearch(const SearchOptions &options, int request,
                   const std::shared_ptr<SearchControl> &control);
    std::shared_ptr<SearchControl> searchControl_;
    std::shared_ptr<SearchControl> searchWarmControl_;
    std::shared_ptr<SearchIndex> searchIndex_ = std::make_shared<SearchIndex>();
    std::shared_ptr<std::atomic_bool> sourceGenerating_ = std::make_shared<std::atomic_bool>(false);
    void applyEnvironment(QProcess &process, const QString &directory);
    QString argumentClass_, activeInput_;
    QStringList inputs_, indexQueue_, exportQueue_, backgroundQueue_;
    void nextIndex();
    void nextBackground();
    Project project_;
    AppSettings settings_;
    QProcess background_;
    QString backgroundErrorTail_;
    bool preparing_ = false, fullReady_ = false, cancelPreparing_ = false;
    QString query_;
    bool searchPending_ = false, regex_ = false, caseSensitive_ = false;
    int searchGeneration_ = 0;
    QStringList cacheOrder_;
    QProcess process_;
    QString engine_, input_, currentName_, jobDir_, exportDir_;
    QString errorTail_;
    std::shared_ptr<QTemporaryDir> workspace_;
    std::shared_ptr<std::atomic_bool> indexCanceled_;
    bool indexing_ = false, postprocessing_ = false, clearing_ = false;
    bool directoryOnly_ = false, metadataReady_ = true, metadataPreparing_ = false;
    bool sourcesAfterMetadata_ = false;
    std::shared_ptr<std::atomic_bool> metadataCanceled_;
    std::shared_ptr<SearchControl> exportControl_;
    int projectGeneration_ = 0;
    QHash<QString, QString> cache_;
    Job job_ = Job::None;
    bool smali_ = false;
    bool canceled_ = false;
};
