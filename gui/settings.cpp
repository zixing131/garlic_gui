#include "settings.h"
#include <QJsonArray>
#include <QJsonDocument>
QJsonObject AppSettings::toJson() const {
    return {{"threads", threads},
            {"maxTabs", maxTabs},
            {"fontSize", fontSize},
            {"cacheMiB", cacheMiB},
            {"sourceMiB", sourceMiB},
            {"escapeUnicode", escapeUnicode},
            {"background", background},
            {"wordWrap", wordWrap},
            {"showMetadata", showMetadata},
            {"showNotice", showNotice},
            {"mcpEnabled", mcpEnabled},
            {"mcpPort", mcpPort},
            {"mcpTransport", mcpTransport},
            {"mcpHost", mcpHost},
            {"cacheMode", cacheMode},
            {"showMemory", showMemory},
            {"excluded", QJsonArray::fromStringList(excluded)},
            {"theme", theme}};
}
AppSettings AppSettings::fromJson(const QJsonObject &j) {
    AppSettings s;
    s.threads = qBound(1, j.value("threads").toInt(s.threads), 16);
    s.maxTabs = qBound(1, j.value("maxTabs").toInt(s.maxTabs), 64);
    s.fontSize = qBound(8, j.value("fontSize").toInt(s.fontSize), 32);
    s.cacheMiB = qBound(16, j.value("cacheMiB").toInt(s.cacheMiB), 4096);
    s.sourceMiB = qBound(1, j.value("sourceMiB").toInt(s.sourceMiB), 64);
    s.escapeUnicode = j.value("escapeUnicode").toBool(s.escapeUnicode);
    s.background = j.value("background").toBool(s.background);
    s.wordWrap = j.value("wordWrap").toBool(s.wordWrap);
    s.showMetadata = j.value("showMetadata").toBool(s.showMetadata);
    s.showNotice = j.value("showNotice").toBool(s.showNotice);
    s.mcpPort = qBound(1024, j.value("mcpPort").toInt(s.mcpPort), 65535);
    s.mcpTransport = j.value("mcpTransport").toString() == "http" ? "http" : "stdio";
    s.mcpHost = j.value("mcpHost").toString("127.0.0.1").trimmed();
    s.cacheMode = j.value("cacheMode").toString() == "memory" ? "memory" : "disk";
    s.showMemory = j.value("showMemory").toBool(false);
    s.mcpEnabled = j.value("mcpEnabled").toBool(s.mcpEnabled);
    for (const auto &v : j.value("excluded").toArray()) {
        const auto p = v.toString().trimmed();
        if (!p.isEmpty())
            s.excluded << p;
    }
    s.theme = j.value("theme").toString("dark");
    if (s.theme != "light")
        s.theme = "dark";
    return s;
}
AppSettings AppSettings::load() {
    return fromJson(
        QJsonDocument::fromJson(QSettings().value("preferences").toByteArray()).object());
}
void AppSettings::save() const {
    QSettings().setValue("preferences", QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}
