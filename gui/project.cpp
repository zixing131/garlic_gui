#include "project.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <algorithm>

QString Project::normalize(QString name) {
    if (name.startsWith('L') && name.endsWith(';'))
        name = name.mid(1, name.size() - 2);
    return name.replace('.', '/');
}
QString Project::classOf(const QString &id) {
    const int end = id.indexOf(';');
    return end > 0 && id.startsWith('L') ? id.mid(1, end - 1) : normalize(id);
}
void Project::reset(const QString &input) {
    input_ = input;
    inputs_ = {input};
    documents_.clear();
    referenceIndex_ = std::make_shared<ReferenceIndex>();
    overrideIndex_ = std::make_shared<OverrideIndex>();
    classes_.clear();
    symbols_.clear();
    classNames_.clear();
    parents_.clear();
    aliases_.clear();
    undo_.clear();
}
void Project::addClass(const QJsonObject &entry) {
    referenceIndex_ = std::make_shared<ReferenceIndex>();
    if (overrideIndex_->ready)
        overrideIndex_ = std::make_shared<OverrideIndex>();
    const auto name = entry.value("name").toString();
    if (classes_.contains(name)) {
        const auto old = classes_.value(name);
        for (const auto &kind : {"methods", "fields"})
            for (const auto &member : old.value(kind).toArray())
                symbols_.remove(member.toObject().value("id").toString());
    }
    classes_.insert(name, entry);
    parents_.remove(name);
    const auto refs = entry.value("refs");
    if (refs.isObject()) {
        for (const auto &v : refs.toObject().value(classId(name)).toArray()) {
            const auto row = v.toArray();
            if (row[2] == "extends" || row[2] == "implements")
                parents_[name] << classOf(row[0].toString());
        }
    } else
        for (const auto &v : refs.toArray()) {
            const auto ref = v.toObject();
            if (ref.value("kind") == "extends" || ref.value("kind") == "implements")
                parents_[name] << classOf(ref.value("target").toString());
        }
    for (const auto &simple :
         QSet<QString>{name.section('/', -1), name.section('/', -1).section('$', -1)})
        if (!classNames_[simple].contains(classId(name)))
            classNames_[simple] << classId(name);
    symbols_.insert(classId(name), {{"id", classId(name)},
                                    {"name", name.section('/', -1)},
                                    {"owner", name},
                                    {"kind", entry.value("kind")},
                                    {"flags", entry.value("flags")}});
    for (const auto &kind : {QString("methods"), QString("fields")})
        for (const auto &v : entry.value(kind).toArray()) {
            auto s = v.toObject();
            s["owner"] = name;
            s["kind"] = kind == "methods" ? "method" : "field";
            symbols_.insert(s.value("id").toString(), s);
        }
}
QStringList Project::classes() const {
    auto keys = classes_.keys();
    keys.sort();
    return keys;
}
QString Project::owner(const QString &name) const {
    QString n = normalize(name);
    while (classes_.value(n).value("inner").toBool() && n.contains('$')) {
        const QString parent = n.left(n.lastIndexOf('$'));
        if (!classes_.contains(parent))
            break;
        n = parent;
    }
    return n;
}
QJsonArray Project::members(const QString &name, bool methods) const {
    return info(name).value(methods ? "methods" : "fields").toArray();
}
QJsonArray Project::xrefs(const QString &id) const {
    std::lock_guard<std::mutex> guard(referenceIndex_->lock);
    if (!referenceIndex_->ready) {
        QHash<QString, QString> canonical;
        for (auto it = classes_.cbegin(); it != classes_.cend(); ++it) {
            QSet<QString> seen;
            auto add = [&](const QString &from, const QString &raw, int offset,
                           const ReferencePosition &position) {
                if (!canonical.contains(raw))
                    canonical[raw] = canonicalId(raw);
                const auto target = canonical.value(raw);
                const auto key = from + ":" + QString::number(offset) + target;
                if (seen.contains(key))
                    return;
                seen.insert(key);
                referenceIndex_->targets[target].append(position);
                const auto clazz = classId(classOf(target));
                if (clazz != target)
                    referenceIndex_->targets[clazz].append(position);
            };
            const auto refs = it.value().value("refs");
            if (refs.isObject()) {
                const auto groups = refs.toObject();
                for (auto group = groups.begin(); group != groups.end(); ++group) {
                    const auto rows = group.value().toArray();
                    for (int i = 0; i < rows.size(); i++) {
                        const auto row = rows[i].toArray();
                        add(group.key(), row[0].toString(), row[1].toInt(),
                            {it.key(), group.key(), i});
                    }
                }
            } else {
                const auto rows = refs.toArray();
                for (int i = 0; i < rows.size(); i++) {
                    const auto ref = rows[i].toObject();
                    add(ref.value("from").toString(), ref.value("target").toString(),
                        ref.value("offset").toInt(), {it.key(), {}, i});
                }
            }
        }
        referenceIndex_->ready = true;
    }
    QJsonArray result;
    for (const auto &position : referenceIndex_->targets.value(canonicalId(id))) {
        const auto refs = classes_.value(position.owner).value("refs");
        QJsonObject ref;
        if (refs.isObject()) {
            const auto row =
                refs.toObject().value(position.from).toArray().at(position.index).toArray();
            ref = {{"from", position.from},
                   {"target", row[0]},
                   {"offset", row[1]},
                   {"kind", row.size() > 2 ? row[2] : QJsonValue("bytecode")}};
        } else
            ref = refs.toArray().at(position.index).toObject();
        ref["class"] = position.owner;
        result.append(ref);
    }
    return result;
}

