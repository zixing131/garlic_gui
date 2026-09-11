#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <atomic>
#include <memory>
namespace Resources {
QJsonObject inspect(const QString &path, std::shared_ptr<std::atomic_bool> canceled = {});
QString materialize(const QString &path, const QString &entry, QString *error,
                    std::shared_ptr<std::atomic_bool> canceled = {});
QByteArray read(const QString &path, const QString &entry, qint64 limit, QString *error);
QString decodeXml(const QByteArray &bytes);
QString configurationName(const QByteArray &config);
QString describeTable(const QByteArray &bytes, QMap<QString, QString> *files = nullptr);
QStringList launcherActivities(const QString &manifest);
QString signature(const QString &path);
QString hex(const QByteArray &bytes);
}
