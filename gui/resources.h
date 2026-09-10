#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QMap>
namespace Resources {
QJsonObject inspect(const QString &path);
QByteArray read(const QString &path, const QString &entry, qint64 limit, QString *error);
QString decodeXml(const QByteArray &bytes);
QString describeTable(const QByteArray &bytes, QMap<QString, QString> *files = nullptr);
QString signature(const QString &path);
QString hex(const QByteArray &bytes);
}
