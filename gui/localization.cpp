#include "localization.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

QStringList Localization::directories() {
    const auto base = QCoreApplication::applicationDirPath();
    QStringList paths{base + "/languages"};
#ifdef Q_OS_MAC
    paths << base + "/../../../languages"; // Beside the .app, without changing its signature.
#endif
    paths << base + "/../Resources/languages" << base + "/../languages";
    return paths;
}
static QJsonObject catalog(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QList<QPair<QString, QString>> Localization::languages() {
    QList<QPair<QString, QString>> result{{"zh_CN", QString::fromUtf8("简体中文")}};
    QSet<QString> seen{"zh_CN"};
    for (const auto &directory : directories())
        for (const auto &name : QDir(directory).entryList({"*.json"}, QDir::Files, QDir::Name)) {
            const auto data = catalog(directory + '/' + name);
            const auto code = data.value("locale").toString();
            const auto label = data.value("name").toString();
            if (code.isEmpty() || label.isEmpty() || seen.contains(code) || name != code + ".json" ||
                !data.value("messages").isObject()) continue;
            seen.insert(code); result.append({code, label});
        }
    return result;
}
bool Localization::loadLanguage(const QString &code) {
    messages_ = {};
    static const QRegularExpression valid("^[a-z]{2,3}(?:_[A-Za-z0-9]+)*$");
    if (!valid.match(code).hasMatch()) return false;
    if (code == "zh_CN") return true;
    for (const auto &directory : directories()) {
        const auto data = catalog(directory + '/' + code + ".json");
        if (data.value("locale").toString() != code || !data.value("messages").isObject()) continue;
        messages_ = data.value("messages").toObject();
        return true;
    }
    return false;
}
QString Localization::translate(const char *, const char *source, const char *, int) const {
    const auto value = messages_.value(QString::fromUtf8(source)).toString();
    return value.isEmpty() ? QString() : value;
}
