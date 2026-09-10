#include "resources.h"
#include <QSslCertificate>
#include <QtCore>
extern "C" {
#include "zip.h"
}
namespace {
struct Data {
    QByteArray b;
    quint16 u16(qint64 p) const {
        if (p < 0 || p + 2 > b.size())
            throw QString("Truncated data");
        return qFromLittleEndian<quint16>(b.constData() + p);
    }
    quint32 u32(qint64 p) const {
        if (p < 0 || p + 4 > b.size())
            throw QString("Truncated data");
        return qFromLittleEndian<quint32>(b.constData() + p);
    }
    quint64 u64(qint64 p) const {
        if (p < 0 || p + 8 > b.size())
            throw QString("Truncated data");
        return qFromLittleEndian<quint64>(b.constData() + p);
    }
    QByteArray slice(qint64 p, qint64 n) const {
        if (p < 0 || n < 0 || p > b.size() || n > b.size() - p)
            throw QString("Invalid data range");
        return b.mid(p, n);
    }
};
QStringList pool(const Data &d, int start, int size) {
    QStringList out;
    const auto count = d.u32(start + 8), flags = d.u32(start + 16), strings = d.u32(start + 20);
    if (count > 1000000 || d.u16(start + 2) < 28 ||
        quint64(count) * 4 + d.u16(start + 2) > quint64(size))
        throw QString("Invalid string pool");
    auto len8 = [&](qint64 &p) {
        int n = uchar(d.slice(p++, 1)[0]);
        if (n & 0x80)
            n = ((n & 0x7f) << 8) | uchar(d.slice(p++, 1)[0]);
        return n;
    };
    auto len16 = [&](qint64 &p) {
        quint32 n = d.u16(p);
        p += 2;
        if (n & 0x8000) {
            n = ((n & 0x7fff) << 16) | d.u16(p);
            p += 2;
        }
        return n;
    };
    for (quint32 i = 0; i < count; i++) {
        qint64 p = start + quint64(strings) + d.u32(start + d.u16(start + 2) + 4 * i);
        if (p < start || p >= start + size)
            throw QString("Invalid string offset");
        if (flags & 0x100) {
            len8(p);
            int n = len8(p);
            if (p + n > start + size)
                throw QString("Invalid UTF-8 length");
            out << QString::fromUtf8(d.slice(p, n));
        } else {
            auto n = len16(p);
            if (p + quint64(n) * 2 > quint64(start + size))
                throw QString("Invalid UTF-16 length");
            QString s;
            s.reserve(n);
            for (quint32 j = 0; j < n; j++)
                s += QChar(d.u16(p + 2 * j));
            out << s;
        }
    }
    return out;
}
QString value(quint8 type, quint32 n, const QStringList &strings) {
    if (type == 3)
        return strings.value(n);
    if (type == 0x12)
        return n ? "true" : "false";
    if (type == 0x10)
        return QString::number(qint32(n));
    if (type == 0x11)
        return "0x" + QString::number(n, 16);
    if (type == 1 || type == 2)
        return QString(type == 1 ? "@0x" : "?0x") + QString::number(n, 16).rightJustified(8, '0');
    if (type >= 0x1c && type <= 0x1f)
        return "#" + QString::number(n, 16).rightJustified(8, '0');
    if (type == 4) {
        float f;
        memcpy(&f, &n, 4);
        return QString::number(f);
    }
    return QString("0x%1 (type 0x%2)").arg(n, 0, 16).arg(type, 0, 16);
}
struct Archive {
    zip_t *z = nullptr;
    explicit Archive(const QString &path)
        : z(zip_open(QFile::encodeName(path).constData(), 0, 'r')) {}
    ~Archive() {
        if (z)
            zip_close(z);
    }
};
} // namespace
namespace Resources {
QByteArray read(const QString &path, const QString &entry, qint64 limit, QString *error) {
    if (entry.isEmpty()) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            *error = f.errorString();
            return {};
        }
        if (f.size() > limit) {
            *error = "文件超过查看大小限制";
            return {};
        }
        return f.readAll();
    }
    Archive a(path);
    if (!a.z || zip_entry_open(a.z, entry.toUtf8().constData()) < 0) {
        *error = "无法打开压缩包条目";
        return {};
    }
    const auto n = zip_entry_size(a.z);
    if (n > quint64(limit)) {
        *error = "资源超过查看大小限制";
        return {};
    }
    QByteArray b(n, Qt::Uninitialized);
    if (zip_entry_noallocread(a.z, b.data(), b.size()) < 0) {
        *error = "资源解压失败";
        return {};
    }
    return b;
}
QString decodeXml(const QByteArray &bytes) {
    if (bytes.trimmed().startsWith('<'))
        return QString::fromUtf8(bytes);
    try {
        Data d{bytes};
        if (d.u16(0) != 3)
            return {};
        const quint32 total = d.u32(4);
        if (total > quint32(bytes.size()))
            throw QString("Truncated XML");
        QString out;
        QXmlStreamWriter w(&out);
        w.setAutoFormatting(true);
        w.writeStartDocument();
        QStringList strings;
        QMap<quint32, QString> ns;
        for (quint32 p = d.u16(2); p + 8 <= total;) {
            const auto type = d.u16(p), header = d.u16(p + 2);
            const auto size = d.u32(p + 4);
            if (size < header || header < 8 || size > total - p)
                throw QString("Invalid XML chunk");
            if (type == 1)
                strings = pool(d, p, size);
            else if (type == 0x100)
                ns[d.u32(p + 20)] = strings.value(d.u32(p + 16));
            else if (type == 0x102) {
                if (size < 36)
                    throw QString("Invalid XML element");
                const auto uri = d.u32(p + 16), name = d.u32(p + 20);
                auto tag = strings.value(name);
                if (ns.contains(uri) && !ns[uri].isEmpty())
                    tag = ns[uri] + ":" + tag;
                w.writeStartElement(tag);
                for (auto it = ns.begin(); it != ns.end(); ++it)
                    w.writeNamespace(strings.value(it.key()), it.value());
                const auto stride = d.u16(p + 26), count = d.u16(p + 28), start = d.u16(p + 24);
                if (stride < 20 || quint64(16) + start + quint64(stride) * count > size)
                    throw QString("Invalid XML attributes");
                for (quint32 i = 0; i < count; i++) {
                    auto a = p + 16 + start + i * stride;
                    auto name = strings.value(d.u32(a + 4));
                    auto uri = d.u32(a);
                    if (ns.contains(uri) && !ns[uri].isEmpty())
                        name = ns[uri] + ":" + name;
                    auto raw = d.u32(a + 8);
                    w.writeAttribute(name, raw != 0xffffffff ? strings.value(raw)
                                                             : value(uchar(d.slice(a + 15, 1)[0]),
                                                                     d.u32(a + 16), strings));
                }
            } else if (type == 0x103)
                w.writeEndElement();
            else if (type == 0x104)
                w.writeCharacters(strings.value(d.u32(p + 16)));
            p += size;
        }
        w.writeEndDocument();
        return out;
    } catch (const QString &e) {
        return QString("XML 解析失败：%1\n").arg(e);
    }
}
// Android ResTable layout: frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h.
QString describeTable(const QByteArray &bytes) {
    try {
        Data d{bytes};
        if (d.u16(0) != 2)
            return hex(bytes);
        const quint32 total = d.u32(4);
        if (total > quint32(bytes.size()))
            throw QString("Truncated resource table");
        QString out = QString("Android resource table — %1 packages\n").arg(d.u32(8));
        QStringList globals;
        for (quint32 p = d.u16(2); p + 8 <= total;) {
            const auto type = d.u16(p), head = d.u16(p + 2);
            const auto size = d.u32(p + 4);
            if (head < 8 || size < head || size > total - p)
                throw QString("Invalid resource chunk");
            if (type == 1)
                globals = pool(d, p, size);
            if (type == 0x200) {
                if (head < 284)
                    throw QString("Invalid package header");
                const auto packageId = d.u32(p + 8);
                QString package;
                for (int i = 0; i < 128 && d.u16(p + 12 + i * 2); ++i)
                    package += QChar(d.u16(p + 12 + i * 2));
                auto readPool = [&](quint32 offset) {
                    if (offset > size - 8 || d.u32(p + offset + 4) > size - offset)
                        throw QString("Invalid package string pool");
                    return pool(d, p + offset, d.u32(p + offset + 4));
                };
                auto types = readPool(d.u32(p + 268)), keys = readPool(d.u32(p + 276));
                auto typeOffset = head >= 288 ? d.u32(p + 284) : 0;
                out += "\nPackage: " + package + "\n";
                for (quint32 t = p + head; t + 8 <= p + size;) {
                    auto th = d.u16(t + 2);
                    auto ts = d.u32(t + 4);
                    if (th < 8 || ts < th || ts > p + size - t)
                        throw QString("Invalid type chunk");
                    if (d.u16(t) == 0x201) {
                        if (th < 24)
                            throw QString("Invalid type header");
                        auto typeId = uchar(d.slice(t + 8, 1)[0]),
                             flags = uchar(d.slice(t + 9, 1)[0]);
                        auto count = d.u32(t + 12), dataStart = d.u32(t + 16);
                        const quint32 stride = (flags & 2) ? 2 : 4;
                        if (dataStart > ts || dataStart < th ||
                            quint64(count) * stride > dataStart - th)
                            throw QString("Invalid entry offsets");
                        auto configSize = d.u32(t + 20);
                        if (configSize < 4 || configSize > th - 20)
                            throw QString("Invalid resource configuration");
                        const auto config = d.slice(t + 24, configSize - 4);
                        bool defaults = true;
                        for (char c : config)
                            if (c) {
                                defaults = false;
                                break;
                            }
                        out +=
                            "\n[" + types.value(typeId - 1) + "; " +
                            (defaults ? QString("default") : QString::fromLatin1(config.toHex())) +
                            "]\n";
                        for (quint32 i = 0; i < count; ++i) {
                            quint32 index = i, offset;
                            if (flags & 1) {
                                index = d.u16(t + th + i * 4);
                                offset = quint32(d.u16(t + th + i * 4 + 2)) * 4;
                            } else if (flags & 2) {
                                auto off = d.u16(t + th + i * 2);
                                if (off == 0xffff)
                                    continue;
                                offset = quint32(off) * 4;
                            } else {
                                offset = d.u32(t + th + i * 4);
                                if (offset == 0xffffffff)
                                    continue;
                            }
                            if (offset > ts - dataStart || ts - dataStart - offset < 8)
                                throw QString("Invalid entry range");
                            const quint32 e = t + dataStart + offset, end = t + ts;
                            auto ef = d.u16(e + 2), es = d.u16(e);
                            const auto key = (ef & 8) ? es : d.u32(e + 4);
                            const quint32 id =
                                (packageId << 24) | ((typeId + typeOffset) << 16) | index;
                            out += QString("0x%1  %2/%3 = ")
                                       .arg(id, 8, 16, QChar('0'))
                                       .arg(types.value(typeId - 1), keys.value(key));
                            if (ef & 8)
                                out += value(ef >> 8, d.u32(e + 4), globals);
                            else {
                                if (es < 8 || es > end - e)
                                    throw QString("Invalid entry size");
                                if (ef & 1) {
                                    if (es < 16)
                                        throw QString("Invalid map header");
                                    auto maps = d.u32(e + 12);
                                    if (quint64(maps) * 12 > end - e - es)
                                        throw QString("Invalid map entries");
                                    out += QString("{ parent: @0x%1")
                                               .arg(d.u32(e + 8), 8, 16, QChar('0'));
                                    for (quint32 m = 0; m < maps; ++m) {
                                        auto v = e + es + m * 12;
                                        out += QString("\n    @0x%1: %2")
                                                   .arg(d.u32(v), 8, 16, QChar('0'))
                                                   .arg(value(uchar(d.slice(v + 7, 1)[0]),
                                                              d.u32(v + 8), globals));
                                    }
                                    out += "\n}";
                                } else {
                                    if (end - e - es < 8)
                                        throw QString("Invalid resource value");
                                    out += value(uchar(d.slice(e + es + 3, 1)[0]),
                                                 d.u32(e + es + 4), globals);
                                }
                            }
                            out += '\n';
                            if (out.size() > 4 * 1024 * 1024)
                                return out + "\n预览达到 4 MiB 上限，可导出完整资源表。\n";
                        }
                    }
                    t += ts;
                }
            }
            p += size;
        }
        return out;
    } catch (const QString &e) {
        return "资源表解析失败：" + e;
    }
}

