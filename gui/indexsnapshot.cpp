#include "project.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtEndian>

namespace {
constexpr qint64 HeaderSize = 4096;
constexpr int Shards = ProjectMap::ShardCount;
struct Block { qint64 offset = 0, size = 0; QByteArray hash; };
// Hash as the stream is written: no giant temporary serialization buffer.
class BlockWriter : public QIODevice {
public:
    explicit BlockWriter(QIODevice *target) : target_(target), hash_(QCryptographicHash::Blake2b_256) { open(WriteOnly); }
    QByteArray hash() const { return hash_.result(); }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override {
        const auto written = target_->write(data, size);
        if (written > 0) hash_.addData(QByteArrayView(data, written));
        return written;
    }
private:
    QIODevice *target_;
    QCryptographicHash hash_;
};
bool verified(QByteArrayView bytes, const QByteArray &expected, const std::shared_ptr<std::atomic_bool> &cancel) {
    QCryptographicHash hash(QCryptographicHash::Blake2b_256);
    for (qsizetype offset = 0; offset < bytes.size(); offset += 1048576) {
        if (cancel->load()) return false;
        hash.addData(bytes.sliced(offset, qMin<qsizetype>(1048576, bytes.size() - offset)));
    }
    return hash.result() == expected;
}
}

bool Project::writeIndexSnapshot(QIODevice *device) const {
    if (device->isSequential() || !device->seek(0) || device->write(QByteArray(HeaderSize, '\0')) != HeaderSize) return false;
    QList<Block> blocks;
    auto block = [&](auto write) {
        Block entry; entry.offset = device->pos();
        BlockWriter stream(device); QDataStream out(&stream); out.setVersion(QDataStream::Qt_6_5);
        if (!write(out) || out.status() != QDataStream::Ok || canceled_->load()) return false;
        entry.size = device->pos() - entry.offset; entry.hash = stream.hash(); blocks.append(entry); return true;
    };
    for (const auto *map : {&classes_, &symbols_}) {
        for (int shard = 0; shard < Shards; ++shard) {
            auto it = map->shard(shard).cbegin();
            const auto count = map->shard(shard).size();
            if (!block([&](QDataStream &out) {
                out << quint32(count);
                for (qsizetype n = 0; n < count; ++n, ++it) {
                    if (canceled_->load()) return false;
                    out << it.key() << QJsonDocument(it.value()).toJson(QJsonDocument::Compact);
                }
                return true;
            })) return false;
        }
    }
    std::lock_guard<std::mutex> guard(referenceIndex_->lock);
    if (!referenceIndex_->ready) return false;
    if (!block([&](QDataStream &out) {
        out << input_ << inputs_ << classNames_ << parents_ << aliases_ << deobfuscateLocals_ << aliasVersion();
        out << quint32(referenceIndex_->groups.size());
        for (const auto &group : referenceIndex_->groups) out << group.owner << group.from;
        out << referenceIndex_->targets << quint32(referenceIndex_->positions.size());
        for (const auto &positions : referenceIndex_->positions) {
            if (canceled_->load()) return false;
            out << quint32(positions.size());
            QByteArray bytes(qsizetype(positions.size()) * 8, Qt::Uninitialized);
            auto data = reinterpret_cast<uchar *>(bytes.data());
            for (const auto &position : positions) {
                qToBigEndian<qint32>(position.group, data); qToBigEndian<qint32>(position.index, data + 4); data += 8;
            }
            if (out.writeRawData(bytes.constData(), bytes.size()) != bytes.size()) return false;
        }
        return true;
    })) return false;
    QByteArray header; QDataStream out(&header, QIODevice::WriteOnly); out.setVersion(QDataStream::Qt_6_5);
    out << QByteArray("GARLIC-INDEX-5") << quint32(blocks.size());
    for (const auto &entry : blocks) out << entry.offset << entry.size << entry.hash;
    if (header.size() > HeaderSize) return false;
    header.append(QByteArray(HeaderSize - header.size(), '\0'));
    return device->seek(0) && device->write(header) == HeaderSize;
}

