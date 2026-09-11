#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QFont>
struct AppSettings {
    QString language = "zh_CN", uiFontFamily, editorFontFamily, monoFontFamily;
    int uiFontSize = 0, monoFontSize = 13;
    QFont interfaceFont() const;
    QFont codeFont(bool mono = false) const;
    QString pythonPath, nodePath, indexDirectory;
    int indexCacheGiB = 20;
    int scriptTimeout = 300;
    int hexPreviewKiB = 64;
    int threads = 4, maxTabs = 12, fontSize = 13, cacheMiB = 2048, sourceMiB = 8;
    bool escapeUnicode = false, background = false, wordWrap = false, showMetadata = true;
    bool showNotice = true, mcpEnabled = false;
    int mcpPort = 8650;
    QString mcpTransport = "stdio", mcpHost = "127.0.0.1", cacheMode = "disk";
    bool showMemory = true;
    bool deobfuscate = false, deobfuscateStrings = false, simplifyControlFlow = false;
    bool unflatten = false;
    QString numberFormat = "auto";
    QStringList excluded;
    QString theme = "dark";
    QJsonObject toJson() const;
    static AppSettings fromJson(const QJsonObject &json);
    static AppSettings load();
    void save() const;
};
