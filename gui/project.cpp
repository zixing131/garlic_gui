#include "project.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QtEndian>
#include <algorithm>

static QString referenceTarget(const QJsonArray &dictionary, const QJsonValue &value) {
    if (value.isString()) return value.toString();
    const int index = value.toInt(-1);
    return value.isDouble() && value.toDouble() == index && index >= 0 && index < dictionary.size()
        ? dictionary.at(index).toString() : QString();
}

QString Project::normalize(QString name) {
    if (name.startsWith('L') && name.endsWith(';'))
        name = name.mid(1, name.size() - 2);
    return name.replace('.', '/');
}
QString Project::sourceStem(const QString &name) {
    const auto hex = normalize(name).toUtf8().toHex();
    QStringList parts;
    for (int i = 0; i < hex.size(); i += 64) parts.append(QString::fromLatin1(hex.mid(i, 64)));
    return "_classes/" + parts.join('/');
}
QString Project::sourcePath(const QString &directory, const QString &name, const QString &suffix) {
    return QDir(directory).filePath((QFileInfo::exists(QDir(directory).filePath(".garlic-safe-paths"))
        ? sourceStem(name) : normalize(name)) + suffix);
}
QString Project::classOf(const QString &id) {
    const int end = id.indexOf(';');
    return end > 0 && id.startsWith('L') ? id.mid(1, end - 1) : normalize(id);
}
void Project::reset(const QString &input) {
    canceled_ = std::make_shared<std::atomic_bool>(false);
    input_ = input;
    inputs_ = {input};
    documents_.clear();
    localIndex_ = std::make_shared<LocalIndex>();
    mapArchive_ = std::make_shared<MapArchiveIndex>();
    javaArchive_ = std::make_shared<MapArchiveIndex>();
    referenceIndex_ = std::make_shared<ReferenceIndex>();
    overrideIndex_ = std::make_shared<OverrideIndex>();
    symbolIndex_ = std::make_shared<SymbolIndex>();
    classes_.clear();
    symbols_.clear();
    classNames_.clear();
    parents_.clear();
    aliases_.clear();
    aliasIndex_ = std::make_shared<AliasIndex>();
    undo_.clear();
}
void Project::addClass(const QJsonObject &entry) {
    if (referenceIndex_.use_count() > 1 || referenceIndex_->ready || !referenceIndex_->groups.isEmpty())
        referenceIndex_ = std::make_shared<ReferenceIndex>();
    if (overrideIndex_->ready)
        overrideIndex_ = std::make_shared<OverrideIndex>();
    if (symbolIndex_->ready)
        symbolIndex_ = std::make_shared<SymbolIndex>();
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
            const auto kind = row.size() > 2 ? row.at(2).toString() : QString();
            if ((kind == "extends" || kind == "implements") && !row.isEmpty())
                parents_[name] << classOf(referenceTarget(entry.value("ref_targets").toArray(), row.at(0)));
        }
    } else
        for (const auto &v : refs.toArray()) {
            const auto ref = v.toObject();
            if (ref.value("kind") == "extends" || ref.value("kind") == "implements")
                parents_[name] << classOf(ref.value("target").toString());
        }
    const auto id = classId(name);
    // Directory entries do not populate this map. A full class symbol proves
    // membership already exists, without scanning every other obfuscated "a".
    if (!symbols_.contains(id))
        for (const auto &simple : QSet<QString>{name.section('/', -1), name.section('/', -1).section('$', -1)})
            classNames_[simple].append(id);
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
        QHash<QString, QPair<int, int>> canonical;
        auto intern = [&](const QString &id) {
            auto found = referenceIndex_->targets.constFind(id);
            if (found != referenceIndex_->targets.cend()) return found.value();
            const int index = referenceIndex_->positions.size();
            referenceIndex_->targets.insert(id, index);
            referenceIndex_->positions.append(QList<ReferencePosition>{});
            return index;
        };
        auto resolveTarget = [&](const QString &raw) -> QPair<int, int> {
            if (raw.isEmpty()) return {-1, -1};
            const auto found = canonical.constFind(raw);
            if (found != canonical.cend()) return found.value();
            const auto id = canonicalId(raw);
            const QPair<int, int> result{intern(id), intern(classId(classOf(id)))};
            canonical.insert(raw, result);
            return result;
        };
        auto add = [&](const QPair<int, int> &target, int offset, const ReferencePosition &position,
                       QSet<quint64> &seen) {
            if (target.first < 0) return;
            const quint64 key = (quint64(quint32(target.first)) << 32) | quint32(offset);
            if (seen.contains(key)) return;
            seen.insert(key);
            referenceIndex_->positions[target.first].append(position);
            if (target.second != target.first)
                referenceIndex_->positions[target.second].append(position);
        };
        for (auto it = classes_.cbegin(); it != classes_.cend(); ++it) {
            if (canceled_->load()) return {};
            const auto refs = it.value().value("refs");
            const auto dictionary = it.value().value("ref_targets").toArray();
            QVector<QPair<int, int>> resolved;
            resolved.reserve(dictionary.size());
            for (const auto &raw : dictionary) resolved.append(resolveTarget(raw.toString()));
            if (refs.isObject()) {
                const auto groups = refs.toObject();
                for (auto g = groups.begin(); g != groups.end(); ++g) {
                    const auto from = g.key();
                    const int group = referenceIndex_->groups.size();
                    referenceIndex_->groups.append({it.key(), from});
                    QSet<quint64> seen;
                    const auto rows = g.value().toArray();
                    for (int i = 0; i < rows.size(); ++i) {
                        const auto row = rows[i].toArray();
                        if (row.size() < 2) continue;
                        const auto raw = row.at(0);
                        const int number = raw.toInt(-1);
                        const auto target = raw.isDouble() && raw.toDouble() == number &&
                            number >= 0 && number < resolved.size() ? resolved[number] : resolveTarget(raw.toString());
                        add(target, row.at(1).toInt(), {group, i}, seen);
                    }
                }
            } else {
                const int group = referenceIndex_->groups.size();
                referenceIndex_->groups.append({it.key(), {}});
                QHash<QString, QSet<quint64>> seen;
                const auto rows = refs.toArray();
                for (int i = 0; i < rows.size(); ++i) {
                    const auto ref = rows[i].toObject();
                    add(resolveTarget(ref.value("target").toString()), ref.value("offset").toInt(),
                        {group, i}, seen[ref.value("from").toString()]);
                }
            }
        }
        referenceIndex_->ready = true;
    }
    QJsonArray result;
    const int targetIndex = referenceIndex_->targets.value(canonicalId(id), -1);
    if (targetIndex < 0) return result;
    for (const auto &position : referenceIndex_->positions.at(targetIndex)) {
        const auto &group = referenceIndex_->groups.at(position.group);
        const auto entry = classes_.value(group.owner);
        const auto refs = entry.value("refs");
        QJsonObject ref;
        if (refs.isObject()) {
            const auto rows = refs.toObject().value(group.from).toArray();
            if (position.index < 0 || position.index >= rows.size())
                continue;
            const auto row = rows.at(position.index).toArray();
            if (row.size() < 2)
                continue;
            ref = {{"from", group.from},
                   {"target", referenceTarget(entry.value("ref_targets").toArray(), row.at(0))},
                   {"offset", row.at(1)},
                   {"kind", row.size() > 2 ? row.at(2) : QJsonValue("bytecode")}};
        } else
            ref = refs.toArray().at(position.index).toObject();
        ref["class"] = group.owner;
        result.append(ref);
    }
    return result;
}

