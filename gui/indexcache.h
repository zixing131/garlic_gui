#pragma once
#include "project.h"
#include "settings.h"
namespace IndexCache {
struct Ticket { QString key, epoch; QStringList inputs; QString engine; QByteArray stamps; };
struct Lookup { Ticket ticket; std::shared_ptr<Project> project; QString path; };
bool enabled(const AppSettings &settings);
QString directory(const AppSettings &settings);
Lookup lookup(const AppSettings &settings, const QStringList &inputs, const QString &engine,
              const std::shared_ptr<std::atomic_bool> &canceled, bool rebuild = false);
bool store(const AppSettings &settings, const Ticket &ticket, const std::shared_ptr<Project> &project,
           const QStringList &jsonl, const QString &workspace, QString *error = nullptr, qint64 quotaBytes = -1);
bool clear(const AppSettings &settings, QString *error = nullptr);
QJsonObject stats(const AppSettings &settings);
}
