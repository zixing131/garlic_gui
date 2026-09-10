#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
namespace Resources {
QJsonObject inspect(const QString &path);
QByteArray read(const QString &path, const QString &entry, qint64 limit, QString *error);
QString decodeXml(const QByteArray &bytes);
QString describeTable(const QByteArray &bytes);
QString signature(const QString &path);
QString hex(const QByteArray &bytes);
}
