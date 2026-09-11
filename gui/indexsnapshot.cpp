#include "project.h"
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QDebug>
#include <QDataStream>
#include <QtEndian>

// JSON payloads use the optimized Qt JSON parser; query tables are stored directly so reopening does
// not repeat addClass(), automatic renaming or reference discovery.
bool Project::writeIndexSnapshot(QIODevice *device) const {
    QDataStream out(device); out.setVersion(QDataStream::Qt_6_5);
    out << QByteArray("GARLIC-INDEX-1") << input_ << inputs_;
    auto objects = [&](const QHash<QString, QJsonObject> &map) {
        out << quint32(map.size());
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (canceled_->load() || out.status() != QDataStream::Ok) return false;
            out << it.key() << QJsonDocument(it.value()).toJson(QJsonDocument::Compact);
        }
        return true;
    };
    if (!objects(classes_) || !objects(symbols_)) return false;
    out << classNames_ << parents_ << aliases_;
    std::lock_guard<std::mutex> guard(referenceIndex_->lock);
    if (!referenceIndex_->ready || canceled_->load()) return false;
    out << quint32(referenceIndex_->groups.size());
    for (const auto &group : referenceIndex_->groups) out << group.owner << group.from;
    out << referenceIndex_->targets << quint32(referenceIndex_->positions.size());
    for (const auto &positions : referenceIndex_->positions) {
        if (canceled_->load() || out.status() != QDataStream::Ok) return false;
        out << quint32(positions.size());
        QByteArray bytes(qsizetype(positions.size()) * 8, Qt::Uninitialized);
        auto data = reinterpret_cast<uchar *>(bytes.data());
        for (const auto &position : positions) {
            qToBigEndian<qint32>(position.group, data); qToBigEndian<qint32>(position.index, data + 4); data += 8;
        }
        if (out.writeRawData(bytes.constData(), bytes.size()) != bytes.size()) return false;
    }
    return !canceled_->load() && out.status() == QDataStream::Ok;
}
std::shared_ptr<Project> Project::readIndexSnapshot(QIODevice *device, const std::shared_ptr<std::atomic_bool> &canceled) {
    QElapsedTimer timing; timing.start();
    QDataStream in(device); in.setVersion(QDataStream::Qt_6_5);
    QByteArray magic; in >> magic;
    if (magic != "GARLIC-INDEX-1") return {};
    auto project = std::make_shared<Project>(); project->setCancellationToken(canceled);
    in >> project->input_ >> project->inputs_;
    auto count = [&]() -> quint32 {
        quint32 n = 0; in >> n;
        if (n > 100000000 || qint64(n) > device->bytesAvailable()) in.setStatus(QDataStream::ReadCorruptData);
        return in.status() == QDataStream::Ok ? n : 0;
    };
    auto objects = [&](QHash<QString, QJsonObject> &map) {
        const auto n = count();
        if (in.status() != QDataStream::Ok) return false;
        map.reserve(n);
        for (quint32 i = 0; i < n; ++i) {
            if (canceled->load()) return false;
            QString key; QByteArray bytes; in >> key >> bytes;
            QJsonParseError error; const auto value = QJsonDocument::fromJson(bytes, &error);
            if (in.status() != QDataStream::Ok || error.error != QJsonParseError::NoError || !value.isObject()) return false;
            map.insert(key, value.object());
        }
        return true;
    };
    if (!objects(project->classes_)) return {};
    const auto classesMs = timing.elapsed();
    if (!objects(project->symbols_)) return {};
    const auto symbolsMs = timing.elapsed();
    in >> project->classNames_ >> project->parents_ >> project->aliases_;
    auto index = project->referenceIndex_;
    const auto groups = count();
    for (quint32 i = 0; i < groups; ++i) {
        if (canceled->load()) return {};
        ReferenceGroup group; in >> group.owner >> group.from; index->groups.append(std::move(group));
    }
    in >> index->targets;
    const auto targets = count();
    index->positions.resize(targets);
    for (quint32 i = 0; i < targets; ++i) {
        if (canceled->load()) return {};
        const auto n = count();
        if (qint64(n) * 8 > device->bytesAvailable()) return {};
        QByteArray bytes(qsizetype(n) * 8, Qt::Uninitialized);
        if (in.readRawData(bytes.data(), bytes.size()) != bytes.size()) return {};
        auto &positions = index->positions[i]; positions.reserve(n);
        const auto data = reinterpret_cast<const uchar *>(bytes.constData());
        for (quint32 j = 0; j < n; ++j) {
            const auto group = qFromBigEndian<qint32>(data + qsizetype(j) * 8);
            const auto row = qFromBigEndian<qint32>(data + qsizetype(j) * 8 + 4);
            if (group < 0 || quint32(group) >= groups || row < 0) return {};
            positions.append({group, row});
        }
    }
    for (auto it = index->targets.cbegin(); it != index->targets.cend(); ++it)
        if (it.value() < 0 || quint32(it.value()) >= targets) return {};
    if (canceled->load() || in.status() != QDataStream::Ok || !device->atEnd()) return {};
    if (qEnvironmentVariableIsSet("GARLIC_PROFILE_LOAD")) qInfo() << "Snapshot classes / symbols / references ms" << classesMs << symbolsMs - classesMs << timing.elapsed() - symbolsMs;
    index->ready = true;
    project->symbols(); project->aliasVersion();
    return canceled->load() ? nullptr : project;
}