QJsonArray Project::callees(const QString &id) const {
    QJsonArray result;
    if (!id.contains("->"))
        return result;
    const auto ownerName = classOf(id);
    const auto refs = classes_.value(ownerName).value("refs");
    QSet<QString> seen;
    auto append = [&](const QString &raw, int offset, const QString &kind) {
        if (!raw.contains("->") || (kind != "bytecode" && !kind.isEmpty()))
            return;
        const auto target = canonicalId(raw);
        const auto key = target + ':' + QString::number(offset);
        if (seen.contains(key))
            return;
        seen.insert(key);
        result.append(QJsonObject{{"from", id},
                                  {"target", target},
                                  {"offset", offset},
                                  {"kind", kind.isEmpty() ? "bytecode" : kind},
                                  {"class", ownerName}});
    };
    if (refs.isObject()) {
        const auto rows = refs.toObject().value(id).toArray();
        for (const auto &value : rows) {
            const auto row = value.toArray();
            if (row.size() >= 2)
                append(referenceTarget(classes_.value(ownerName).value("ref_targets").toArray(), row.at(0)), row.at(1).toInt(),
                       row.size() > 2 ? row.at(2).toString() : QStringLiteral("bytecode"));
        }
    } else {
        for (const auto &value : refs.toArray()) {
            const auto ref = value.toObject();
            if (canonicalId(ref.value("from").toString()) == canonicalId(id))
                append(ref.value("target").toString(), ref.value("offset").toInt(),
                       ref.value("kind").toString());
        }
    }
    return result;
}