QString hex(const QByteArray &b) {
    QString out;
    for (int i = 0; i < b.size() && i < 65536; i += 16) {
        auto line = b.mid(i, 16);
        QString ascii;
        for (char c : line)
            ascii += uchar(c) >= 32 && uchar(c) < 127 ? QChar(c) : QChar('.');
        out += QString("%1  %2  %3\n")
                   .arg(i, 8, 16, QChar('0'))
                   .arg(QString::fromLatin1(line.toHex(' ')), -47)
                   .arg(ascii);
    }
    if (b.size() > 65536)
        out += "\n仅预览前 64 KiB，可导出完整资源。";
    return out;
}
QJsonObject inspect(const QString &path) {
    QJsonArray entries;
    Archive a(path);
    QString manifest;
    if (a.z) {
        int count = zip_entries_total(a.z);
        for (int i = 0; i < count && i < 200000; i++) {
            if (zip_entry_openbyindex(a.z, i) < 0)
                continue;
            QString n = QString::fromUtf8(zip_entry_name(a.z));
            if (!zip_entry_isdir(a.z))
                entries.append(QJsonObject{{"name", n}, {"size", double(zip_entry_size(a.z))}});
            zip_entry_close(a.z);
        }
    }
    QString error;
    if (QFileInfo(path).suffix().compare("apk", Qt::CaseInsensitive) == 0)
        manifest = decodeXml(read(path, "AndroidManifest.xml", 16 * 1024 * 1024, &error));
    QXmlStreamReader xml(manifest);
    QString package, application, version;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            auto attrs = xml.attributes();
            if (xml.name() == u"manifest") {
                package = attrs.value("package").toString();
                version = attrs.value("http://schemas.android.com/apk/res/android", "versionName")
                              .toString();
            }
            if (xml.name() == u"application")
                application =
                    attrs.value("http://schemas.android.com/apk/res/android", "name").toString();
        }
    }
    if (application.startsWith('.'))
        application = package + application;
    else if (!application.isEmpty() && !application.contains('.'))
        application = package + '.' + application;
    return {{"path", path},
            {"entries", entries},
            {"manifest", manifest},
            {"package", package},
            {"application", application},
            {"version", version},
            {"bytes", double(QFileInfo(path).size())}};
}
QString signature(const QString &path) {
    QString out = "APK 签名信息\n\n";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return f.errorString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    out += "文件 SHA-256: " + QString::fromLatin1(hash.result().toHex()) + '\n';
    auto certs = [&](const QByteArray &der) {
        auto list = QSslCertificate::fromData(der, QSsl::Der);
        for (const auto &c : list) {
            out += "\n证书主体: " + c.subjectInfo(QSslCertificate::CommonName).join(", ") +
                   "\n签发者: " + c.issuerInfo(QSslCertificate::CommonName).join(", ") +
                   "\n序列号: " + c.serialNumber() +
                   "\n有效期: " + c.effectiveDate().toString(Qt::ISODate) + " — " +
                   c.expiryDate().toString(Qt::ISODate) +
                   "\n证书 SHA-256: " + c.digest(QCryptographicHash::Sha256).toHex() + '\n';
        }
    };
    Archive a(path);
    if (a.z)
        for (int i = 0; i < zip_entries_total(a.z); i++) {
            zip_entry_openbyindex(a.z, i);
            QString n = QString::fromUtf8(zip_entry_name(a.z));
            if (n.startsWith("META-INF/") &&
                (n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC")))
                out += "v1 签名条目: " + n + '\n';
            zip_entry_close(a.z);
        }
    try {
        f.seek(qMax(qint64(0), f.size() - 65557));
        Data tail{f.readAll()};
        int e = -1;
        for (int i = tail.b.size() - 22; i >= 0; --i)
            if (tail.u32(i) == 0x06054b50 && i + 22 + tail.u16(i + 20) == tail.b.size()) {
                e = i;
                break;
            }
        if (e < 0)
            throw QString("找不到 ZIP 目录");
        qint64 central = tail.u32(e + 16);
        if (central < 24)
            throw QString("无 APK Signing Block");
        f.seek(central - 24);
        Data foot{f.read(24)};
        if (foot.slice(8, 16) != "APK Sig Block 42")
            throw QString("未发现 v2/v3 Signing Block");
        quint64 size = foot.u64(0);
        if (size < 24 || size > 32 * 1024 * 1024 || size + 8 > quint64(central))
            throw QString("签名块大小无效");
        f.seek(central - size - 8);
        Data block{f.read(size + 8)};
        if (block.u64(0) != size)
            throw QString("签名块长度不一致");
        auto length = [](const Data &d, qint64 &p) {
            auto n = d.u32(p);
            p += 4;
            auto b = d.slice(p, n);
            p += n;
            return Data{b};
        };
        for (qint64 p = 8; p < block.b.size() - 24;) {
            quint64 n = block.u64(p);
            p += 8;
            if (n < 4 || n > quint64(block.b.size() - 24 - p))
                throw QString("签名记录无效");
            auto id = block.u32(p);
            if (id == 0x7109871a || id == 0xf05368c0 || id == 0x1b93ad61) {
                out += QString("\n签名方案: %1\n")
                           .arg(id == 0x7109871a   ? "v2"
                                : id == 0xf05368c0 ? "v3"
                                                   : "v3.1");
                Data record{block.slice(p + 4, n - 4)};
                qint64 r = 0;
                auto signers = length(record, r);
                qint64 s = 0;
                while (s < signers.b.size()) {
                    auto signer = length(signers, s);
                    qint64 a = 0;
                    auto signedData = length(signer, a);
                    qint64 b = 0;
                    length(signedData, b);
                    auto certificates = length(signedData, b);
                    qint64 c = 0;
                    while (c < certificates.b.size())
                        certs(length(certificates, c).b);
                }
            }
            p += n;
        }
    } catch (const QString &e) {
        out += '\n' + e + '\n';
    }
    return out + "\n以上为签名块与证书解析，未执行 APK 完整性或信任链验证。\n";
}
} // namespace Resources