QJsonArray Project::symbols(const QString &query) const {
    QJsonArray out;
    for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it)
        if (query.isEmpty() || it.key().contains(query, Qt::CaseInsensitive) ||
            symbolName(it.key()).contains(query, Qt::CaseInsensitive))
            out.append(it.value());
    return out;
}
QString Project::displayName(const QString &name) const {
    return renamedClass(name).section('/', -1);
}
QString Project::renamedClass(const QString &name) const {
    const auto n = normalize(name);
    const auto a = aliases_.value(classId(n));
    if (info(n).value("inner").toBool() && n.contains('$')) {
        const int split = n.lastIndexOf('$');
        return renamedClass(n.left(split)) + "$" + (a.isEmpty() ? n.mid(split + 1) : a);
    }
    return a.isEmpty() ? n : n.left(n.lastIndexOf('/') + 1) + a;
}
QString Project::symbolName(const QString &id) const {
    if (aliases_.contains(id))
        return aliases_.value(id);
    return symbols_.value(id).value("name").toString(id);
}
QString Project::rename(const QString &id, const QString &newName) {
    static const QRegularExpression valid("^[\\p{L}_$][\\p{L}\\p{N}_$]*$");
    static const QSet<QString> reserved = {
        "class",      "interface", "enum",    "public",     "private", "protected", "static",
        "void",       "int",       "long",    "short",      "byte",    "char",      "float",
        "double",     "boolean",   "new",     "return",     "if",      "else",      "for",
        "while",      "switch",    "case",    "default",    "try",     "catch",     "finally",
        "throw",      "throws",    "extends", "implements", "import",  "package",   "this",
        "super",      "null",      "true",    "false",      "final",   "abstract",  "synchronized",
        "volatile",   "transient", "native",  "assert",     "break",   "continue",  "do",
        "instanceof", "const",     "goto",    "strictfp",   "record",  "sealed",    "yield",
        "var"};
    if (!symbols_.contains(id))
        return tr("符号不存在，无法重命名。");
    if (!valid.match(newName).hasMatch() || reserved.contains(newName))
        return tr("请输入合法且非保留字的 Java 标识符。");
    const auto symbol = symbols_.value(id);
    if (symbol.value("name").toString().startsWith('<'))
        return tr("构造方法请通过重命名所属类修改。");
    for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it) {
        if (it.key() == id || symbolName(it.key()) != newName)
            continue;
        const bool classSymbol = !id.contains("->");
        bool sameScope = it.value().value("owner") == symbol.value("owner");
        if (classSymbol)
            sameScope = classOf(it.key()).section('/', 0, -2) == classOf(id).section('/', 0, -2) &&
                        !it.key().contains("->");
        if (sameScope && (classSymbol || it.value().value("kind") == symbol.value("kind")))
            return tr("同一作用域已存在该名称。");
    }
    undo_.push_back(aliases_);
    if (undo_.size() > 100)
        undo_.removeFirst();
    if (newName == symbol.value("name").toString())
        aliases_.remove(id);
    else
        aliases_[id] = newName;
    emit renamed();
    return {};
}
void Project::undoRename() {
    if (undo_.isEmpty())
        return;
    aliases_ = undo_.takeLast();
    emit renamed();
}
QJsonObject Project::aliases() const {
    QJsonObject out;
    for (auto it = aliases_.cbegin(); it != aliases_.cend(); ++it)
        out[it.key()] = it.value();
    return out;
}
bool Project::save(const QString &path, QString *error) const {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QJsonObject data{
        {"version", 1},
        {"input", input_},
        {"size", double(QFileInfo(input_).size())},
        {"modified", QString::number(QFileInfo(input_).lastModified().toMSecsSinceEpoch())},
        {"aliases", aliases()}};
    QJsonArray inputs;
    for (const auto &input : inputs_) {
        QFileInfo info(input);
        inputs.append(
            QJsonObject{{"path", input},
                        {"size", double(info.size())},
                        {"modified", QString::number(info.lastModified().toMSecsSinceEpoch())}});
    }
    data["inputs"] = inputs;
    file.write(QJsonDocument(data).toJson());
    if (file.commit())
        return true;
    if (error)
        *error = file.errorString();
    return false;
}
bool Project::loadAliases(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        if (error)
            *error = tr("无法读取项目。");
        return false;
    }
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    if (data.value("version").toInt() != 1 ||
        QFileInfo(data.value("input").toString()).canonicalFilePath() !=
            QFileInfo(input_).canonicalFilePath() ||
        data.value("size").toDouble() != QFileInfo(input_).size() ||
        (data.contains("modified") &&
         data.value("modified").toString() !=
             QString::number(QFileInfo(input_).lastModified().toMSecsSinceEpoch()))) {
        if (error)
            *error = tr("项目与当前输入文件不匹配。");
        return false;
    }
    if (data.contains("inputs")) {
        QStringList paths;
        for (const auto &value : data.value("inputs").toArray()) {
            const auto item = value.toObject();
            QFileInfo info(item.value("path").toString());
            paths << info.absoluteFilePath();
            if (!info.isFile() || double(info.size()) != item.value("size").toDouble() ||
                QString::number(info.lastModified().toMSecsSinceEpoch()) !=
                    item.value("modified").toString()) {
                if (error)
                    *error = tr("项目输入文件已经变更。");
                return false;
            }
        }
        if (paths != inputs_) {
            if (error)
                *error = tr("项目输入文件列表不匹配。");
            return false;
        }
    }
    const auto previous = aliases_;
    const auto oldUndo = undo_;
    QSignalBlocker blocker(this);
    aliases_.clear();
    const auto values = data.value("aliases").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto message = rename(it.key(), it.value().toString());
        if (!message.isEmpty()) {
            aliases_ = previous;
            undo_ = oldUndo;
            if (error)
                *error = message;
            return false;
        }
    }
    undo_.clear();
    blocker.unblock();
    emit renamed();
    return true;
}
QString Project::resolve(const QString &token, const QString &context) const {
    QStringList candidates = classNames_.value(token);
    for (auto it = aliases_.cbegin(); it != aliases_.cend(); ++it)
        if (!it.key().contains("->") && it.value() == token && !candidates.contains(it.key()))
            candidates << it.key();
    if (candidates.size() == 1)
        return candidates.first();
    const auto pkg = normalize(context).section('/', 0, -2);
    QStringList local;
    for (const auto &id : candidates)
        if (classOf(id).section('/', 0, -2) == pkg)
            local << id;
    return local.size() == 1 ? local.first() : QString();
}

