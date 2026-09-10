#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
struct AppSettings {
    int threads = 4, maxTabs = 12, fontSize = 13, cacheMiB = 2048, sourceMiB = 8;
    bool escapeUnicode = false, background = false, wordWrap = false, showMetadata = true;
    bool showNotice = true, mcpEnabled = false;
    int mcpPort = 8650;
    QString mcpTransport = "stdio", mcpHost = "127.0.0.1", cacheMode = "disk";
    bool showMemory = false;
    bool deobfuscate = false, simplifyControlFlow = false;
    bool unflatten = false;
    QStringList excluded;
    QString theme = "dark";
    QJsonObject toJson() const;
    static AppSettings fromJson(const QJsonObject &json);
    static AppSettings load();
    void save() const;
};
