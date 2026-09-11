#include "indexcache.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
namespace {
QByteArray digest(const QString &path, const std::shared_ptr<std::atomic_bool> &cancel, qint64 limit = -1) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Blake2b_256);
    while (!file.atEnd() && (limit < 0 || file.pos() < limit)) {
        if (cancel && cancel->load()) return {};
        const auto data = file.read(limit < 0 ? 1048576 : qMin<qint64>(1048576, limit - file.pos()));
        if (data.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(data);
    }
    return hash.result().toHex();
}
QByteArray stamps(const QStringList &inputs, const QString &engine) {
    QJsonArray values;
    for (const auto &path : inputs + QStringList{engine}) {
        const QFileInfo info(path);
        values.append(QJsonArray{info.absoluteFilePath(), QString::number(info.size()),
                                 QString::number(info.lastModified().toMSecsSinceEpoch())});
    }
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}
QString fingerprint(const AppSettings &settings, const QStringList &inputs, const QString &engine,
                    const std::shared_ptr<std::atomic_bool> &cancel) {
    QCryptographicHash key(QCryptographicHash::Sha256);
    key.addData("garlic-index-4:" GARLIC_GUI_VERSION);
    key.addData(qVersion()); // QString shard hashing may change between Qt versions.
    auto add = [&](const QByteArray &value) { key.addData(QByteArray::number(value.size()) + ':'); key.addData(value); };
    for (const auto &path : inputs) {
        const auto hash = digest(path, cancel); if (hash.isEmpty()) return {};
        add(QFileInfo(path).absoluteFilePath().toUtf8()); add(hash);
    }
    const auto engineHash = digest(engine, cancel); if (engineHash.isEmpty()) return {};
    add(engineHash);
    add(QJsonDocument(QJsonObject{{"deobfuscate", settings.deobfuscate},
        {"excluded", QJsonArray::fromStringList(settings.excluded)}}).toJson(QJsonDocument::Compact));
    return QString::fromLatin1(key.result().toHex());
}
QString epoch(const QString &root) {
    QFile file(root + "/.garlic-index-epoch"); if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}
