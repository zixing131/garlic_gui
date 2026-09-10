#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
struct AppSettings {
    int threads = 4, maxTabs = 12, fontSize = 13, cacheMiB = 256, sourceMiB = 8;
    bool escapeUnicode = false, background = false, wordWrap = false, showMetadata = true;
    bool showNotice = true, mcpEnabled = false;
    QStringList excluded;
    QString theme = "dark";
    QJsonObject toJson() const;
    static AppSettings fromJson(const QJsonObject &json);
    static AppSettings load();
    void save() const;
};
