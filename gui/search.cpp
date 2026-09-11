#include "search.h"
#include "resources.h"
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
    if (o.indexOnly) {
        // Prewarming needs only a superset Bloom filter, not hundreds of thousands
        // of split line lists which immediately evict each other from the LRU.
        const auto folded = text.toCaseFolded();
        for (qsizetype i = 0; i + 2 < folded.size(); ++i) {
            if ((i & 4095) == 0 && control->canceled) return {};
            const auto bit = gram(folded[i], folded[i + 1], folded[i + 2]);
            document->grams[bit / 64] |= quint64(1) << (bit % 64);
        }
        return document;
    }
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
    const bool defaultPackage = package == "<default>" || package == "默认包" ||
        package == "預設套件" || package == "默認包" || package == "(default package)";
    package.replace('.', '/');
    while (package.endsWith('/'))
        package.chop(1);
    auto inPackage = [&](const QString &name) {
        return defaultPackage ? !name.contains('/') : package.isEmpty() || name.startsWith(package + '/');
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
    QHash<QString, int> resourceHitOccurrences;
    auto append = [&](QJsonObject hit) {
        if (hit.value("kind") == "resource" && hit.value("line").toInt() > 0) {
            const auto text = hit.take("fullLine").toString();
            const auto match = re.match(text);
            const int column = o.regex ? match.capturedStart() : text.indexOf(o.query, 0,
                o.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
            hit.insert("lineOccurrence", resourceHitOccurrences[hit.value("path").toString() + "!" + hit.value("entry").toString() + "!" + hit.value("generated").toString() + "\n" + text]++);
            hit.insert("sourceLine", text);
            hit.insert("column", column);
            hit.insert("length", o.regex ? match.capturedLength() : o.query.size());
        }
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
    if (o.resources) {
        for (const auto &input : project->inputs()) {
            if (stopped()) break;
            const auto entries = Resources::inspect(input, std::shared_ptr<std::atomic_bool>(control, &control->canceled)).value("entries").toArray();
            for (const auto &value : entries) {
                if (stopped()) break;
                const auto entry = value.toObject();
                const auto name = entry.value("name").toString();
                const auto path = entry.value("sourcePath").toString(input);
                const auto sourceEntry = entry.value("sourceEntry").toString(name);
                auto hit = [&](const QString &text, int line) {
                    append({{"kind", "resource"}, {"node", name}, {"text", text.left(1200)},
                            {"fullLine", text}, {"line", line}, {"path", path}, {"entry", sourceEntry}});
                };
                if (contains(name)) hit(name, 0);
                if (stopped()) break;
                const auto suffix = QFileInfo(name).suffix().toLower();
                if (!QStringList{"xml", "arsc", "txt", "json", "html", "js", "css", "properties", "csv", "yaml", "yml"}.contains(suffix)) continue;
                QMap<QString, QString> parts;
                if (suffix == "arsc") {
                    const QString tableKey = path + '!' + sourceEntry;
                    if (index) {
                        std::lock_guard<std::mutex> guard(index->mutex);
                        if (auto cached = index->resourceTables.object(tableKey)) parts = *cached;
                    }
                    if (parts.isEmpty()) {
                        QString error;
                        const auto bytes = Resources::read(path, sourceEntry, 128LL * 1048576, &error);
                        if (!error.isEmpty()) { ++result.skipped; continue; }
                        const auto cancel = std::shared_ptr<std::atomic_bool>(control, &control->canceled);
                        Resources::describeTable(bytes, &parts, false, cancel);
                        if (parts.isEmpty() && !control->canceled)
                            parts.insert({}, Resources::describeTable(bytes, nullptr, true, cancel));
                        if (index && !control->canceled) {
                            qint64 cost = 1;
                            for (const auto &text : parts) cost += text.size() * 2LL / 1024 + 1;
                            if (cost <= index->resourceTables.maxCost()) {
                                std::lock_guard<std::mutex> guard(index->mutex);
                                index->resourceTables.insert(tableKey, new QMap<QString, QString>(parts), int(cost));
                            }
                        }
                    }
                } else parts.insert(QString(), QString());
                for (auto part = parts.cbegin(); part != parts.cend() && !stopped(); ++part) {
                const QString key = "resource:" + path + '!' + sourceEntry + '!' + part.key();
                std::shared_ptr<const SearchDocument> document;
                if (index) {
                    std::lock_guard<std::mutex> guard(index->mutex);
                    const auto filter = index->filters.constFind(key);
                    if (filter != index->filters.cend() && !possibleMatch(*filter, bits)) { ++result.indexRejected; continue; }
                    if (auto cached = index->documents.object(key)) { document = *cached; ++result.cachedFiles; }
                }
                if (!document) {
                    QString error;
                    const auto bytes = suffix == "arsc" ? QByteArray() : Resources::read(path, sourceEntry, qint64(o.sourceMiB) * 1048576, &error);
                    if (!error.isEmpty()) { ++result.skipped; continue; }
                    const auto text = suffix == "arsc" ? part.value()
                        : suffix == "xml" ? Resources::decodeXml(bytes) : QString::fromUtf8(bytes);
                    auto options = o; options.code = options.comments = true;
                    document = prepareDocument(text, options, control);
                    if (!document) break;
                    if (index) {
                        std::lock_guard<std::mutex> guard(index->mutex);
                        index->filters.insert(key, document->grams);
                        index->documents.insert(key, new std::shared_ptr<const SearchDocument>(document), qMax(1, int(text.size() * 4LL / 1024)));
                    }
                }
                ++result.scanned;
                for (int line = 0; line < document->lines.size() && !stopped(); ++line)
                    if (contains(document->lines[line])) {
                        if (suffix == "arsc") append({{"kind", "resource"}, {"node", part.key()}, {"text", document->lines[line].left(1200)},
                            {"fullLine", document->lines[line]}, {"line", line + 1}, {"path", path}, {"entry", sourceEntry}, {"generated", part.key()}});
                        else hit(document->lines[line], line + 1);
                    }
                }
            }
        }
        flush();
    }
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
    if (!o.sourceSmali && (o.code || o.comments) && !o.indexOnly && !stopped())
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
                auto owner = o.sourceSmali ? name : project->owner(name);
                if (!seen.contains(owner)) {
                    seen.insert(owner);
                    pending.push_back(owner);
                }
            }
        const int total = pending.size();
        while (!pending.empty() && !stopped()) {
            const bool inFlight = generating->load();
            if (inFlight && QFileInfo::exists(directory + "/source-maps.bin")) {
                QThread::msleep(25);
                continue;
            }
            const int checks = inFlight ? qMin<int>(512, pending.size()) : int(pending.size());
            for (int i = 0; i < checks && !stopped(); ++i) {
                auto name = std::move(pending.front());
                pending.pop_front();
                const QString base = directory + '/' + (singleClass ? "source" : safePaths ? Project::sourceStem(name) : name),
                              path = base + (o.sourceSmali ? ".smali" : ".java");
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
                    if (!info.exists() && !QFileInfo::exists(directory + "/java-sources.bin")) {
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
                if (!info.exists()) {
                    const QFileInfo archive(directory + "/java-sources.bin");
                    filterKey += ":pack:" + QString::number(archive.size()) + ":" + QString::number(archive.lastModified().toMSecsSinceEpoch());
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
                if (hasFilter && (o.indexOnly || filterRejects)) {
                    ++result.indexRejected;
                    continue;
                }
                if (!document) {
                    if (immutableSources) {
                        if (!info.exists() && !QFileInfo::exists(directory + "/java-sources.bin")) {
                            ++result.missing;
                            continue;
                        }
                        if (info.size() > qint64(o.sourceMiB) * 1024 * 1024) {
                            ++result.skipped;
                            continue;
                        }
                    }
                    const auto source = o.sourceSmali ? [&] { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }() : project->sourceBytes(path, name);
                    if (source.isEmpty()) {
                        ++result.missing;
                        continue;
                    }
                    if (source.size() > qint64(o.sourceMiB) * 1048576) { ++result.skipped; continue; }
                    QString text;
                    if (!hasAliases) {
                        text = QString::fromUtf8(source);
                    } else {
                        // Reuse the renamed text across scopes and LRU eviction.
                        // The workspace owns these files; aliases are part of the key.
                        const QString preparedPath = path + ".search-" + aliasVersion;
                        QFile prepared(preparedPath);
                        if (immutableSources && prepared.open(QIODevice::ReadOnly)) {
                            text = QString::fromUtf8(prepared.readAll());
                        } else {
                            text = project->document(name, o.sourceSmali, path).text;
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
                        if (!o.indexOnly && cost <= index->documents.maxCost())
                            index->documents.insert(
                                key, new std::shared_ptr<const SearchDocument>(document),
                                int(cost));
                    }
                }
                if (o.indexOnly || !possibleMatch(document->grams, bits)) {
                    ++result.indexRejected;
                    continue;
                }
                QHash<QString, int> lineOccurrences;
                for (int row = 0; row < document->lines.size() && !stopped(); row++) {
                    const auto &line = document->lines[row];
                    const auto &searchable = document->searchable[row];
                    const int lineNumber = row + 1;
                    const int lineOccurrence = lineOccurrences[line]++;
                    const int matchStart = o.regex
                                               ? re.match(searchable).capturedStart()
                                               : searchable.indexOf(
                                                     o.query, 0,
                                                     o.caseSensitive ? Qt::CaseSensitive
                                                                     : Qt::CaseInsensitive);
                    if (matchStart >= 0) {
                        const int crop = qMax(0, matchStart - 200);
                        append({{"class", name}, {"smali", o.sourceSmali},
                                {"node", QString(name).replace('/', '.')},
                                {"kind", "code"},
                                {"icon_kind", project->info(name).value("kind")},
                                {"flags", project->info(name).value("flags")},
                                {"sourceLine", line}, {"lineOccurrence", lineOccurrence},
                                {"column", matchStart}, {"length", o.regex ? re.match(searchable).capturedLength() : o.query.size()},
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
