#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <memory>
#include <mutex>

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
    void replaceData(const Project &other);
    void reset(const QString &input);
    void addClass(const QJsonObject &entry);
    QStringList classes() const;
    QJsonObject info(const QString &name) const { return classes_.value(normalize(name)); }
    QString owner(const QString &name) const;
    QJsonArray members(const QString &name, bool methods) const;
    QJsonArray xrefs(const QString &id) const;
    QJsonArray callees(const QString &id) const;
    QJsonArray symbols(const QString &query = {}) const;
    QString displayName(const QString &name) const;
    QString renamedClass(const QString &name) const;
    QString symbolName(const QString &id) const;
    QString alias(const QString &id) const { return aliases_.value(id); }
    QString rename(const QString &id, const QString &newName);
    void deobfuscateNames();
    void undoRename();
    bool canUndo() const { return !undo_.isEmpty(); }
    QJsonObject aliases() const;
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
    static QString classId(const QString &name) { return "L" + normalize(name) + ";"; }
    static QString classOf(const QString &id);
    void cacheDocument(const QString &key, const SourceDocument &doc) { documents_[key] = doc; }
    void removeDocument(const QString &key) { documents_.remove(key); }
    void clearDocuments() { documents_.clear(); }
    qint64 documentBytes(const QString &key) const;
    QStringList applicationCandidates() const;
    QString input() const { return input_; }
    void setInputs(const QStringList &inputs) { inputs_ = inputs; }
    QStringList inputs() const { return inputs_; }
  signals:
    void renamed();

  private:
    QHash<QString, QJsonObject> classes_, symbols_;
    QHash<QString, QString> aliases_;
    QHash<QString, QStringList> classNames_, parents_;
    QVector<QHash<QString, QString>> undo_;
    QHash<QString, SourceDocument> documents_;
    struct ReferencePosition { QString owner, from; int index; };
    struct ReferenceIndex { std::mutex lock; bool ready = false; QHash<QString, QList<ReferencePosition>> targets; };
    std::shared_ptr<ReferenceIndex> referenceIndex_ = std::make_shared<ReferenceIndex>();
    struct OverrideIndex { std::mutex lock; bool ready = false; QJsonArray entries; };
    std::shared_ptr<OverrideIndex> overrideIndex_ = std::make_shared<OverrideIndex>();
    struct SymbolIndex { std::mutex lock; bool ready = false; QJsonArray entries; };
    std::shared_ptr<SymbolIndex> symbolIndex_ = std::make_shared<SymbolIndex>();
    QString input_;
    QStringList inputs_;
};
