#pragma once
#include "project.h"
#include <QRegularExpression>
#include <atomic>
#include <functional>
struct SearchOptions {
    QString query, package;
    bool classes = false, methods = false, fields = false, code = true, comments = false;
    bool regex = false, caseSensitive = false;
    int limit = 1000, sourceMiB = 8;
};
struct SearchControl {
    std::atomic_bool canceled{false};
};
struct SearchResult {
    QJsonArray hits;
    int scanned = 0, missing = 0, skipped = 0;
    bool truncated = false, canceled = false;
    QString error;
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
                           const std::shared_ptr<SearchEvents> &events, int request);

Q_DECLARE_METATYPE(SearchResult)