bool advanceEpoch(const QString &root) {
    QSaveFile file(root + "/.garlic-index-epoch");
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QUuid::createUuid().toByteArray()); return file.commit();
}
QJsonObject manifest(const QString &path) {
    QFile file(path + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool managed(const QString &path) {
    if (QFileInfo(path).isSymLink()) return false;
    if (manifest(path).value("owner") == "garlic-index-cache") return true;
    QFile marker(path + "/.garlic-owner");
    return marker.open(QIODevice::ReadOnly) && marker.size() == 18 && marker.readAll() == "garlic-index-cache";
}
qint64 bytes(const QString &path) {
    qint64 size = 0;
    QDirIterator it(path, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); size += it.fileInfo().size(); }
    return size;
}
// The root lock excludes live publishers, so marked staging directories here
// are leftovers from interrupted processes. Never touch unmarked directories.
void clearStaging(const QString &root) {
    for (const auto &info : QDir(root).entryInfoList({".garlic-stage-*"}, QDir::Dirs | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot)) {
        QFile marker(info.filePath() + "/.garlic-owner");
        if (marker.open(QIODevice::ReadOnly) && marker.readAll() == "garlic-index-cache") {
            marker.close(); QDir(info.filePath()).removeRecursively();
        }
    }
    for (const auto &info : QDir(root).entryInfoList({"source-*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (!managed(info.filePath())) continue;
        for (const auto &path : {info.filePath(), info.filePath() + "/individual"})
            for (const auto &stage : QDir(path).entryInfoList({".source-stage-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks))
                QDir(stage.filePath()).removeRecursively();
    }
}
struct Entry { QString path; qint64 created, size; };
QList<Entry> entries(const QString &root, bool withSize = true) {
    QList<Entry> result;
    for (const auto &info : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (!(info.fileName().startsWith("index-") && info.fileName().size() == 70) &&
            !(info.fileName().startsWith("source-") && info.fileName().size() == 71)) continue;
        const auto m = manifest(info.filePath());
        if (!managed(info.filePath())) continue;
        result.append({info.filePath(), qint64(m.value("created").toDouble()), withSize ? bytes(info.filePath()) : 0});
    }
    std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) { return a.created < b.created; });
    return result;
}
}
bool IndexCache::enabled(const AppSettings &settings) {
    return settings.cacheMode == "disk" && !qEnvironmentVariableIsSet("GARLIC_DISABLE_INDEX_CACHE");
}
QString IndexCache::directory(const AppSettings &settings) {
    return settings.indexDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/garlic/indexes"
        : QDir(settings.indexDirectory).absolutePath();
}
IndexCache::Lookup IndexCache::lookup(const AppSettings &settings, const QStringList &inputs, const QString &engine,
                                     const std::shared_ptr<std::atomic_bool> &cancel, bool rebuild) {
    Lookup result;
    if (!enabled(settings)) return result;
    const auto root = directory(settings);
    if (!QDir().mkpath(root)) return result;
    QLockFile lock(root + "/.garlic-index-lock");
    if (!lock.tryLock(0)) return result; // Never hold up opening on another writer.
    clearStaging(root);
    // No entry for these inputs: don't delay first directory display hashing a
    // large APK. The background writer computes the full content key later.
    bool candidate = rebuild;
    for (const auto &entry : entries(root, false))
        if (manifest(entry.path).value("inputs").toArray() == QJsonArray::fromStringList(inputs)) { candidate = true; break; }
    const auto inputStamps = stamps(inputs, engine);
    if (!candidate) {
        result.ticket = {"pending", epoch(root), inputs, engine, inputStamps}; return result;
    }
    lock.unlock();
    QElapsedTimer timing; timing.start();
    const auto contentKey = fingerprint(settings, inputs, engine, cancel);
    if (contentKey.isEmpty() || !lock.tryLock(0)) return {};
    result.ticket = {contentKey, {}, inputs, engine, inputStamps};
    result.path = root + "/index-" + result.ticket.key;
    if (rebuild) {
        if (!advanceEpoch(root)) return {};
        if (managed(result.path)) QDir(result.path).removeRecursively();
    }
    result.ticket.epoch = epoch(root);
    const auto sources = sourceDirectory(settings, result.ticket);
    if (managed(sources)) result.sources = sources;
    if (rebuild || cancel->load()) return result;
    const auto m = manifest(result.path);
    const auto binary = result.path + "/snapshot.bin";
    if (m.value("owner") != "garlic-index-cache" || m.value("key") != result.ticket.key ||
        QFileInfo(binary).isSymLink() || QFileInfo(binary).size() != qint64(m.value("snapshotBytes").toDouble())) return result;
    for (const auto &value : m.value("files").toArray()) {
        const auto name = value.toString(); const QFileInfo info(result.path + '/' + name);
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || !info.isFile() || info.isSymLink()) return result;
    }
    if (digest(binary, cancel, 4096) != m.value("headerBlake2b256").toString().toLatin1()) return result;
    if (qEnvironmentVariableIsSet("GARLIC_PROFILE_LOAD")) qInfo() << "Cache validation ms" << timing.elapsed();
    QFile file(binary);
    if (file.open(QIODevice::ReadOnly)) result.project = Project::readIndexSnapshot(&file, cancel);
    return result;
}
IndexCache::Ticket IndexCache::resolveTicket(const AppSettings &settings, Ticket ticket,
                                             const std::shared_ptr<std::atomic_bool> &cancel) {
    if (ticket.key == "pending") ticket.key = fingerprint(settings, ticket.inputs, ticket.engine, cancel);
    return ticket;
}
namespace {
QString sourceKey(const AppSettings &settings, const IndexCache::Ticket &ticket) {
    const auto config = QJsonDocument(QJsonObject{{"escapeUnicode", settings.escapeUnicode},
        {"simplifyControlFlow", settings.simplifyControlFlow}, {"unflatten", settings.unflatten}}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(ticket.key.toUtf8() + config, QCryptographicHash::Sha256).toHex());
}
QString sourceBundle(const QString &name, bool smali) {
    return "individual/" + QString::fromLatin1(QCryptographicHash::hash(name.toUtf8() + (smali ? ":smali" : ":java"), QCryptographicHash::Sha256).toHex());
}
bool writeManifest(const QString &path, const QJsonObject &object) {
    QSaveFile out(path + "/manifest.json");
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
}
bool sourceFilesValid(const QString &path, const QJsonObject &m) {
    if (m.value("complete") != true) return false;
    const auto files = m.value("files").toObject();
    if (files.isEmpty()) return false;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (it.key().contains('/') || it.key().contains('\\') || it.key() == "." || it.key() == "..") return false;
        QFileInfo file(path + '/' + it.key());
        if (!file.isFile() || file.isSymLink() || file.size() != qint64(it.value().toDouble())) return false;
    }
    return true;
}
}
QString IndexCache::sourceDirectory(const AppSettings &settings, const Ticket &ticket) {
    if (!enabled(settings) || ticket.key.isEmpty() || ticket.key == "pending") return {};
    return directory(settings) + "/source-" + sourceKey(settings, ticket);
}
QString IndexCache::cachedSource(const QString &directory, const QString &name, bool smali) {
    if (directory.isEmpty()) return {};
    const auto path = directory + '/' + sourceBundle(name, smali);
    const auto m = manifest(path);
    return sourceFilesValid(path, m) ? path + (smali ? "/code.smali" : "/code.java") : QString();
}
QString IndexCache::fullSources(const QString &directory) {
    if (directory.isEmpty()) return {};
    const auto path = directory + "/full";
    const auto m = manifest(path);
    return sourceFilesValid(path, m) ? path : QString();
}
bool IndexCache::storeSources(const AppSettings &settings, const Ticket &original, const QString &workspace,
                              const QString &name, bool smali, const QString &path, bool full,
                              const std::shared_ptr<std::atomic_bool> &cancel, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (!enabled(settings) || original.key.isEmpty() || cancel->load()) return false;
    const auto ticket = resolveTicket(settings, original, cancel);
    if (ticket.key.isEmpty() || stamps(ticket.inputs, ticket.engine) != ticket.stamps) return false;
    QTemporaryDir staging(workspace + "/persistent-source-XXXXXX");
    if (!staging.isValid()) return fail("Cannot stage source cache");
    QJsonObject files;
    auto copy = [&](const QString &from, const QString &relative) {
        if (cancel->load()) return false;
        const auto to = staging.path() + '/' + relative;
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QFile::copy(from, to)) return false;
        if (!relative.contains('/') && (!full || QStringList{".garlic-safe-paths", "java-sources.bin", "java-sources.bin.index", "source-maps.bin", "source-maps.bin.index", "source.java"}.contains(relative)))
            files[relative] = double(QFileInfo(to).size());
        return true;
    };
    if (full) {
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next(); const auto relative = QDir(path).relativeFilePath(it.filePath());
            if (!copy(it.filePath(), relative)) return fail("Cannot preserve full sources");
        }
        // Packed APK outputs contain these four files. Directory outputs have
        // a safe-path marker; individual missing files fall back on demand.
        if (QFileInfo::exists(staging.path() + "/java-sources.bin")) {
            for (const auto &required : {"java-sources.bin", "java-sources.bin.index", "source-maps.bin", "source-maps.bin.index"})
                if (!files.contains(required)) return fail("Incomplete source archive");
        }
        if (files.isEmpty()) return fail("No completed source files");
    } else {
        const auto extension = smali ? ".smali" : ".java";
        if (!copy(path, "code" + QString(extension))) return fail("Cannot preserve source");
        const auto map = !smali && QFileInfo(path).fileName() == "source.java"
            ? Project::sourcePath(QFileInfo(path).absolutePath(), name, ".map.json")
            : path.left(path.size() - QString(extension).size()) + ".map.json";
        if (QFileInfo::exists(map) && !copy(map, "code.map.json")) return fail("Cannot preserve source mapping");
    }
    if (!writeManifest(staging.path(), {{"complete", true}, {"files", files}})) return fail("Cannot finalize sources");
    const auto root = directory(settings), destination = sourceDirectory(settings, ticket);
    const auto incoming = bytes(staging.path());
    const auto limit = qint64(settings.indexCacheGiB) * 1024 * 1024 * 1024;
    if (incoming + 4096 > limit) return fail("Source cache exceeds the storage quota");
    QLockFile lock(root + "/.garlic-index-lock");
    if (!lock.tryLock(30000)) return fail("Index cache is busy");
    if (cancel->load() || epoch(root) != ticket.epoch || stamps(ticket.inputs, ticket.engine) != ticket.stamps) return false;
    clearStaging(root);
    auto saved = entries(root); qint64 used = 0;
    for (const auto &entry : saved) used += entry.size;
    QStorageInfo disk(root);
    for (const auto &entry : saved) {
        disk.refresh();
        if (used + incoming + 4096 <= limit && (disk.bytesAvailable() < 0 || disk.bytesAvailable() >= incoming)) break;
        if (QDir(entry.path).removeRecursively()) used -= entry.size;
    }
    disk.refresh();
    if (used + incoming + 4096 > limit || (disk.bytesAvailable() >= 0 && disk.bytesAvailable() < incoming)) return fail("Insufficient source cache space");
    if (QFileInfo::exists(destination) && !managed(destination)) return fail("Source destination is not managed by Garlic");
    QDir().mkpath(destination);
    if (manifest(destination).value("owner") != "garlic-index-cache" && !writeManifest(destination, {{"owner", "garlic-index-cache"},
        {"key", sourceKey(settings, ticket)}, {"inputs", QJsonArray::fromStringList(ticket.inputs)},
        {"created", double(QDateTime::currentMSecsSinceEpoch())}})) return fail("Cannot register source cache");
    QSaveFile owner(destination + "/.garlic-owner");
    if (!owner.open(QIODevice::WriteOnly) || owner.write("garlic-index-cache") != 18 || !owner.commit()) return fail("Cannot mark source cache");
    const auto bundle = full ? QString("full") : sourceBundle(name, smali);
    const auto target = destination + '/' + bundle;
    QDir().mkpath(QFileInfo(target).absolutePath());
    QTemporaryDir publish(QFileInfo(target).absolutePath() + "/.source-stage-XXXXXX");
    if (!publish.isValid()) return fail("Cannot publish source cache");
    QDirIterator it(staging.path(), QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancel->load()) return false;
        it.next(); const auto to = publish.path() + '/' + QDir(staging.path()).relativeFilePath(it.filePath());
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QFile::copy(it.filePath(), to)) return fail("Cannot publish source file");
    }
    if (QFileInfo::exists(target) && !QDir(target).removeRecursively()) return fail("Cannot replace source cache");
    if (!QDir().rename(publish.path(), target)) return fail("Cannot commit source cache");
    publish.setAutoRemove(false); return true;
}
bool IndexCache::clearSources(const AppSettings &settings, const Ticket &original, QString *error) {
    const auto ticket = resolveTicket(settings, original, std::make_shared<std::atomic_bool>(false));
    const auto path = sourceDirectory(settings, ticket);
    if (path.isEmpty() || !QFileInfo::exists(path)) return true;
    QLockFile lock(directory(settings) + "/.garlic-index-lock");
    if (!lock.tryLock(30000)) { if (error) *error = "Source cache is busy"; return false; }
    if (!managed(path)) return false;
    return QDir(path).removeRecursively();
}
bool IndexCache::store(const AppSettings &settings, const Ticket &ticket, const std::shared_ptr<Project> &project,
                       const QStringList &jsonl, const QString &workspace, QString *error, qint64 quotaBytes) {
    auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    const auto cancel = project->cancellationToken();
    if (!enabled(settings) || ticket.key.isEmpty() || cancel->load()) return false;
    if (stamps(ticket.inputs, ticket.engine) != ticket.stamps) return fail("Input changed while indexing");
    const auto contentKey = ticket.key == "pending" ? fingerprint(settings, ticket.inputs, ticket.engine, cancel) : ticket.key;
    if (contentKey.isEmpty()) return false;
    QTemporaryDir staging(workspace + "/persistent-index-XXXXXX");
    if (!staging.isValid()) return fail("Cannot create index staging directory");
    const auto binary = staging.path() + "/snapshot.bin";
    QFile snapshot(binary);
    if (!snapshot.open(QIODevice::WriteOnly) || !project->writeIndexSnapshot(&snapshot)) return fail("Index snapshot write failed");
    snapshot.close();
    QStringList names{"snapshot.bin"};
    for (int i = 0; i < jsonl.size(); ++i) {
        if (cancel->load()) return false;
        const auto name = QString("metadata-%1.jsonl").arg(i);
        if (!QFile::copy(jsonl[i], staging.path() + '/' + name)) return fail("Cannot preserve index JSONL");
        names.append(name);
    }
    const auto hash = digest(binary, cancel, 4096); if (hash.isEmpty()) return false;
    QSaveFile info(staging.path() + "/manifest.json");
    if (!info.open(QIODevice::WriteOnly)) return fail(info.errorString());
    info.write(QJsonDocument(QJsonObject{{"owner", "garlic-index-cache"}, {"key", contentKey},
        {"created", double(QDateTime::currentMSecsSinceEpoch())}, {"inputs", QJsonArray::fromStringList(project->inputs())},
        {"headerBlake2b256", QString::fromLatin1(hash)}, {"snapshotBytes", double(QFileInfo(binary).size())},
        {"files", QJsonArray::fromStringList(names)}}).toJson());
    if (!info.commit()) return fail(info.errorString());
    names.append("manifest.json");
    const auto incoming = bytes(staging.path()) + 18, limit = quotaBytes >= 0 ? quotaBytes : qint64(settings.indexCacheGiB) * 1024 * 1024 * 1024;
    if (incoming > limit) return fail("This index exceeds the configured storage quota");
    const auto root = directory(settings);
    QLockFile lock(root + "/.garlic-index-lock");
    if (!lock.tryLock(1000)) return fail("Index cache is busy");
    if (stamps(ticket.inputs, ticket.engine) != ticket.stamps) return fail("Input changed while saving index");
    if (cancel->load() || epoch(root) != ticket.epoch) return false;
    clearStaging(root);
    auto saved = entries(root); qint64 used = 0;
    for (const auto &entry : saved) used += entry.size;
    QStorageInfo disk(root);
    for (const auto &entry : saved) {
        disk.refresh();
        if (used + incoming <= limit && (disk.bytesAvailable() < 0 || disk.bytesAvailable() >= incoming)) break;
        if (QDir(entry.path).removeRecursively()) used -= entry.size;
    }
    disk.refresh();
    if (used + incoming > limit || (disk.bytesAvailable() >= 0 && disk.bytesAvailable() < incoming)) return fail("Insufficient index cache space");
    QTemporaryDir publish(root + "/.garlic-stage-XXXXXX");
    if (!publish.isValid()) return fail("Cannot publish index cache");
    QFile marker(publish.path() + "/.garlic-owner");
    if (!marker.open(QIODevice::WriteOnly) || marker.write("garlic-index-cache") != 18) return fail("Cannot mark index staging directory");
    marker.close();
    for (const auto &name : names) {
        if (cancel->load()) return false;
        if (!QFile::copy(staging.path() + '/' + name, publish.path() + '/' + name)) return fail("Index cache copy failed");
    }
    const auto destination = root + "/index-" + contentKey;
    if (QFileInfo::exists(destination) && !managed(destination)) return fail("Index destination is not managed by Garlic");
    if (QFileInfo::exists(destination) && !QDir(destination).removeRecursively()) return fail("Cannot replace old index cache");
    if (!QDir().rename(publish.path(), destination)) return fail("Index cache publication failed");
    publish.setAutoRemove(false); return true;
}
bool IndexCache::clear(const AppSettings &settings, QString *error) {
    const auto root = directory(settings); if (!QDir().mkpath(root)) return false;
    QLockFile lock(root + "/.garlic-index-lock");
    if (!lock.tryLock(30000)) { if (error) *error = "Index cache is busy"; return false; }
    if (!advanceEpoch(root)) return false;
    clearStaging(root);
    bool ok = true;
    for (const auto &entry : entries(root)) if (!QDir(entry.path).removeRecursively()) ok = false;
    if (!ok && error) *error = "Some index files could not be removed";
    return ok;
}
QJsonObject IndexCache::stats(const AppSettings &settings) {
    qint64 size = 0; const auto saved = entries(directory(settings));
    for (const auto &entry : saved) size += entry.size;
    int indexes = 0, sources = 0;
    for (const auto &entry : saved) if (QFileInfo(entry.path).fileName().startsWith("source-")) ++sources; else ++indexes;
    return {{"directory", directory(settings)}, {"entries", indexes}, {"sourceEntries", sources}, {"bytes", double(size)},
        {"limit", double(qint64(settings.indexCacheGiB) * 1024 * 1024 * 1024)}};
}
