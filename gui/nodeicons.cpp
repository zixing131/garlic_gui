#include "nodeicons.h"
#include <QHash>
#include <QPainter>

// Explicitly reference the resource initializer so static-library linking retains it.
static void initNodeResources() { Q_INIT_RESOURCE(jadx_icons); }

namespace NodeIcons {
QString baseName(const QString &kind, quint32 flags, bool constructor) {
    if (kind.startsWith("tool") || kind.startsWith("res"))
        return kind;
    if (kind == "package")
        return "package";
    if (kind == "method") {
        QString name = "method";
        if (flags & 0x400)
            name = "abstractMethod";
        if (constructor || (flags & 0x10000))
            name = "constructorMethod";
        if (flags & 1)
            name = "publicMethod";
        if (flags & 2)
            name = "privateMethod";
        if (flags & 4)
            name = "protectedMethod";
        if (flags & 0x20)
            name = "methodReference";
        return name;
    }
    if (kind == "field") {
        if (flags & 1)
            return "publicField";
        if (flags & 2)
            return "privateField";
        if (flags & 4)
            return "protectedField";
        return "field";
    }
    if (kind == "enum" || (flags & 0x4000))
        return "enum";
    if (kind == "annotation" || (flags & 0x2000))
        return "annotationtype";
    if (kind == "interface" || (flags & 0x200))
        return "interface";
    if (kind == "abstract" || (flags & 0x400))
        return "abstractClass";
    if (flags & 4)
        return "protectedClass";
    if (flags & 2)
        return "privateClass";
    if (flags & 1)
        return "publicClass";
    return "class";
}

QIcon icon(const QString &kind, quint32 flags, bool constructor) {
    static const bool initialized = [] {
        initNodeResources();
        return true;
    }();
    Q_UNUSED(initialized);
    static QHash<QString, QIcon> cache;
    const QString base = baseName(kind, flags, constructor);
    const quint32 marks = kind == "method" || kind == "field" ? flags & 0x18 : 0;
    const QString key = base + QString::number(marks);
    if (cache.contains(key))
        return cache.value(key);
    QIcon result;
    for (int scale = 1; scale <= 4; ++scale) {
        auto load = [scale](const QString &name) {
            return QPixmap(QString(":/jadx/png/%1@%2x.png").arg(name).arg(scale));
        };
        QPixmap pixmap = load(base);
        {
            QPainter painter(&pixmap);
            // Both upstream overlays have the same 16x16 canvas as the base icon.
            if (marks & 0x10)
                painter.drawPixmap(0, 0, load("finalMark"));
            if (marks & 8)
                painter.drawPixmap(0, 0, load("staticMark"));
        }
        pixmap.setDevicePixelRatio(scale);
        result.addPixmap(pixmap);
    }
    cache.insert(key, result);
    return result;
}
QIcon resource(const QString &path) {
    const QString name = path.section('/', -1).toLower(), ext = name.section('.', -1);
    QString iconName = "unknown";
    if (name == "androidmanifest.xml")
        iconName = "manifest";
    else if (ext == "arsc")
        iconName = "resourceBundle";
    else if (ext == "xml")
        iconName = "xml";
    else if (QStringList{"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "ico", "avif"}.contains(
                 ext))
        iconName = "ImagesFileType";
    else if (QStringList{"dex", "class", "java", "smali"}.contains(ext))
        iconName = "java";
    else if (QStringList{"so", "dll", "dylib", "exe"}.contains(ext))
        iconName = "binaryFile";
    else if (ext == "apk")
        iconName = "archiveApk";
    else if (QStringList{"zip", "jar", "war", "gz", "apks", "xapk"}.contains(ext))
        iconName = "archive";
    else if (QStringList{"mp3", "wav", "ogg", "flac", "m4a"}.contains(ext))
        iconName = "audioFile";
    else if (QStringList{"mp4", "webm", "mkv", "avi"}.contains(ext))
        iconName = "videoFile";
    else if (QStringList{"ttf", "otf", "woff", "woff2"}.contains(ext))
        iconName = "fontFile";
    else if (ext == "json")
        iconName = "json";
    else if (ext == "html" || ext == "htm")
        iconName = "html";
    else if (QStringList{"txt", "properties", "md", "js", "css", "yaml", "yml", "csv", "log"}
                 .contains(ext))
        iconName = "text";
    return icon("res" + iconName);
}
} // namespace NodeIcons