std::shared_ptr<Project> Project::readIndexSnapshot(QIODevice *device, const std::shared_ptr<std::atomic_bool> &canceled) {
    QElapsedTimer timing; timing.start();
    if (!device->seek(0) || device->size() < HeaderSize) return {};
    QByteArray header = device->read(HeaderSize); QDataStream table(header); table.setVersion(QDataStream::Qt_6_5);
    QByteArray magic; quint32 count = 0; table >> magic >> count;
    if (magic != "GARLIC-INDEX-5" || count != Shards * 2 + 1) return {};
    QList<Block> blocks; qint64 end = HeaderSize;
    for (quint32 i = 0; i < count; ++i) {
        Block b; table >> b.offset >> b.size >> b.hash;
        if (table.status() != QDataStream::Ok || b.offset != end || b.size < 4 || b.size > device->size() - end || b.hash.size() != 32) return {};
        end += b.size; blocks.append(b);
    }
    if (end != device->size()) return {};
    // Each worker owns its stream. Mapping shares file pages without allocating
    // another multi-gigabyte copy of the snapshot or sharing QFile seek state.
    auto file = qobject_cast<QFile *>(device);
    uchar *mapped = file ? file->map(0, file->size()) : nullptr;
    if (file && !mapped) return {}; // Rebuild instead of allocating a second full snapshot on mapping failure.
    struct Unmap { QFile *file; uchar *data; ~Unmap() { if (data) file->unmap(data); } } mapping{file, mapped};
    QByteArray fallback;
    if (!mapped) { if (!device->seek(0)) return {}; fallback = device->readAll(); if (fallback.size() != device->size()) return {}; }
    const char *data = mapped ? reinterpret_cast<const char *>(mapped) : fallback.constData();
    auto project = std::make_shared<Project>(); project->setCancellationToken(canceled);
    QHash<QString, QJsonObject> maps[Shards * 2];
    QThreadPool pool;
    const int requested = qEnvironmentVariableIntValue("GARLIC_CACHE_READ_THREADS");
    pool.setMaxThreadCount(requested > 0 ? qBound(1, requested, 8) : qMax(1, qMin(8, QThread::idealThreadCount())));
    QList<QFuture<bool>> tasks;
    for (int task = 0; task < blocks.size(); ++task) {
        const int b = task == 0 ? Shards * 2 : task - 1; // Start the reference table immediately.
        tasks.append(QtConcurrent::run(&pool, [&, b] {
        const auto &entry = blocks[b];
        const auto bytes = QByteArray::fromRawData(data + entry.offset, entry.size);
        if (!verified(bytes, entry.hash, canceled)) return false;
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
        QDataStream in(&buffer); in.setVersion(QDataStream::Qt_6_5);
        auto count = [&]() {
            quint32 n = 0; in >> n;
            if (n > 100000000 || qint64(n) > buffer.bytesAvailable()) in.setStatus(QDataStream::ReadCorruptData);
            return in.status() == QDataStream::Ok ? n : quint32(0);
        };
        if (b < Shards * 2) {
            const auto n = count(); auto &map = maps[b]; map.reserve(n);
            for (quint32 i = 0; i < n; ++i) {
                if (canceled->load()) return false;
                QString key; QByteArray json; in >> key >> json;
                QJsonParseError error; const auto value = QJsonDocument::fromJson(json, &error);
                if (in.status() != QDataStream::Ok || error.error != QJsonParseError::NoError || !value.isObject()) return false;
                if (ProjectMap::shardFor(key) != b % Shards) return false;
                map.insert(key, value.object());
            }
            if (quint32(map.size()) != n) return false;
        } else {
            in >> project->input_ >> project->inputs_ >> project->classNames_ >> project->parents_ >> project->aliases_ >> project->deobfuscateLocals_ >> project->aliasIndex_->version;
            if (project->aliasIndex_->version.size() != 64) return false;
            auto index = project->referenceIndex_;
            const auto groups = count(); index->groups.reserve(groups);
            for (quint32 i = 0; i < groups; ++i) {
                if (canceled->load()) return false;
                ReferenceGroup group; in >> group.owner >> group.from; index->groups.append(std::move(group));
            }
            in >> index->targets;
            const auto targets = count(); index->positions.resize(targets);
            for (quint32 i = 0; i < targets; ++i) {
                if (canceled->load()) return false;
                const auto n = count();
                if (qint64(n) * 8 > buffer.bytesAvailable()) return false;
                const auto raw = reinterpret_cast<const uchar *>(bytes.constData() + buffer.pos());
                if (!buffer.seek(buffer.pos() + qint64(n) * 8)) return false;
                auto &positions = index->positions[i]; positions.reserve(n);
                for (quint32 j = 0; j < n; ++j) {
                    const auto group = qFromBigEndian<qint32>(raw + qsizetype(j) * 8);
                    const auto row = qFromBigEndian<qint32>(raw + qsizetype(j) * 8 + 4);
                    if (group < 0 || quint32(group) >= groups || row < 0) return false;
                    positions.append({group, row});
                }
            }
            for (auto it = index->targets.cbegin(); it != index->targets.cend(); ++it)
                if (it.value() < 0 || quint32(it.value()) >= targets) return false;
        }
        return !canceled->load() && in.status() == QDataStream::Ok && buffer.atEnd();
        }));
    }
    // Member shards are smaller than the reference/class blocks. Prepare the
    // symbol query array while those larger blocks are still being restored.
    bool valid = true;
    for (int b = 0; b < Shards; ++b) if (!tasks[1 + Shards + b].result()) valid = false;
    if (valid && !canceled->load()) {
        for (int b = 0; b < Shards; ++b) project->symbols_.adopt(b, std::move(maps[Shards + b]));
        project->symbols();
    }
    for (int b = 0; b <= Shards; ++b) if (!tasks[b].result()) valid = false;
    if (!valid || canceled->load()) return {};
    const auto parsedMs = timing.elapsed();
    for (int b = 0; b < Shards; ++b) project->classes_.adopt(b, std::move(maps[b]));
    project->referenceIndex_->ready = true;
    project->aliasVersion();
    if (qEnvironmentVariableIsSet("GARLIC_PROFILE_LOAD")) qInfo() << "Snapshot parallel restore / finish ms" << parsedMs << timing.elapsed() - parsedMs;
    return canceled->load() ? nullptr : project;
}