QJsonArray Project::symbols(const QString &query) const {
    std::lock_guard<std::mutex> guard(symbolIndex_->lock);
    if (!symbolIndex_->ready) {
        for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it) {
            if (canceled_->load()) return {};
            symbolIndex_->entries.append(it.value());
        }
        symbolIndex_->ready = true;
    }
    if (query.isEmpty())
        return symbolIndex_->entries;
    QJsonArray out;
    for (const auto &value : symbolIndex_->entries) {
        const auto symbol = value.toObject();
        const auto id = symbol.value("id").toString();
        if (id.contains(query, Qt::CaseInsensitive) ||
            symbolName(id).contains(query, Qt::CaseInsensitive))
            out.append(symbol);
    }
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
    if (id.contains("@local:")) return id.section(':', -1);
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
    const bool local = id.contains("@local:") && symbols_.contains(id.section("@local:", 0, 0));
    if (!symbols_.contains(id) && !local)
        return tr("符号不存在，无法重命名。");
    if (!valid.match(newName).hasMatch() || reserved.contains(newName))
        return tr("请输入合法且非保留字的 Java 标识符。");
    const auto symbol = local ? QJsonObject{{"name", id.section(':', -1)}} : symbols_.value(id);
    if (symbol.value("name").toString().startsWith('<'))
        return tr("构造方法请通过重命名所属类修改。");
    QStringList candidates;
    if (local) {
        const auto method = id.section("@local:", 0, 0);
        QHash<QString, QString> variables;
        {
            std::lock_guard<std::mutex> guard(localIndex_->lock);
            variables = localIndex_->methods.value(method);
        }
        for (auto it = aliases_.cbegin(); it != aliases_.cend(); ++it)
            if (it.key().startsWith(method + "@local:")) variables.insert(it.key(), it.key().section(':', -1));
        for (auto it = variables.cbegin(); it != variables.cend(); ++it)
            if (it.key() != id && aliases_.value(it.key(), it.value()) == newName)
                return tr("该方法中已有同名局部变量。");
    }
    if (!local && id.contains("->")) {
        for (const auto &value : members(classOf(id), symbol.value("kind") == "method"))
            candidates << value.toObject().value("id").toString();
    } else if (!local) {
        // Exact original-name lookup plus the (usually small) project alias set.
        const auto package = classOf(id).section('/', 0, -2);
        const auto target = package.isEmpty() ? newName : package + '/' + newName;
        candidates << classId(target);
        for (auto it = aliases_.cbegin(); it != aliases_.cend(); ++it)
            if (!it.key().contains("->") && classOf(it.key()).section('/', 0, -2) == classOf(id).section('/', 0, -2))
                candidates << it.key();
    }
    for (const auto &candidate : candidates)
        if (candidate != id && symbols_.contains(candidate) && symbolName(candidate) == newName)
            return tr("同一作用域已存在该名称。");
    undo_.push_back(aliases_);
    if (undo_.size() > 100)
        undo_.removeFirst();
    if (newName == symbol.value("name").toString())
        aliases_.remove(id);
    else
        aliases_[id] = newName;
    aliasIndex_ = std::make_shared<AliasIndex>();
    emit renamed();
    return {};
}
void Project::deobfuscateNames() {
    aliasIndex_ = std::make_shared<AliasIndex>();
    static const QRegularExpression identifier("^[\\p{L}_$][\\p{L}\\p{N}_$]*$");
    static const QRegularExpression noisy("[\\p{Cc}\\p{Cf}\\p{Co}\\p{Cs}\\p{Cn}\\p{Mn}\\p{Mc}]");
    QSet<QString> used;
    for (auto it = symbols_.cbegin(); it != symbols_.cend(); ++it) {
        if (canceled_->load()) return;
        used.insert(aliases_.value(it.key(), it.value().value(QStringLiteral("name")).toString(it.key())));
    }
    auto ids = symbols_.keys();
    std::sort(ids.begin(), ids.end());
    int classNumber = 1, fieldNumber = 1, methodNumber = 1;
    for (const auto &id : ids) {
        if (canceled_->load()) return;
        const auto symbol = symbols_.value(id);
        const auto name = symbol.value("name").toString();
        bool asciiIdentifier = !name.isEmpty();
        for (qsizetype i = 0; i < name.size() && asciiIdentifier; ++i) {
            const auto c = name.at(i).unicode();
            asciiIdentifier = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
                (i > 0 && c >= '0' && c <= '9');
        }
        const bool suspicious = name.size() <= 1 || (!asciiIdentifier &&
            (noisy.match(name).hasMatch() || name.contains(QChar(0xfffd))));
        if (aliases_.contains(id) || name.isEmpty() || name == "<init>" || name == "<clinit>" ||
            ((asciiIdentifier || identifier.match(name).hasMatch()) && !suspicious))
            continue;
        const bool method = id.contains("->") && id.contains('(');
        const bool member = id.contains("->");
        const QString prefix = !member ? "Class_" : method ? "method_" : "field_";
        int &number = !member ? classNumber : method ? methodNumber : fieldNumber;
        QString replacement;
        do { replacement = prefix + QString::number(number++); } while (used.contains(replacement));
        aliases_.insert(id, replacement);
        used.insert(replacement);
    }
}

