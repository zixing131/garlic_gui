#include "search.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>
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
                           const std::shared_ptr<SearchEvents> &events, int request) {
    SearchResult result;
    const auto re = searchExpression(o);
    if (!re.isValid()) {
        result.error = re.errorString();
        return result;
    }
    if (o.query.isEmpty())
        return result;
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
            if (!inPackage(owner) || !(kind == "method"  ? o.methods
                                       : kind == "field" ? o.fields
                                                         : o.classes))
                continue;
            QString text = kind == "method" || kind == "field"
                               ? project->symbolName(id)
                               : QString(project->renamedClass(owner)).replace('/', '.');
            if (re.match(text).hasMatch())
                append({{"class", owner},
                        {"id", id},
                        {"node", QString(owner).replace('/', '.') +
                                     (kind == "method" || kind == "field"
                                          ? "." + project->symbolName(id) +
                                                symbol.value("descriptor").toString()
                                          : QString())},
                        {"kind", kind},
                        {"line", 0},
                        {"text", text + symbol.value("descriptor").toString()}});
        }
    flush();
    if ((o.code || o.comments) && !stopped()) {
        QStringList pending;
        QSet<QString> seen;
        for (const auto &name : project->classes())
            if (inPackage(name)) {
                auto owner = project->owner(name);
                if (!seen.contains(owner)) {
                    seen.insert(owner);
                    pending << owner;
                }
            }
        const int total = pending.size();
        while (!pending.isEmpty() && !stopped()) {
            bool advanced = false;
            for (int i = 0; i < pending.size() && !stopped();) {
                const auto name = pending[i];
                const QString base = directory + '/' + (singleClass ? "source" : name),
                              path = base + ".java";
                const QString map =
                    singleClass ? directory + '/' + name + ".map.json" : base + ".map.json";
                // The engine publishes a map after flushing a complete class, so in-flight files
                // are not searched.
                if (generating->load() && !QFileInfo::exists(map)) {
                    ++i;
                    continue;
                }
                pending.removeAt(i);
                advanced = true;
                ++result.scanned;
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    ++result.missing;
                    continue;
                }
                if (file.size() > qint64(o.sourceMiB) * 1024 * 1024) {
                    ++result.skipped;
                    continue;
                }
                QString text;
                if (project->aliases().isEmpty())
                    text = QString::fromUtf8(file.readAll());
                else
                    text = project->document(name, false, path).text;
                bool blockComment = false;
                int lineNumber = 0, start = 0;
                while (start < text.size() && !stopped()) {
                    int end = text.indexOf('\n', start);
                    if (end < 0)
                        end = text.size();
                    QString line = text.mid(start, end - start);
                    start = end + 1;
                    ++lineNumber;
                    QString searchable = line;
                    bool inString = false, escaped = false;
                    QChar quote;
                    for (int n = 0; n < line.size(); n++) {
                        bool comment = blockComment;
                        if (!blockComment && !inString && line.mid(n, 2) == "//") {
                            if (!o.comments)
                                for (int j = n; j < line.size(); j++)
                                    searchable[j] = ' ';
                            break;
                        }
                        if (!blockComment && !inString && line.mid(n, 2) == "/*") {
                            blockComment = true;
                            comment = true;
                        }
                        if (blockComment && line.mid(n, 2) == "*/") {
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
                    auto match = re.match(searchable);
                    if (match.hasMatch()) {
                        const int crop = qMax(0, int(match.capturedStart()) - 200);
                        append({{"class", name},
                                {"node", QString(name).replace('/', '.')},
                                {"kind", "code"},
                                {"line", lineNumber},
                                {"text", line.mid(crop, 1200)}});
                    }
                }
            }
            flush();
            emit events->progress(request, result.scanned, total);
            if (!advanced && !pending.isEmpty())
                QThread::msleep(100);
        }
    }
    flush();
    stopped();
    return result;
}