// Mask comments and string/character literals without changing UTF-16 offsets.
static QString codeMask(QString text, bool smali) {
    int i = 0;
    while (i < text.size()) {
        int start = i;
        if ((!smali && text.mid(i, 2) == "//") || (smali && text[i] == '#')) {
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (!smali && text.mid(i, 2) == "/*") {
            int end = text.indexOf("*/", i + 2);
            i = end < 0 ? text.size() : end + 2;
        } else if (text[i] == '"' || text[i] == '\'') {
            QChar q = text[i++];
            while (i < text.size()) {
                if (text[i] == '\\') {
                    i = qMin(int(text.size()), i + 2);
                    continue;
                }
                if (text[i++] == q)
                    break;
            }
        } else {
            ++i;
            continue;
        }
        for (int j = start; j < i; j++)
            if (text[j] != '\n')
                text[j] = ' ';
    }
    return text;
}
SourceDocument Project::document(const QString &name, bool smali, const QString &path,
                                 bool applyAliases) const {
    const QString key = normalize(name) + (smali ? ":smali" : ":java");
    if (path.startsWith("memory:")) {
        const auto doc = documents_.value(key);
        return applyAliases ? this->applyAliases(doc, smali) : doc;
    }
    QFile file(path);
    SourceDocument result;
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const QByteArray bytes = file.readAll();
    result.text = QString::fromUtf8(bytes);
    const QString mask = codeMask(result.text, smali);
    QMap<int, int> occupied;
    auto overlaps = [&](int start, int end) {
        auto it = occupied.lowerBound(start);
        if (it != occupied.end() && it.key() < end)
            return true;
        if (it != occupied.begin() && std::prev(it).value() > start)
            return true;
        return false;
    };
    auto add = [&](int start, int end, const QString &id, bool declaration) {
        if (start < 0 || end <= start || end > result.text.size() || overlaps(start, end))
            return;
        occupied.insert(start, end);
        result.spans.push_back({start, end, canonicalId(id), declaration});
    };
    if (!smali) {
        QString mapPath = path;
        mapPath.chop(5);
        mapPath += ".map.json";
        if (QFileInfo(path).fileName() == "source.java")
            mapPath = QFileInfo(path).absolutePath() + "/" + owner(name) + ".map.json";
        QFile map(mapPath);
        QJsonArray records;
        if (map.open(QIODevice::ReadOnly))
            records = QJsonDocument::fromJson(map.readAll()).array();
        QVector<int> offsets{0};
        for (const auto &v : records) {
            auto r = v.toObject();
            offsets << r.value("start").toInt() << r.value("end").toInt();
        }
        std::sort(offsets.begin(), offsets.end());
        QHash<int, int> positions;
        int previous = 0, chars = 0;
        for (int offset : offsets) {
            if (offset < 0 || offset > bytes.size() || positions.contains(offset))
                continue;
            chars += QString::fromUtf8(bytes.constData() + previous, offset - previous).size();
            positions[offset] = chars;
            previous = offset;
        }
        for (const auto &v : records) {
            const auto r = v.toObject();
            const int from = r.value("start").toInt(), to = r.value("end").toInt();
            if (from < 0 || to > bytes.size() || to <= from)
                continue;
            const int start = positions.value(from), end = positions.value(to);
            const auto token = r.value("token").toString();
            if (token.isEmpty())
                continue;
            const QRegularExpression re("(?<![\\w$])" + QRegularExpression::escape(token) +
                                            "(?![\\w$])",
                                        QRegularExpression::UseUnicodePropertiesOption);
            auto matches = re.globalMatch(mask.mid(start, end - start));
            while (matches.hasNext()) {
                const auto m = matches.next();
                int pos = start + m.capturedStart();
                const bool covered = overlaps(pos, pos + m.capturedLength());
                if (!covered) {
                    add(pos, pos + m.capturedLength(), r.value("id").toString(),
                        r.value("declaration").toBool());
                    break;
                }
            }
        }
        // Source maps describe most member references. Fill the remaining common field form so
        // a double-click on `this.field` is still a semantic navigation target when a decompiler
        // omitted that individual map record.
        QHash<QString, QString> fields;
        QString current = normalize(name);
        QSet<QString> owners;
        QStringList todo{current};
        while (!todo.isEmpty() && owners.size() < 128) {
            const auto owner = todo.takeFirst();
            if (owners.contains(owner))
                continue;
            owners.insert(owner);
            for (const auto &field : members(owner, false)) {
                const auto object = field.toObject();
                const auto fieldName = object.value("name").toString();
                if (!fieldName.isEmpty() && !fields.contains(fieldName))
                    fields.insert(fieldName, canonicalId(object.value("id").toString()));
            }
            todo.append(parents_.value(owner));
        }
        for (auto field = fields.cbegin(); field != fields.cend(); ++field) {
            const QRegularExpression memberAccess(
                "\\b(?:this|super)\\s*\\.\\s*(" + QRegularExpression::escape(field.key()) +
                    ")(?![\\p{L}\\p{N}_$])",
                QRegularExpression::UseUnicodePropertiesOption);
            auto matches = memberAccess.globalMatch(mask);
            while (matches.hasNext()) {
                const auto match = matches.next();
                add(match.capturedStart(1), match.capturedEnd(1), field.value(), false);
            }
        }
        QHash<QString, QString> imports;
        const QRegularExpression importRe("\\bimport\\s+([\\w.$]+)\\s*;");
        auto importsIt = importRe.globalMatch(mask);
        while (importsIt.hasNext()) {
            auto m = importsIt.next();
            const auto n = normalize(m.captured(1));
            imports[n.section('/', -1)] = classId(n);
        }
        const QRegularExpression words("[\\p{L}_$][\\p{L}\\p{N}_$]*");
        auto wordsIt = words.globalMatch(mask);
        while (wordsIt.hasNext()) {
            const auto m = wordsIt.next();
            const QString word = m.captured();
            const QString before = mask.mid(qMax(0, int(m.capturedStart()) - 32),
                                            qMin(32, int(m.capturedStart()))),
                          after = mask.mid(m.capturedEnd(), 80);
            const bool typeContext =
                QRegularExpression("(?:\\b(?:new|instanceof|extends|implements|class|interface|"
                                   "enum|import)\\s+|@)$")
                    .match(before)
                    .hasMatch() ||
                QRegularExpression("^\\s*(?:[.<\\[]|[\\p{L}_$][\\p{L}\\p{N}_$]*\\s*[,;=()])")
                    .match(after)
                    .hasMatch();
            if (!typeContext)
                continue;
            QString id = imports.value(word);
            if (id.isEmpty())
                id = resolve(word, name);
            if (!id.isEmpty())
                add(m.capturedStart(), m.capturedEnd(), id, false);
        }
    } else {
        const QRegularExpression refs("(L[^\\s;]+;)->([\\w$<>]+)(\\([^\\s]*|:[^\\s,]+)");
        auto it = refs.globalMatch(mask);
        while (it.hasNext()) {
            auto m = it.next();
            add(m.capturedStart(2), m.capturedEnd(2), m.captured(), false);
        }
        const QRegularExpression declarations(
            "^\\.(method|field)\\s+[^\\n]*?([\\w$<>]+)(\\([^\\s]*|:[^\\s=]+)",
            QRegularExpression::MultilineOption);
        it = declarations.globalMatch(mask);
        while (it.hasNext()) {
            auto m = it.next();
            add(m.capturedStart(2), m.capturedEnd(2),
                classId(name) + "->" + m.captured(2) + m.captured(3), true);
        }
        const QRegularExpression types("L[^\\s;,()]+;");
        it = types.globalMatch(mask);
        while (it.hasNext()) {
            auto m = it.next();
            add(m.capturedStart(), m.capturedEnd(), m.captured(), false);
        }
    }
    std::sort(result.spans.begin(), result.spans.end(),
              [](const auto &a, const auto &b) { return a.start < b.start; });
    return applyAliases ? this->applyAliases(result, smali) : result;
}

SourceDocument Project::applyAliases(SourceDocument result, bool smali) const {
    {
        int shift = 0;
        for (auto &span : result.spans) {
            const int originalStart = span.start, originalEnd = span.end;
            span.start += shift;
            span.end += shift;
            QString replacement = aliases_.value(span.id);
            if ((!span.id.contains("->") && span.id.endsWith(';')) ||
                (!smali && span.id.contains(";-><init>"))) {
                const QString old = classOf(span.id), renamed = renamedClass(old);
                replacement.clear();
                if (old != renamed) {
                    replacement = smali ? classId(renamed) : renamed.section('/', -1);
                    if (!smali && !result.text.mid(span.start, span.end - span.start).contains('$'))
                        replacement = replacement.section('$', -1);
                }
            }
            if (replacement.isEmpty())
                continue;
            result.text.replace(span.start, span.end - span.start, replacement);
            const int delta = replacement.size() - (originalEnd - originalStart);
            span.end += delta;
            shift += delta;
        }
    }
    return result;
}

QString Project::canonicalId(const QString &id) const {
    if (symbols_.contains(id) || !id.contains("->"))
        return id;
    const QString suffix = id.mid(id.indexOf("->"));
    QStringList work{classOf(id)};
    QSet<QString> visited;
    while (!work.isEmpty() && visited.size() < 128) {
        auto name = work.takeFirst();
        if (visited.contains(name))
            continue;
        visited.insert(name);
        const auto candidate = classId(name) + suffix;
        if (symbols_.contains(candidate))
            return candidate;
        work.append(parents_.value(name));
    }
    return id;
}
QString Project::overrideOf(const QString &id) const {
    if (!id.contains("->") || id.contains("-><") || !symbols_.contains(id))
        return {};
    const auto symbol = symbols_.value(id);
    if (symbol.value("kind") != "method" || (symbol.value("flags").toInt() & 0x0a))
        return {};
    const QString suffix = id.mid(id.indexOf("->"));
    QStringList todo = parents_.value(classOf(id));
    QSet<QString> seen;
    while (!todo.isEmpty() && seen.size() < 128) {
        const auto parent = todo.takeFirst();
        if (seen.contains(parent))
            continue;
        seen.insert(parent);
        const auto candidate = classId(parent) + suffix;
        const auto parentMethod = symbols_.value(candidate);
        if (!parentMethod.isEmpty() && parentMethod.value("kind") == "method" &&
            !(parentMethod.value("flags").toInt() & 0x0a))
            return candidate;
        todo.append(parents_.value(parent));
    }
    return {};
}
QString Project::overrideAnnotation(const QString &id) const {
    const auto parent = overrideOf(id);
    if (parent.isEmpty())
        return {};
    return "@Override // " + classOf(parent).replace('/', '.') + "." + symbolName(parent);
}
QJsonArray Project::overrideAnnotations() const {
    std::lock_guard<std::mutex> guard(overrideIndex_->lock);
    if (!overrideIndex_->ready) {
        for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it) {
            const auto symbol = it.value();
            if (symbol.value("kind") != "method")
                continue;
            const auto id = symbol.value("id").toString();
            const auto text = overrideAnnotation(id);
            if (!text.isEmpty())
                overrideIndex_->entries.append(QJsonObject{{"id", id},
                                                           {"owner", symbol.value("owner")},
                                                           {"flags", symbol.value("flags")},
                                                           {"text", text}});
        }
        overrideIndex_->ready = true;
    }
    return overrideIndex_->entries;
}
QString Project::methodSource(const SourceDocument &document, const QString &id) const {
    const auto mask = codeMask(document.text, false);
    for (const auto &span : document.spans)
        if (span.id == id && span.declaration) {
            int start = document.text.lastIndexOf('\n', span.start) + 1;
            int depth = 0;
            bool entered = false;
            for (int i = span.end; i < mask.size(); i++) {
                if (mask[i] == '{') {
                    entered = true;
                    ++depth;
                } else if (mask[i] == '}' && entered && --depth == 0)
                    return document.text.mid(start, i + 1 - start);
                else if (mask[i] == ';' && !entered)
                    return document.text.mid(start, i + 1 - start);
            }
        }
    return {};
}

