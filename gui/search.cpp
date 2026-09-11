#include "search.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <deque>
namespace {
quint32 gram(QChar a, QChar b, QChar c) {
    return (quint32(a.unicode()) * 251u * 251u + quint32(b.unicode()) * 251u + c.unicode()) & 8191u;
}
std::shared_ptr<const SearchDocument>
prepareDocument(const QString &text, const SearchOptions &o,
                const std::shared_ptr<SearchControl> &control) {
    auto document = std::make_shared<SearchDocument>();
    document->lines = text.split('\n');
    if (text.isEmpty() || text.endsWith('\n')) document->lines.removeLast();
    bool blockComment = false;
    for (const auto &line : document->lines) {
        if (control->canceled)
            return {};
        QString searchable = line;
        bool inString = false, escaped = false;
        QChar quote;
        for (int n = 0; !(o.code && o.comments) && n < line.size(); n++) {
            bool comment = blockComment;
            if (!blockComment && !inString && line[n] == '/' && n + 1 < line.size() && line[n + 1] == '/') {
                if (!o.comments)
                    for (int j = n; j < line.size(); j++)
                        searchable[j] = ' ';
                break;
            }
            if (!blockComment && !inString && line[n] == '/' && n + 1 < line.size() && line[n + 1] == '*') {
                blockComment = true;
                comment = true;
            }
            if (blockComment && line[n] == '*' && n + 1 < line.size() && line[n + 1] == '/') {
                if (!o.comments) {
                    searchable[n] = ' ';
                    if (n + 1 < line.size())
                        searchable[n + 1] = ' ';
                }
                blockComment = false;
                ++n;
                continue;
            }
            if (comment && !o.comments)
                searchable[n] = ' ';
            if (!comment && !o.code)
                searchable[n] = ' ';
            if (!comment) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (inString && line[n] == '\\') {
                    escaped = true;
                    continue;
                }
                if (inString && line[n] == quote)
                    inString = false;
                else if (!inString && (line[n] == '"' || line[n] == '\'')) {
                    inString = true;
                    quote = line[n];
                }
            }
        }

        document->searchable << searchable;
        // A superset filter can be shared by code-only, comment-only and combined queries.
        const auto folded = line.toCaseFolded();
        for (int i = 0; i + 2 < folded.size(); i++) {
            auto bit = gram(folded[i], folded[i + 1], folded[i + 2]);
            document->grams[bit / 64] |= quint64(1) << (bit % 64);
        }
    }
    return document;
}
QVector<quint32> queryGrams(const SearchOptions &o) {
    QVector<quint32> bits;
    if (o.regex) return bits;
    const auto query = o.query.toCaseFolded();
    // Scope masking introduces spaces that need not occur in the raw source.
    // Only use grams unaffected by those synthetic spaces.
    for (int i = 0; i + 2 < query.size(); ++i)
        if (!query[i].isSpace() && !query[i + 1].isSpace() && !query[i + 2].isSpace())
            bits.append(gram(query[i], query[i + 1], query[i + 2]));
    return bits;
}
bool possibleMatch(const std::array<quint64, 128> &grams, const QVector<quint32> &bits) {
    for (auto bit : bits)
        if (!(grams[bit / 64] & (quint64(1) << (bit % 64)))) return false;
    return true;
}
} // namespace
QRegularExpression searchExpression(const SearchOptions &o) {
    return QRegularExpression(o.regex ? "(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)" + o.query
                                      : QRegularExpression::escape(o.query),
                              o.caseSensitive ? QRegularExpression::NoPatternOption
                                              : QRegularExpression::CaseInsensitiveOption);
}
SearchResult searchProject(const std::shared_ptr<Project> &project, const SearchOptions &o,
                           const QString &directory, bool singleClass,
                           const std::shared_ptr<std::atomic_bool> &generating,
                           const std::shared_ptr<SearchControl> &control,
                           const std::shared_ptr<SearchEvents> &events, int request,
                           const std::shared_ptr<SearchIndex> &index, bool immutableSources) {
    SearchResult result;
    const bool safePaths = QFileInfo::exists(directory + "/.garlic-safe-paths");
    const bool hasAliases = project->hasAliases();
    const auto bits = queryGrams(o);
    const auto aliasVersion = project->aliasVersion();
    const auto re = searchExpression(o);
    if (!re.isValid()) {
        result.error = re.errorString();
        return result;
    }
    if (o.query.isEmpty())
        return result;
    const auto contains = [&](const QString &text) {
        return o.regex ? re.match(text).hasMatch()
                       : text.contains(o.query, o.caseSensitive ? Qt::CaseSensitive
                                                               : Qt::CaseInsensitive);
    };
    QString package = o.package.trimmed();
    package.replace('.', '/');
    while (package.endsWith('/'))
        package.chop(1);
    auto inPackage = [&](const QString &name) {
        return package.isEmpty() || name.startsWith(package + '/');
    };
    QJsonArray batch;
    QElapsedTimer elapsed;
    elapsed.start();
    auto flush = [&] {
        if (!batch.isEmpty()) {
            emit events->batch(request, batch);
            batch = {};
        }
        elapsed.restart();
    };
    auto append = [&](QJsonObject hit) {
        result.hits.append(hit);
        batch.append(hit);
        if (batch.size() >= 50 || elapsed.elapsed() > 100)
            flush();
    };
    auto stopped = [&] {
        if (control->canceled) {
            result.canceled = true;
            return true;
        }
        if (result.hits.size() >= o.limit) {
            result.truncated = true;
            return true;
        }
        return false;
    };
    if (o.classes || o.methods || o.fields)
        for (const auto &v : project->symbols()) {
            if (stopped())
                break;
            auto symbol = v.toObject();
            const auto owner = symbol.value("owner").toString(),
                       kind = symbol.value("kind").toString(), id = symbol.value("id").toString();
            if (!inPackage(owner))
                continue;
            const bool nameEnabled = kind == "method"  ? o.methods
                                     : kind == "field" ? o.fields
                                                       : o.classes;
            if (!nameEnabled) continue;
            QString text = kind == "method" || kind == "field"
                               ? (hasAliases && !project->alias(id).isEmpty()
                                      ? project->alias(id) : symbol.value("name").toString())
                               : QString(hasAliases ? project->renamedClass(owner) : owner).replace('/', '.');
            if (nameEnabled && contains(text))
                append({{"class", owner},
                        {"id", id},
                        {"node", QString(owner).replace('/', '.') +
                                     (kind == "method" || kind == "field"
                                          ? "." + project->symbolName(id) +
                                                symbol.value("descriptor").toString()
                                          : QString())},
                        {"kind", kind},
                        {"flags", symbol.value("flags")},
                        {"constructor", symbol.value("name").toString() == "<init>"},
                        {"line", 0},
                        {"text", text + symbol.value("descriptor").toString()}});
        }
    if ((o.code || o.comments) && !o.indexOnly && !stopped())
        for (const auto &v : project->overrideAnnotations()) {
            if (stopped())
                break;
            const auto entry = v.toObject();
            const auto owner = entry.value("owner").toString();
            if (!inPackage(owner))
                continue;
            const auto id = entry.value("id").toString();
            const auto annotation = entry.value("text").toString();
            const int commentAt = annotation.indexOf("//");
            QString searchable;
            if (o.code)
                searchable = annotation.left(commentAt).trimmed();
            if (o.comments)
                searchable += (searchable.isEmpty() ? QString() : " ") + annotation.mid(commentAt);
            if (contains(searchable))
                append({{"class", owner},
                        {"id", id},
                        {"node", QString(owner).replace('/', '.') + "." +
                                     project->symbolName(id)},
                        {"kind", "code"},
                        {"icon_kind", project->info(owner).value("kind")},
                        {"flags", entry.value("flags")},
                        {"line", 0},
                        {"text", annotation}});
        }
    flush();
    if ((o.code || o.comments) && !stopped()) {
        std::deque<QString> pending;
        QSet<QString> seen;
        for (const auto &name : project->classes())
            if (inPackage(name)) {
                auto owner = project->owner(name);
                if (!seen.contains(owner)) {
                    seen.insert(owner);
                    pending.push_back(owner);
                }
            }
        const int total = pending.size();
        while (!pending.empty() && !stopped()) {
            const bool inFlight = generating->load();
            const int checks = inFlight ? qMin<int>(512, pending.size()) : int(pending.size());
            for (int i = 0; i < checks && !stopped(); ++i) {
                auto name = std::move(pending.front());
                pending.pop_front();
                const QString base = directory + '/' + (singleClass ? "source" : safePaths ? Project::sourceStem(name) : name),
                              path = base + ".java";
                const QString map =
                    singleClass ? directory + '/' + (safePaths ? Project::sourceStem(name) : name) + ".map.json" : base + ".map.json";
                // The engine publishes a map after flushing a complete class, so in-flight files
                // are not searched.
                if (inFlight && !QFileInfo::exists(map)) {
                    pending.push_back(std::move(name));
                    continue;
                }
                ++result.scanned;
                QFileInfo info(path);
                QString filterKey = path + ":" + aliasVersion;
                if (!immutableSources) {
                    if (!info.exists()) {
                        ++result.missing;
                        continue;
                    }
                    if (info.size() > qint64(o.sourceMiB) * 1024 * 1024) {
                        ++result.skipped;
                        continue;
                    }
                    filterKey += ":" + QString::number(info.size()) + ":" +
                           QString::number(info.lastModified().toMSecsSinceEpoch());
                }
                const QString key = filterKey + ":" + QString::number(o.code) + QString::number(o.comments);
                std::shared_ptr<const SearchDocument> document;
                bool hasFilter = false;
                bool filterRejects = false;
                if (index) {
                    std::lock_guard<std::mutex> lock(index->mutex);
                    if (auto cached = index->documents.object(key)) {
                        document = *cached;
                        ++result.cachedFiles;
                    }
                    const auto cachedFilter = index->filters.constFind(filterKey);
                    if (cachedFilter != index->filters.cend()) {
                        hasFilter = true;
                        filterRejects = !possibleMatch(cachedFilter.value(), bits);
                    }
                }
                if (hasFilter && filterRejects) {
                    ++result.indexRejected;
                    continue;
                }
                if (!document) {
                    if (immutableSources) {
                        if (!info.exists()) {
                            ++result.missing;
                            continue;
                        }
                        if (info.size() > qint64(o.sourceMiB) * 1024 * 1024) {
                            ++result.skipped;
                            continue;
                        }
                    }
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly)) {
                        ++result.missing;
                        continue;
                    }
                    QString text;
                    if (!hasAliases) {
                        text = QString::fromUtf8(file.readAll());
                    } else {
                        // Reuse the renamed text across scopes and LRU eviction.
                        // The workspace owns these files; aliases are part of the key.
                        const QString preparedPath = path + ".search-" + aliasVersion;
                        QFile prepared(preparedPath);
                        if (immutableSources && prepared.open(QIODevice::ReadOnly)) {
                            text = QString::fromUtf8(prepared.readAll());
                        } else {
                            text = project->document(name, false, path).text;
                            if (immutableSources) {
                                QSaveFile saved(preparedPath);
                                if (saved.open(QIODevice::WriteOnly)) {
                                    const auto bytes = text.toUtf8();
                                    if (saved.write(bytes) == bytes.size()) saved.commit();
                                }
                            }
                        }
                    }
                    document = prepareDocument(text, o, control);
                    if (!document)
                        break;
                    if (index) {
                        std::lock_guard<std::mutex> lock(index->mutex);
                        index->filters.insert(filterKey, document->grams);
                        const auto cost = qMax<qint64>(
                            1, (text.size() * 4LL + document->lines.size() * 64LL + 1024) / 1024);
                        if (cost <= index->documents.maxCost())
                            index->documents.insert(
                                key, new std::shared_ptr<const SearchDocument>(document),
                                int(cost));
                    }
                }
                if (o.indexOnly || !possibleMatch(document->grams, bits)) {
                    ++result.indexRejected;
                    continue;
                }
                for (int row = 0; row < document->lines.size() && !stopped(); row++) {
                    const auto &line = document->lines[row];
                    const auto &searchable = document->searchable[row];
                    const int lineNumber = row + 1;
                    const int matchStart = o.regex
                                               ? re.match(searchable).capturedStart()
                                               : searchable.indexOf(
                                                     o.query, 0,
                                                     o.caseSensitive ? Qt::CaseSensitive
                                                                     : Qt::CaseInsensitive);
                    if (matchStart >= 0) {
                        const int crop = qMax(0, matchStart - 200);
                        append({{"class", name},
                                {"node", QString(name).replace('/', '.')},
                                {"kind", "code"},
                                {"icon_kind", project->info(name).value("kind")},
                                {"flags", project->info(name).value("flags")},
                                {"line", lineNumber},
                                {"text", line.mid(crop, 1200)}});
                    }
                }
            }
            flush();
            emit events->progress(request, result.scanned, total);
            if (inFlight && !pending.empty())
                QThread::msleep(25);
        }
    }
    flush();
    stopped();
    return result;
}
