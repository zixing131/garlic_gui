#pragma once
#include <QtCore>
namespace MemoryUsage {
struct Sample {
    quint64 current = 0, available = 0, peak = 0;
};
Sample read(const QList<qint64> &children = {});
} // namespace MemoryUsage
