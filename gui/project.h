#pragma once
#include <QIODevice>
#include "projectmap.h"
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <memory>
#include <mutex>
#include <atomic>

struct SourceSpan {
    int start = 0, end = 0;
    QString id;
    bool declaration = false;
};
struct SourceDocument {
    QString text;
    QVector<SourceSpan> spans;
};

class Project : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    std::shared_ptr<Project> snapshot() const;
    void cancelPendingWork() { canceled_->store(true); }
    std::shared_ptr<std::atomic_bool> cancellationToken() const { return canceled_; }
    void setCancellationToken(std::shared_ptr<std::atomic_bool> token) { canceled_ = std::move(token); }
    void replaceData(const Project &other, bool keepDocuments = false);
    void reset(const QString &input);
    bool writeIndexSnapshot(QIODevice *device) const;
    static std::shared_ptr<Project> readIndexSnapshot(QIODevice *device, const std::shared_ptr<std::atomic_bool> &canceled);
    void addClass(const QJsonObject &entry);
    // A fresh directory-only project defers symbol maps until full metadata.
    void addDirectoryClass(const QJsonObject &entry) { classes_.insert(entry.value("name").toString(), entry); }
    QStringList classes() const;
    int classCount() const { return classes_.size(); }
    QJsonObject info(const QString &name) const { return classes_.value(normalize(name)); }
    QString owner(const QString &name) const;
    QJsonArray members(const QString &name, bool methods) const;
    QJsonArray xrefs(const QString &id) const;
    QJsonArray callees(const QString &id) const;
    QJsonArray symbols(const QString &query = {}) const;
    QString displayName(const QString &name) const;
    QString renamedClass(const QString &name) const;
    QString symbolName(const QString &id) const;
    QJsonObject symbolInfo(const QString &id) const { return symbols_.value(id); }
    QString alias(const QString &id) const { return aliases_.value(id); }
    QString rename(const QString &id, const QString &newName);
    void deobfuscateNames();
    void undoRename();
    bool canUndo() const { return !undo_.isEmpty(); }
    QJsonObject aliases() const;
    QHash<QString, QString> aliasMap() const { return aliases_; }
    bool hasAliases() const { return !aliases_.isEmpty(); }
    QString aliasVersion() const;
    bool save(const QString &path, QString *error = nullptr) const;
    bool loadAliases(const QString &path, QString *error = nullptr);
    SourceDocument document(const QString &name, bool smali, const QString &sourcePath,
                            bool applyAliases = true) const;
    SourceDocument applyAliases(SourceDocument document, bool smali) const;
    QString canonicalId(const QString &id) const;
    QString methodSource(const SourceDocument &document, const QString &id) const;
    // Returns the declared parent/interface method implemented by id, if indexed.
    QString overrideOf(const QString &id) const;
    QString overrideAnnotation(const QString &id) const;
    QJsonArray overrideAnnotations() const;
    QString resolve(const QString &token, const QString &context) const;
    static QString normalize(QString name);
    static QString sourceStem(const QString &name);
    static QString sourcePath(const QString &directory, const QString &name, const QString &suffix);
    static QString classId(const QString &name) { return "L" + normalize(name) + ";"; }
    static QString classOf(const QString &id);
    void cacheDocument(const QString &key, const SourceDocument &doc) { documents_[key] = doc; }
    void removeDocument(const QString &key) { documents_.remove(key); }
    void clearDocuments() { documents_.clear(); }
    QByteArray sourceBytes(const QString &path, const QString &name) const;
    qint64 documentBytes(const QString &key) const;
    QStringList applicationCandidates() const;
    QString input() const { return input_; }
    void setInputs(const QStringList &inputs) { inputs_ = inputs; }
    QStringList inputs() const { return inputs_; }
  signals:
    void renamed();

  private:
    std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
    ProjectMap classes_, symbols_;
    QHash<QString, QString> aliases_;
    QHash<QString, QStringList> classNames_, parents_;
    QVector<QHash<QString, QString>> undo_;
    QHash<QString, SourceDocument> documents_;
    struct MapArchiveIndex {
        std::mutex lock;
        QString path;
        qint64 scanned = 8;
        QDateTime modified, created;
        QHash<QString, QPair<qint64, quint32>> entries;
    };
    std::shared_ptr<MapArchiveIndex> mapArchive_ = std::make_shared<MapArchiveIndex>();
    std::shared_ptr<MapArchiveIndex> javaArchive_ = std::make_shared<MapArchiveIndex>();
    QByteArray packedBytes(const QString &source, const QString &name, bool java) const;
    struct ReferencePosition { int group, index; };
    struct ReferenceGroup { QString owner, from; };
    struct ReferenceIndex {
        std::mutex lock;
        bool ready = false;
        QVector<ReferenceGroup> groups;
        QHash<QString, int> targets;
        QVector<QList<ReferencePosition>> positions;
    };
    std::shared_ptr<ReferenceIndex> referenceIndex_ = std::make_shared<ReferenceIndex>();
    struct OverrideIndex { std::mutex lock; bool ready = false; QJsonArray entries; };
    std::shared_ptr<OverrideIndex> overrideIndex_ = std::make_shared<OverrideIndex>();
    struct SymbolIndex { std::mutex lock; bool ready = false; QJsonArray entries; };
    std::shared_ptr<SymbolIndex> symbolIndex_ = std::make_shared<SymbolIndex>();
    struct LocalIndex { std::mutex lock; QHash<QString, QHash<QString, QString>> methods; };
    std::shared_ptr<LocalIndex> localIndex_ = std::make_shared<LocalIndex>();
    struct AliasIndex { std::mutex lock; bool ready = false; QString version; QHash<QString, QStringList> classes; };
    std::shared_ptr<AliasIndex> aliasIndex_ = std::make_shared<AliasIndex>();
    QHash<QString, QStringList> classAliases() const;
    QString input_;
    QStringList inputs_;
};
