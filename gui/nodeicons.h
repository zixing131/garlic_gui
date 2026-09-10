#pragma once
#include <QIcon>
#include <QString>

namespace NodeIcons {
QIcon resource(const QString &path);
// Keep these rules aligned with jadx's JClass / JField / MethodRenderHelper.
QString baseName(const QString &kind, quint32 flags = 0, bool constructor = false);
QIcon icon(const QString &kind, quint32 flags = 0, bool constructor = false);
} // namespace NodeIcons