std::shared_ptr<Project> Project::snapshot() const {
    auto copy = std::make_shared<Project>();
    copy->replaceData(*this);
    return copy;
}
void Project::replaceData(const Project &other) {
    documents_ = other.documents_;
    referenceIndex_ = other.referenceIndex_;
    overrideIndex_ = other.overrideIndex_;
    input_ = other.input_;
    inputs_ = other.inputs_;
    classes_ = other.classes_;
    symbols_ = other.symbols_;
    classNames_ = other.classNames_;
    parents_ = other.parents_;
    aliases_ = other.aliases_;
    undo_ = other.undo_;
}

qint64 Project::documentBytes(const QString &key) const {
    const auto d = documents_.value(key);
    qint64 n = d.text.size() * 2 + d.spans.size() * sizeof(SourceSpan);
    for (const auto &span : d.spans)
        n += span.id.size() * 2;
    return n;
}
QStringList Project::applicationCandidates() const {
    QStringList result;
    for (const auto &name : classes()) {
        QSet<QString> seen;
        QStringList todo{name};
        bool match = false;
        while (!todo.isEmpty() && seen.size() < 128) {
            const auto n = todo.takeLast();
            if (n == "android/app/Application") {
                match = true;
                break;
            }
            if (seen.contains(n))
                continue;
            seen.insert(n);
            todo.append(parents_.value(n));
        }
        if (match && !(info(name).value("flags").toInt() & 0x600))
            result << name;
    }
    return result;
}
