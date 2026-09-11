#pragma once
#include "project.h"
#include <QCache>
#include <QHash>
#include <QRegularExpression>
#include <array>
#include <atomic>
#include <functional>
struct SearchOptions {
    QString query, package;
    bool classes = false, methods = false, fields = false, code = true, comments = false;
    bool regex = false, caseSensitive = false;
    bool indexOnly = false; // Internal background preparation; never stop at a query hit.
    int limit = 1000, sourceMiB = 8;
};
struct SearchControl {
    std::atomic_bool canceled{false};
};
struct SearchResult {
    QJsonArray hits;
    int scanned = 0, missing = 0, skipped = 0, cachedFiles = 0, indexRejected = 0;
    bool truncated = false, canceled = false;
    QString error;
};
struct SearchDocument {
    QStringList lines, searchable;
    std::array<quint64, 128> grams{};
};
// Shared across requests within one project. KiB costs bound prepared source memory.
struct SearchIndex {
    std::mutex mutex;
    std::atomic_bool ready{false};
    QCache<QString, std::shared_ptr<const SearchDocument>> documents{64 * 1024};
    // Compact per-file Bloom filters survive document-cache eviction. They let later queries
    // reject most immutable project sources without opening or stat'ing every file again.
    QHash<QString, std::array<quint64, 128>> filters;
};
class SearchEvents : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
  signals:
    void batch(int request, const QJsonArray &hits);
    void progress(int request, int scanned, int total);
};
QRegularExpression searchExpression(const SearchOptions &options);
SearchResult searchProject(const std::shared_ptr<Project> &project, const SearchOptions &options,
                           const QString &directory, bool singleClass,
                           const std::shared_ptr<std::atomic_bool> &generating,
                           const std::shared_ptr<SearchControl> &control,
                           const std::shared_ptr<SearchEvents> &events, int request,
                           const std::shared_ptr<SearchIndex> &index = {},
                           bool immutableSources = false);

Q_DECLARE_METATYPE(SearchResult)
