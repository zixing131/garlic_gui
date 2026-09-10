#pragma once
#include <QFile>
#include <QString>

namespace ElfSummary {
inline QString read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return file.errorString();
    auto header = file.read(64);
    if (header.size() < 52 || !header.startsWith(QByteArray::fromHex("7f454c46")))
        return QObject::tr("不是 ELF 文件；可尝试 Ghidra 后端识别其他 Native 格式。");
    bool wide = header[4] == 2, little = header[5] == 1;
    if ((header[4] != 1 && header[4] != 2) || (header[5] != 1 && header[5] != 2) || (wide && header.size() < 64))
        return QObject::tr("无效 ELF 头。");
    auto number = [little](const QByteArray &data, int off, int len) -> quint64 {
        if (off < 0 || off + len > data.size()) return 0;
        quint64 n = 0;
        for (int i = 0; i < len; ++i) n |= quint64(quint8(data[off + i])) << (8 * (little ? i : len - i - 1));
        return n;
    };
    auto machine = number(header, 18, 2);
    QString arch = machine == 183 ? "AArch64" : machine == 40 ? "ARM" : machine == 62 ? "x86-64" : machine == 3 ? "x86" : QString::number(machine);
    QString result = QObject::tr("ELF %1 位 · %2 · %3\n文件大小：%4 字节\n类型：%5\n入口：0x%6\n")
        .arg(wide ? 64 : 32).arg(arch, little ? "Little endian" : "Big endian")
        .arg(file.size()).arg(number(header, 16, 2)).arg(number(header, 24, wide ? 8 : 4), 0, 16);
    quint64 offset = number(header, wide ? 40 : 32, wide ? 8 : 4);
    quint64 size = number(header, wide ? 58 : 46, 2), count = number(header, wide ? 60 : 48, 2);
    quint64 namesIndex = number(header, wide ? 62 : 50, 2);
    if (!offset) return result + "\n无节表。\n";
    if (size < (wide ? 64u : 40u) || size > 4096 || offset > quint64(file.size()) || count > 8192 ||
        count * size > quint64(file.size()) - offset) return result + "\n节表损坏或使用未支持的扩展编号。\n";
    file.seek(offset);
    QByteArray sections = file.read(count * size), names;
    if (namesIndex < count) {
        auto base = int(namesIndex * size);
        auto start = number(sections, base + (wide ? 24 : 16), wide ? 8 : 4);
        auto bytes = number(sections, base + (wide ? 32 : 20), wide ? 8 : 4);
        if (bytes <= 4 * 1024 * 1024 && start <= quint64(file.size()) && bytes <= quint64(file.size()) - start) {
            file.seek(start); names = file.read(bytes);
        }
    }
    result += "\n节名\t类型\t虚拟地址\t文件偏移\t大小\t标志\n";
    for (quint64 i = 0; i < count; ++i) {
        int base = int(i * size);
        auto name = number(sections, base, 4);
        QString label;
        if (name < quint64(names.size())) {
            int end = names.indexOf('\0', int(name));
            if (end >= 0) label = QString::fromUtf8(names.mid(name, qMin(end - int(name), 256)));
        }
        result += QString("%1\t%2\t0x%3\t0x%4\t%5\t0x%6\n").arg(label)
            .arg(number(sections, base + 4, 4))
            .arg(number(sections, base + (wide ? 16 : 12), wide ? 8 : 4), 0, 16)
            .arg(number(sections, base + (wide ? 24 : 16), wide ? 8 : 4), 0, 16)
            .arg(number(sections, base + (wide ? 32 : 20), wide ? 8 : 4))
            .arg(number(sections, base + 8, wide ? 8 : 4), 0, 16);
    }
    return result;
}
}
