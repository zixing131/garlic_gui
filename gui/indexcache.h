#pragma once
#include "project.h"
#include "settings.h"
namespace IndexCache {
struct Ticket { QString key, epoch; QStringList inputs; QString engine; QByteArray stamps; };
struct Lookup { Ticket ticket; std::shared_ptr<Project> project; QString path; QString sources; };
bool enabled(const AppSettings &settings);
QString directory(const AppSettings &settings);
Lookup lookup(const AppSettings &settings, const QStringList &inputs, const QString &engine,
              const std::shared_ptr<std::atomic_bool> &canceled, bool rebuild = false);
Ticket resolveTicket(const AppSettings &settings, Ticket ticket, const std::shared_ptr<std::atomic_bool> &cancel);
QString sourceDirectory(const AppSettings &settings, const Ticket &ticket);
QString cachedSource(const QString &directory, const QString &name, bool smali);
QString fullSources(const QString &directory);
bool storeSources(const AppSettings &settings, const Ticket &ticket, const QString &workspace,
                  const QString &name, bool smali, const QString &path, bool full,
                  const std::shared_ptr<std::atomic_bool> &cancel, QString *error = nullptr);
bool clearSources(const AppSettings &settings, const Ticket &ticket, QString *error = nullptr);
bool store(const AppSettings &settings, const Ticket &ticket, const std::shared_ptr<Project> &project,
           const QStringList &jsonl, const QString &workspace, QString *error = nullptr, qint64 quotaBytes = -1);
bool clear(const AppSettings &settings, QString *error = nullptr);
QJsonObject stats(const AppSettings &settings);
}