void Project::undoRename() {
    if (undo_.isEmpty())
        return;
    aliases_ = undo_.takeLast();
    aliasIndex_ = std::make_shared<AliasIndex>();
    emit renamed();
}
QJsonObject Project::aliases() const {
    QJsonObject out;
    auto keys = aliases_.keys(); keys.sort();
    for (const auto &key : keys) out.insert(key, aliases_.value(key));
    return out;
}
QString Project::aliasVersion() const {
    std::lock_guard<std::mutex> guard(aliasIndex_->lock);
    if (aliasIndex_->version.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        auto ids = aliases_.keys();
        ids.sort();
        for (const auto &id : ids) {
            for (const auto &part : {id.toUtf8(), aliases_.value(id).toUtf8()}) {
                hash.addData(QByteArray::number(part.size()) + ':');
                hash.addData(part);
            }
        }
        aliasIndex_->version = QString::fromLatin1(hash.result().toHex());
    }
    return aliasIndex_->version;
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
    aliasIndex_ = std::make_shared<AliasIndex>();
    const auto values = data.value("aliases").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto message = rename(it.key(), it.value().toString());
        if (!message.isEmpty()) {
            aliases_ = previous;
            aliasIndex_ = std::make_shared<AliasIndex>();
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
QHash<QString, QStringList> Project::classAliases() const {
    std::lock_guard<std::mutex> guard(aliasIndex_->lock);
    if (!aliasIndex_->ready) {
        for (auto it = aliases_.cbegin(); it != aliases_.cend(); ++it)
            if (!it.key().contains("->")) aliasIndex_->classes[it.value()].append(it.key());
        aliasIndex_->ready = true;
    }
    return aliasIndex_->classes;
}
QString Project::resolve(const QString &token, const QString &context) const {
    QStringList candidates = classNames_.value(token);
    for (const auto &id : classAliases().value(token))
        if (!candidates.contains(id)) candidates << id;
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
        if ((!smali && text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') || (smali && text[i] == '#')) {
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (!smali && text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
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
QByteArray Project::packedBytes(const QString &source, const QString &name, bool java) const {
    const int directory = source.lastIndexOf("/_classes/");
    QString root = directory < 0 ? QFileInfo(source).absolutePath() : source.left(directory);
    const auto relative = normalize(name) + ".java";
    if (directory < 0 && source.endsWith('/' + relative)) root = source.left(source.size() - relative.size() - 1);
    const auto path = root + (java ? "/java-sources.bin" : "/source-maps.bin");
    const auto archive = java ? javaArchive_ : mapArchive_;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.read(8) != "GSMAP001") return {};
    QFile index(path + ".index");
    if (!index.open(QIODevice::ReadOnly) || index.read(8) != "GSMIDX01") return {};
    std::unique_lock<std::mutex> guard(archive->lock);
    const auto archiveSize = quint64(file.size());
    const QFileInfo indexInfo(index);
    const auto modified = indexInfo.lastModified();
    if (archive->path != path || archive->created != indexInfo.birthTime() || index.size() < archive->scanned ||
        (index.size() == archive->scanned && archive->modified != modified)) {
        archive->path = path; archive->scanned = 8; archive->entries.clear();
    }
    archive->modified = modified; archive->created = indexInfo.birthTime();
    index.seek(archive->scanned);
    const auto entries = index.readAll();
    qsizetype at = 0;
    while (at + 16 <= entries.size()) {
        if (canceled_->load()) return {};
        const auto header = entries.constData() + at;
        const auto names = qFromLittleEndian<quint32>(header);
        const auto offset = qFromLittleEndian<quint64>(header + 4);
        const auto bytes = qFromLittleEndian<quint32>(header + 12);
        if (!names || names > 65536 || bytes > 128 * 1048576u || at + 16 + names > entries.size() || offset > archiveSize || bytes > archiveSize - offset) break;
        const auto key = QString::fromUtf8(header + 16, names);
        archive->entries.insert(key, {qint64(offset), bytes});
        at += 16 + names;
    }
    archive->scanned += at;
    const auto found = archive->entries.constFind(normalize(name));
    if (found == archive->entries.cend()) return {};
    const auto entry = *found;
    guard.unlock(); // Each reader owns its QFile; source I/O must not block other classes.
    if (!file.seek(entry.first)) return {};
    return file.read(entry.second);
}
QByteArray Project::sourceBytes(const QString &path, const QString &name) const {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) return file.readAll();
    return packedBytes(path, owner(name), true);
}
SourceDocument Project::document(const QString &name, bool smali, const QString &path,
                                 bool applyAliases) const {
    const QString key = normalize(name) + (smali ? ":smali" : ":java");
    if (path.startsWith("memory:")) {
        const auto doc = documents_.value(key);
        return applyAliases ? this->applyAliases(doc, smali) : doc;
    }
    SourceDocument result;
    const QByteArray bytes = sourceBytes(path, name);
    if (bytes.isEmpty()) return result;
    result.text = QString::fromUtf8(bytes);
    const QString mask = codeMask(result.text, smali);
    QMap<int, int> occupied;
    QHash<QString, QString> canonical;
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
        if (!canonical.contains(id)) canonical.insert(id, canonicalId(id));
        result.spans.push_back({start, end, canonical.value(id), declaration});
    };
    if (!smali) {
        QString mapPath = path;
        mapPath.chop(5);
        mapPath += ".map.json";
        if (QFileInfo(path).fileName() == "source.java")
            mapPath = sourcePath(QFileInfo(path).absolutePath(), owner(name), ".map.json");
        QFile map(mapPath);
        QJsonArray records;
        if (map.open(QIODevice::ReadOnly))
            records = QJsonDocument::fromJson(map.readAll()).array();
        else records = QJsonDocument::fromJson(packedBytes(path, owner(name), false)).array();
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
                // A mapped call can contain a receiver/argument with the same name.
                // Select the call identifier, not the first same-spelled local variable.
                const auto id = r.value("id").toString();
                if (id.contains("->") && id.contains('(')) {
                    int next = pos + m.capturedLength();
                    while (next < end && mask[next].isSpace()) ++next;
                    if (next >= mask.size() || mask[next] != '(') continue;
                }
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
            if (canceled_->load()) return {};
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
    if (!smali) {
        // Pair lexical delimiters once; comments and strings are already masked.
        QHash<int, int> closes;
        QVector<int> stack;
        for (int i = 0; i < mask.size(); ++i) {
            if (mask[i] == '{' || mask[i] == '(') stack << i;
            else if ((mask[i] == '}' || mask[i] == ')') && !stack.isEmpty()) {
                closes.insert(stack.takeLast(), i);
            }
        }
        const auto declarations = result.spans;
        static const QRegularExpression declaration(
            R"((?:^|[;{}(,])\s*(?:(?:final|volatile|transient)\s+)*(?!return\b|throw\b|new\b|case\b)(?:[\p{L}_$][\p{L}\p{N}_$.]*(?:\s*<[^;{}()]*>)?(?:\s*\[\s*\])*)(?:\s*\.\.\.)?\s+([\p{L}_$][\p{L}\p{N}_$]*)\s*(?=[=;,:\)\[]))");
        static const QRegularExpression identifier(R"([\p{L}_$][\p{L}\p{N}_$]*)");
        for (const auto &method : declarations) {
            if (canceled_->load()) return {};
            if (!method.declaration || !method.id.contains("->") || !method.id.contains('(')) continue;
            int parameters = method.end;
            while (parameters < mask.size() && mask[parameters].isSpace()) ++parameters;
            if (parameters >= mask.size() || mask[parameters] != '(' || !closes.contains(parameters)) continue;
            int body = closes.value(parameters) + 1;
            while (body < mask.size() && mask[body].isSpace()) ++body;
            if (mask.mid(body, 6) == "throws")
                while (body < mask.size() && mask[body] != '{' && mask[body] != ';') ++body;
            if (body >= mask.size() || mask[body] != '{' || !closes.contains(body)) continue;
            const int end = closes.value(body);
            struct Local { int start, end; QString id; };
            QHash<QString, QVector<Local>> locals;
            const auto region = mask.mid(parameters, end - parameters + 1);
            QVector<int> scopeEnds(region.size(), end);
            QVector<int> scopeStack;
            for (int i = parameters; i <= end; ++i) {
                while (!scopeStack.isEmpty() && scopeStack.last() < i) scopeStack.removeLast();
                if (closes.contains(i) && i != parameters) {
                    int last = closes.value(i);
                    if (mask[i] == '(') {
                        int after = last + 1;
                        while (after < end && mask[after].isSpace()) ++after;
                        if (mask[after] == '{' && closes.contains(after)) last = closes.value(after);
                        else {
                            const auto prefix = mask.mid(qMax(parameters, i - 12), qMin(12, i - parameters));
                            if (QRegularExpression(R"(\b(?:for|catch)\s*$)").match(prefix).hasMatch()) {
                                const int semicolon = mask.indexOf(';', after);
                                last = semicolon < 0 ? end : qMin(end, semicolon);
                            } else last = end;
                        }
                    }
                    scopeStack << (scopeStack.isEmpty() ? last : qMin(last, scopeStack.last()));
                }
                if (!scopeStack.isEmpty()) scopeEnds[i - parameters] = scopeStack.last();
            }
            auto matches = declaration.globalMatch(region);
            while (matches.hasNext()) {
                const auto match = matches.next();
                const int start = parameters + match.capturedStart(1);
                const int scopeEnd = scopeEnds[start - parameters];
                const auto token = match.captured(1);
                locals[token].append({start, scopeEnd, method.id + "@local:" + QString::number(start - parameters) + ':' + token});
            }
            // Subsequent declarators share a type: int x = call(1, 2), y = 3.
            auto groups = declaration.globalMatch(region);
            while (groups.hasNext()) {
                const auto group = groups.next();
                int depth = 0;
                for (int at = parameters + group.capturedEnd(1); at < end; ++at) {
                    const auto c = mask[at];
                    if (c == '(' || c == '[' || c == '{') ++depth;
                    else if (c == ')' || c == ']' || c == '}') { if (!depth) break; --depth; }
                    if (!depth && c == ';') break;
                    if (depth || c != ',') continue;
                    const auto next = QRegularExpression(R"(^\s*([\p{L}_$][\p{L}\p{N}_$]*)\s*(?=[=,;]))").match(mask.mid(at + 1, end - at - 1));
                    if (!next.hasMatch()) break;
                    const int start = at + 1 + next.capturedStart(1);
                    const auto token = next.captured(1);
                    locals[token].append({start, scopeEnds[start - parameters], method.id + "@local:" + QString::number(start - parameters) + ':' + token});
                }
            }
            static const QRegularExpression lambda(R"((?:\(([\p{L}\p{N}_$,\s]*)\)|([\p{L}_$][\p{L}\p{N}_$]*))\s*->)");
            auto lambdas = lambda.globalMatch(region);
            while (lambdas.hasNext()) {
                const auto match = lambdas.next();
                int bodyStart = parameters + match.capturedEnd();
                while (bodyStart < end && mask[bodyStart].isSpace()) ++bodyStart;
                int scopeEnd = closes.value(bodyStart, -1);
                if (scopeEnd < 0) {
                    scopeEnd = bodyStart;
                    while (scopeEnd < end && mask[scopeEnd] != ';' && mask[scopeEnd] != ',') {
                        if (closes.contains(scopeEnd)) scopeEnd = closes.value(scopeEnd);
                        ++scopeEnd;
                    }
                }
                const int capture = match.capturedStart(1) >= 0 ? 1 : 2;
                const auto parts = match.captured(capture).split(',');
                bool untyped = true;
                for (const auto &part : parts)
                    if (!QRegularExpression(R"(^\s*[\p{L}_$][\p{L}\p{N}_$]*\s*$)").match(part).hasMatch()) untyped = false;
                if (!untyped) continue;
                auto names = identifier.globalMatch(match.captured(capture));
                while (names.hasNext()) {
                    const auto name = names.next();
                    const int start = parameters + match.capturedStart(capture) + name.capturedStart();
                    const auto token = name.captured();
                    locals[token].append({start, scopeEnd, method.id + "@local:" + QString::number(start - parameters) + ':' + token});
                }
            }
            for (auto it = locals.begin(); it != locals.end(); ++it)
                std::sort(it->begin(), it->end(), [](const Local &a, const Local &b) { return a.start < b.start; });
            {
                QHash<QString, QString> variables;
                for (auto it = locals.cbegin(); it != locals.cend(); ++it)
                    for (const auto &variable : it.value()) variables.insert(variable.id, it.key());
                std::lock_guard<std::mutex> guard(localIndex_->lock);
                localIndex_->methods.insert(method.id, variables);
            }
            auto tokens = identifier.globalMatch(region);
            while (tokens.hasNext()) {
                const auto token = tokens.next();
                const int start = parameters + token.capturedStart();
                int previous = start - 1;
                while (previous >= parameters && mask[previous].isSpace()) --previous;
                if (previous >= parameters && mask[previous] == '.') continue;
                const auto found = locals.constFind(token.captured());
                if (found == locals.cend()) continue;
                for (auto it = found->crbegin(); it != found->crend(); ++it)
                    if (start >= it->start && start <= it->end) {
                        add(start, start + token.capturedLength(), it->id, start == it->start);
                        break;
                    }
            }
        }
    }
    // Lexically resolved locals take priority over guessed type contexts (a[i], a < b).
    if (!smali) {
        QHash<QString, QString> imports;
        const QRegularExpression importRe("\\bimport\\s+([\\w.$]+)\\s*;");
        auto importsIt = importRe.globalMatch(mask);
        while (importsIt.hasNext()) {
            auto m = importsIt.next();
            const auto n = normalize(m.captured(1));
            imports[n.section('/', -1)] = classId(n);
        }
        const auto aliasClasses = classAliases();
        auto resolveType = [&](const QString &word) -> QString {
            auto candidates = classNames_.value(word);
            for (const auto &id : aliasClasses.value(word))
                if (!candidates.contains(id)) candidates.append(id);
            QStringList local;
            const auto package = normalize(name).section('/', 0, -2);
            for (const auto &id : candidates)
                if (classOf(id).section('/', 0, -2) == package) local.append(id);
            return local.size() == 1 ? local.first() : QString();
        };
        const QRegularExpression typeBefore("(?:\\b(?:new|instanceof|extends|implements|class|interface|"
                                            "enum|import)\\s+|@)$");
        const QRegularExpression typeAfter("^\\s*(?:[<\\[]|[\\p{L}_$][\\p{L}\\p{N}_$]*\\s*[,;=()])");
        const QRegularExpression words("[\\p{L}_$][\\p{L}\\p{N}_$]*");
        auto wordsIt = words.globalMatch(mask);
        while (wordsIt.hasNext()) {
            if (canceled_->load()) return {};
            const auto m = wordsIt.next();
            if (overlaps(m.capturedStart(), m.capturedEnd())) continue;
            const QString word = m.captured();
            const QString before = mask.mid(qMax(0, int(m.capturedStart()) - 32),
                                            qMin(32, int(m.capturedStart()))),
                          after = mask.mid(m.capturedEnd(), 80);
            const bool typeContext = typeBefore.match(before).hasMatch() || typeAfter.match(after).hasMatch();
            if (!typeContext)
                continue;
            QString id = imports.value(word);
            if (id.isEmpty())
                id = resolveType(word);
            if (!id.isEmpty())
                add(m.capturedStart(), m.capturedEnd(), id, false);
        }
    }
    std::sort(result.spans.begin(), result.spans.end(),
              [](const auto &a, const auto &b) { return a.start < b.start; });
    return applyAliases ? this->applyAliases(result, smali) : result;
}

SourceDocument Project::applyAliases(SourceDocument result, bool smali) const {
    // Append unchanged ranges and replacements once. Replacing every token in a
    // large QString repeatedly shifts the rest of the file for each alias.
    QString rewritten;
    rewritten.reserve(result.text.size());
    int cursor = 0, shift = 0;
    bool changed = false;
    for (auto &span : result.spans) {
        const int originalStart = span.start, originalEnd = span.end;
        span.start += shift;
        span.end += shift;
        QString replacement = aliases_.value(span.id);
        if ((!span.id.contains("->") && span.id.endsWith(';')) ||
            (!smali && !span.id.contains("@local:") && span.id.contains(";-><init>"))) {
            const QString old = classOf(span.id), renamed = renamedClass(old);
            replacement.clear();
            if (old != renamed) {
                replacement = smali ? classId(renamed) : renamed.section('/', -1);
                if (!smali && !QStringView(result.text).mid(originalStart, originalEnd - originalStart).contains('$'))
                    replacement = replacement.section('$', -1);
            }
        }
        if (replacement.isEmpty()) continue;
        rewritten += QStringView(result.text).mid(cursor, originalStart - cursor);
        rewritten += replacement;
        cursor = originalEnd;
        changed = true;
        const int delta = replacement.size() - (originalEnd - originalStart);
        span.end += delta;
        shift += delta;
    }
    if (changed) {
        rewritten += QStringView(result.text).mid(cursor);
        result.text = std::move(rewritten);
    }
    return result;
}

QString Project::canonicalId(const QString &id) const {
    if (id.contains("@local:") || symbols_.contains(id) || !id.contains("->"))
        return id;
    const QString suffix = id.mid(id.indexOf("->"));
    const auto owner = classOf(id);
    QStringList work = parents_.value(owner);
    if (work.isEmpty()) return id;
    QSet<QString> visited{owner};
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
            if (canceled_->load()) return {};
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
void Project::replaceData(const Project &other, bool keepDocuments) {
    if (!keepDocuments)
        documents_ = other.documents_;
    if (!keepDocuments) localIndex_ = other.localIndex_;
    if (!keepDocuments) { mapArchive_ = other.mapArchive_; javaArchive_ = other.javaArchive_; }
    if (!keepDocuments) canceled_ = other.canceled_;
    referenceIndex_ = other.referenceIndex_;
    overrideIndex_ = other.overrideIndex_;
    symbolIndex_ = other.symbolIndex_;
    input_ = other.input_;
    inputs_ = other.inputs_;
    classes_ = other.classes_;
    symbols_ = other.symbols_;
    classNames_ = other.classNames_;
    parents_ = other.parents_;
    aliases_ = other.aliases_;
    aliasIndex_ = other.aliasIndex_;
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
