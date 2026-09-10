#include "resources.h"
#include <QSslCertificate>
#include <QSslKey>
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
    if (type == 0)
        return n == 1 ? "@empty" : "@null";
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
    if (type == 5 || type == 6) {
        static const double radix[] = {1. / 256, 1. / 32768, 1. / 8388608, 1. / 2147483648};
        const double number = qint32(n & 0xffffff00u) * radix[(n >> 4) & 3];
        const QStringList units = {"px", "dp", "sp", "pt", "in", "mm"};
        if (type == 5 && (n & 15) < 6)
            return QString::number(number, 'g', 9) + units[n & 15];
        if (type == 6 && (n & 15) < 2)
            return QString::number(number * 100, 'g', 9) + ((n & 15) ? "%p" : "%");
    }
    if (type == 4) {
        float f;
        memcpy(&f, &n, 4);
        return QString::number(f);
    }
    return QString("0x%1 (type 0x%2)").arg(n, 0, 16).arg(type, 0, 16);
}
struct Archive {
    zip_t *z = nullptr;
    explicit Archive(const QString &path) : z(zip_open(path.toUtf8().constData(), 0, 'r')) {}
    ~Archive() {
        if (z)
            zip_close(z);
    }
};
struct DerNode {
    int tag = 0;
    QByteArray raw, value;
};
QList<DerNode> derNodes(const QByteArray &data) {
    QList<DerNode> nodes;
    qsizetype p = 0;
    while (p < data.size() && nodes.size() < 4096) {
        const auto start = p;
        if (p + 2 > data.size())
            return {};
        int tag = uchar(data[p++]);
        quint64 length = uchar(data[p++]);
        if (length & 128) {
            const int count = length & 127;
            if (!count || count > 4 || p + count > data.size())
                return {};
            length = 0;
            for (int i = 0; i < count; i++)
                length = (length << 8) | uchar(data[p++]);
        }
        if (length > quint64(data.size() - p))
            return {};
        nodes << DerNode{tag, data.mid(start, p + length - start), data.mid(p, length)};
        p += length;
    }
    return nodes;
}
QList<QSslCertificate> certificatesIn(const QByteArray &data, int depth = 0) {
    QList<QSslCertificate> result;
    if (depth > 12 || data.size() > 8 * 1024 * 1024)
        return result;
    for (const auto &node : derNodes(data)) {
        if (node.tag == 0x30) {
            const auto children = derNodes(node.value);
            if (children.size() == 3 && children[0].tag == 0x30 && children[1].tag == 0x30 &&
                children[2].tag == 3) {
                auto cert = QSslCertificate(node.raw, QSsl::Der);
                if (!cert.isNull() && !cert.serialNumber().isEmpty()) {
                    result << cert;
                    continue;
                }
            }
        }
        if (node.tag & 0x20)
            result.append(certificatesIn(node.value, depth + 1));
    }
    return result;
}
QString oidText(const QByteArray &data) {
    if (data.isEmpty())
        return {};
    QStringList values;
    quint64 n = 0;
    bool first = true;
    for (auto ch : data) {
        if (n > (quint64(1) << 55))
            return {};
        n = (n << 7) | (uchar(ch) & 127);
        if (!(uchar(ch) & 128)) {
            if (first) {
                auto a = qMin(quint64(2), n / 40);
                values << QString::number(a) << QString::number(n - a * 40);
                first = false;
            } else
                values << QString::number(n);
            n = 0;
        }
    }
    return values.join('.');
}
QString certificateDetails(const QSslCertificate &c) {
    auto distinguished = [](const QSslCertificate &cert, bool issuer) {
        QStringList fields;
        for (const auto &pair : QList<QPair<QString, QSslCertificate::SubjectInfo>>{
                 {"C", QSslCertificate::CountryName},
                 {"ST", QSslCertificate::StateOrProvinceName},
                 {"L", QSslCertificate::LocalityName},
                 {"O", QSslCertificate::Organization},
                 {"OU", QSslCertificate::OrganizationalUnitName},
                 {"CN", QSslCertificate::CommonName}}) {
            const auto value =
                issuer ? cert.issuerInfo(pair.second) : cert.subjectInfo(pair.second);
            if (!value.isEmpty())
                fields << pair.first + "=" + value.join(", ");
        }
        return fields.join(", ");
    };
    auto key = c.publicKey();
    QString algorithm = key.algorithm() == QSsl::Rsa   ? "RSA"
                        : key.algorithm() == QSsl::Ec  ? "EC"
                        : key.algorithm() == QSsl::Dsa ? "DSA"
                                                       : "Opaque";
    QString out = "\n类型: X.509\n版本: " + c.version() + "\n序列号: " + c.serialNumber() +
                  "\n证书主体: " + distinguished(c, false) + "\n签发者: " + distinguished(c, true) +
                  "\n有效期开始: " + c.effectiveDate().toString(Qt::ISODate) +
                  "\n有效期截止: " + c.expiryDate().toString(Qt::ISODate) +
                  "\n公钥类型: " + algorithm + QString("\n公钥大小: %1 bits\n").arg(key.length());
    auto outer = derNodes(c.toDer());
    if (!outer.isEmpty()) {
        auto parts = derNodes(outer.first().value);
        if (parts.size() > 1) {
            auto alg = derNodes(parts[1].value);
            if (!alg.isEmpty()) {
                auto oid = oidText(alg[0].value);
                const QMap<QString, QString> names{{"1.2.840.113549.1.1.11", "SHA256withRSA"},
                                                   {"1.2.840.113549.1.1.5", "SHA1withRSA"},
                                                   {"1.2.840.113549.1.1.12", "SHA384withRSA"},
                                                   {"1.2.840.10045.4.3.2", "SHA256withECDSA"}};
                out += "签名算法: " + names.value(oid, oid) + "\n签名 OID: " + oid + '\n';
            }
        }
    }
    if (key.algorithm() == QSsl::Rsa) {
        std::function<bool(const QByteArray &, int)> rsa = [&](const QByteArray &data, int depth) {
            if (depth > 6)
                return false;
            auto nodes = derNodes(data);
            if (nodes.size() == 2 && nodes[0].tag == 2 && nodes[1].tag == 2 &&
                nodes[1].value.size() <= 8) {
                quint64 exponent = 0;
                for (auto b : nodes[1].value)
                    exponent = (exponent << 8) | uchar(b);
                out += QString("RSA 指数: %1\nRSA 模数（十六进制）: %2\n")
                           .arg(exponent)
                           .arg(QString::fromLatin1(nodes[0].value.toHex()));
                return true;
            }
            for (const auto &node : nodes) {
                if ((node.tag & 0x20) && rsa(node.value, depth + 1))
                    return true;
                if (node.tag == 3 && !node.value.isEmpty() && node.value[0] == 0 &&
                    rsa(node.value.mid(1), depth + 1))
                    return true;
            }
            return false;
        };
        rsa(key.toDer(), 0);
    }
    for (const auto &h : QList<QPair<QString, QCryptographicHash::Algorithm>>{
             {"MD5", QCryptographicHash::Md5},
             {"SHA-1", QCryptographicHash::Sha1},
             {"SHA-256", QCryptographicHash::Sha256}})
        out += "证书 " + h.first + ": " + c.digest(h.second).toHex(' ').toUpper() + '\n';
    return out;
}
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
// ResTable_config payload, excluding its uint32 size field. Keep Android qualifier order.
QString configurationName(const QByteArray &config) {
    auto byte = [&](int i) { return i < config.size() ? quint8(config[i]) : quint8(0); };
    auto word = [&](int i) { return int(byte(i)) | (int(byte(i + 1)) << 8); };
    auto locale = [&](int i, char base) {
        auto a = byte(i), b = byte(i + 1);
        if (!a)
            return QString();
        if (a & 128)
            return QString(QChar(base + (b & 31))) +
                   QChar(base + ((b & 224) >> 5) + ((a & 3) << 3)) + QChar(base + ((a & 124) >> 2));
        return QString::fromLatin1(config.mid(i, 2)).remove(QChar(0));
    };
    auto chars = [&](int i, int count) {
        return QString::fromLatin1(config.mid(i, count)).section(QChar(0), 0, 0);
    };
    QStringList q;
    auto select = [&](int value, const QStringList &names) {
        if (value > 0 && value < names.size() && !names[value].isEmpty())
            q << names[value];
    };
    if (word(0))
        q << QString("mcc%1").arg(word(0), 3, 10, QChar('0'));
    if (word(2))
        q << (word(2) == 65535 ? "mnc00" : "mnc" + QString::number(word(2)));
    const auto language = locale(4, 'a'), region = locale(6, '0'), script = chars(32, 4),
               variant = chars(36, 8);
    if (!language.isEmpty() || !region.isEmpty()) {
        if (script.isEmpty() && variant.isEmpty() && region.size() != 3) {
            q << language;
            if (!region.isEmpty())
                q << "r" + region;
        } else {
            QString tag = "b+" + language;
            for (const auto &part : {script, region, variant.toUpper()})
                if (!part.isEmpty())
                    tag += '+' + part;
            q << tag;
        }
    }
    select(byte(15) & 3, {"", "neuter", "feminine", "masculine"});
    select((byte(24) >> 6) & 3, {"", "ldltr", "ldrtl"});
    if (word(26))
        q << QString("sw%1dp").arg(word(26));
    if (word(28))
        q << QString("w%1dp").arg(word(28));
    if (word(30))
        q << QString("h%1dp").arg(word(30));
    select(byte(24) & 15, {"", "small", "normal", "large", "xlarge"});
    select((byte(24) >> 4) & 3, {"", "notlong", "long"});
    select(byte(44) & 3, {"", "notround", "round"});
    select((byte(45) >> 2) & 3, {"", "lowdr", "highdr"});
    select(byte(45) & 3, {"", "nowidecg", "widecg"});
    select(byte(8), {"", "port", "land", "square"});
    select(byte(25) & 15, {"", "", "desk", "car", "television", "appliance", "watch", "vrheadset"});
    select((byte(25) >> 4) & 3, {"", "notnight", "night"});
    const QMap<int, QString> densities{{120, "ldpi"},    {160, "mdpi"},     {213, "tvdpi"},
                                       {240, "hdpi"},    {320, "xhdpi"},    {480, "xxhdpi"},
                                       {640, "xxxhdpi"}, {65534, "anydpi"}, {65535, "nodpi"}};
    if (word(10))
        q << densities.value(word(10), QString::number(word(10)) + "dpi");
    select(byte(9), {"", "notouch", "stylus", "finger"});
    select(byte(14) & 3, {"", "keysexposed", "keyshidden", "keyssoft"});
    select(byte(12), {"", "nokeys", "qwerty", "12key"});
    select((byte(14) >> 2) & 3, {"", "navexposed", "navhidden"});
    select(byte(13), {"", "nonav", "dpad", "trackball", "wheel"});
    if (word(16) && word(18))
        q << QString("%1x%2").arg(qMax(word(16), word(18))).arg(qMin(word(16), word(18)));
    int natural = 0;
    if ((byte(25) & 15) == 7 || (byte(45) & 15))
        natural = 26;
    else if (byte(44) & 3)
        natural = 23;
    else if (word(10) == 65534)
        natural = 21;
    else if (word(26) || word(28) || word(30))
        natural = 13;
    else if (byte(25) & 63)
        natural = 8;
    else if ((byte(24) & 63) || word(10))
        natural = 4;
    if (word(20) > 0 && word(20) >= natural)
        q << "v" + QString::number(word(20));
    return q.isEmpty() ? QString() : "-" + q.join('-');
}
// Android ResTable layout: frameworks/base/libs/androidfw/include/androidfw/ResourceTypes.h.
QString describeTable(const QByteArray &bytes, QMap<QString, QString> *files) {
    QMap<QString, QString> generated;
    QHash<QString, QString> names;
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
                        const QString resourceType = types.value(typeId - 1);
                        const auto directory = "res/values" + configurationName(config) + "/";
                        const auto packageSuffix =
                            d.u32(8) > 1 ? "_" + QString::number(packageId, 16) : QString();
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
                            const QString resourceName = keys.value(key);
                            const QString identity = QString::number(id, 16).rightJustified(8, '0');
                            names[identity] = resourceType + "/" + resourceName;
                            QList<QPair<QString, QString>> bag;
                            quint32 parentId = 0;
                            out += QString("0x%1  %2/%3 = ")
                                       .arg(id, 8, 16, QChar('0'))
                                       .arg(types.value(typeId - 1), keys.value(key));
                            const int valueStart = out.size();
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
                                    parentId = d.u32(e + 8);
                                    out += QString("{ parent: @0x%1")
                                               .arg(d.u32(e + 8), 8, 16, QChar('0'));
                                    for (quint32 m = 0; m < maps; ++m) {
                                        auto v = e + es + m * 12;
                                        bag.append(
                                            {QString::number(d.u32(v), 16).rightJustified(8, '0'),
                                             value(uchar(d.slice(v + 7, 1)[0]), d.u32(v + 8),
                                                   globals)});
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
                            const auto decoded = out.mid(valueStart);
                            if (files) {
                                const auto nameXml = resourceName.toHtmlEscaped();
                                QString xml;
                                if ((ef & 1) && !(ef & 8)) {
                                    QString tag = resourceType == "array" ? "array" : resourceType;
                                    xml = "    <" + tag + " name=\"" + nameXml + "\"";
                                    if (resourceType == "style" && parentId)
                                        xml +=
                                            " parent=\"@0x" +
                                            QString::number(parentId, 16).rightJustified(8, '0') +
                                            "\"";
                                    xml += ">\n";
                                    const QMap<QString, QString> quantities{
                                        {"01000004", "other"}, {"01000005", "zero"},
                                        {"01000006", "one"},   {"01000007", "two"},
                                        {"01000008", "few"},   {"01000009", "many"}};
                                    for (const auto &item : bag) {
                                        QString attribute;
                                        if (resourceType == "plurals")
                                            attribute = " quantity=\"" +
                                                        quantities.value(item.first, item.first) +
                                                        "\"";
                                        else if (resourceType != "array")
                                            attribute = " name=\"@0x" + item.first + "\"";
                                        xml += "        <item" + attribute + ">" +
                                               item.second.toHtmlEscaped() + "</item>\n";
                                    }
                                    xml += "    </" + tag + ">\n";
                                } else if (resourceType == "id")
                                    xml = "    <item type=\"id\" name=\"" + nameXml + "\" />\n";
                                else
                                    xml = "    <item type=\"" + resourceType.toHtmlEscaped() +
                                          "\" name=\"" + nameXml + "\">" + decoded.toHtmlEscaped() +
                                          "</item>\n";
                                QString filename = resourceType;
                                if (!filename.endsWith('s'))
                                    filename += 's';
                                generated[directory + filename + packageSuffix + ".xml"] += xml;
                                if (defaults)
                                    generated["res/values/public" + packageSuffix + ".xml"] +=
                                        "    <public type=\"" + resourceType.toHtmlEscaped() +
                                        "\" name=\"" + nameXml + "\" id=\"0x" + identity +
                                        "\" />\n";
                            }
                            out += '\n';
                            if (out.size() > 4 * 1024 * 1024) {
                                if (!files)
                                    return out +
                                           "\n预览达到 4 MiB 上限，可展开资源表查看分类文件。\n";
                                out.clear();
                            }
                        }
                    }
                    t += ts;
                }
            }
            p += size;
        }
        if (files) {
            for (auto it = generated.begin(); it != generated.end(); ++it) {
                auto text = it.value();
                QRegularExpression reference("([@?])0x([0-9a-f]{8})");
                auto matches = reference.globalMatch(text);
                QList<QRegularExpressionMatch> all;
                while (matches.hasNext())
                    all << matches.next();
                for (auto m = all.crbegin(); m != all.crend(); ++m)
                    if (names.contains(m->captured(2)))
                        text.replace(m->capturedStart(), m->capturedLength(),
                                     m->captured(1) + names.value(m->captured(2)));
                it.value() = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n" + text +
                             "</resources>\n";
            }
            *files = generated;
        }
        return out;
    } catch (const QString &e) {
        if (files)
            files->clear();
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
            {"main_activities", QJsonArray::fromStringList(launcherActivities(manifest))},
            {"version", version},
            {"bytes", double(QFileInfo(path).size())}};
}
QStringList launcherActivities(const QString &manifest) {
    QXmlStreamReader xml(manifest);
    const QString ns = "http://schemas.android.com/apk/res/android";
    QString package, target;
    QStringList result;
    bool main = false, launcher = false, inFilter = false, enabled = true, appEnabled = true;
    while (!xml.atEnd()) {
        xml.readNext();
        const auto name = xml.name();
        if (xml.isStartElement()) {
            auto attrs = xml.attributes();
            if (name == u"manifest")
                package = attrs.value("package").toString();
            if (name == u"application")
                appEnabled = attrs.value(ns, "enabled") != u"false";
            if (name == u"activity" || name == u"activity-alias") {
                target = attrs.value(ns, name == u"activity-alias" ? "targetActivity" : "name")
                             .toString();
                enabled = appEnabled && attrs.value(ns, "enabled") != u"false";
            }
            if (name == u"intent-filter") {
                inFilter = true;
                main = false;
                launcher = false;
            }
            if (inFilter && name == u"action" &&
                attrs.value(ns, "name") == u"android.intent.action.MAIN")
                main = true;
            if (inFilter && name == u"category" &&
                (attrs.value(ns, "name") == u"android.intent.category.LAUNCHER" ||
                 attrs.value(ns, "name") == u"android.intent.category.LEANBACK_LAUNCHER"))
                launcher = true;
        } else if (xml.isEndElement()) {
            if (name == u"intent-filter") {
                if (main && launcher && enabled && !target.isEmpty()) {
                    auto full = target;
                    if (full.startsWith('.'))
                        full = package + full;
                    else if (!full.contains('.'))
                        full = package + '.' + full;
                    if (!result.contains(full))
                        result << full;
                }
                inFilter = false;
            }
            if (name == u"activity" || name == u"activity-alias")
                target.clear();
        }
    }
    return xml.hasError() ? QStringList() : result;
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
        for (const auto &c : certificatesIn(der))
            out += certificateDetails(c);
    };
    QStringList entryNames;
    Archive a(path);
    if (a.z)
        for (int i = 0; i < zip_entries_total(a.z); i++) {
            zip_entry_openbyindex(a.z, i);
            QString n = QString::fromUtf8(zip_entry_name(a.z));
            entryNames << n;
            if (n.startsWith("META-INF/", Qt::CaseInsensitive) &&
                (n.endsWith(".RSA", Qt::CaseInsensitive) ||
                 n.endsWith(".DSA", Qt::CaseInsensitive) ||
                 n.endsWith(".EC", Qt::CaseInsensitive))) {
                out += "\nv1 签名条目: " + n + '\n';
                QString error;
                certs(read(path, n, 8 * 1024 * 1024, &error));
                if (!error.isEmpty())
                    out += error + '\n';
            }
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
    QString error;
    auto manifest = read(path, "META-INF/MANIFEST.MF", 16 * 1024 * 1024, &error);
    if (error.isEmpty()) {
        manifest.replace("\r\n", "\n");
        manifest.replace("\n ", "");
        QSet<QString> covered;
        for (const auto &section : manifest.split('\n'))
            if (section.startsWith("Name: "))
                covered.insert(QString::fromUtf8(section.mid(6)));
        QStringList uncovered;
        for (const auto &name : entryNames) {
            auto upper = name.toUpper();
            if (name.endsWith('/') || upper == "META-INF/MANIFEST.MF" ||
                (upper.startsWith("META-INF/") &&
                 (upper.endsWith(".SF") || upper.endsWith(".RSA") || upper.endsWith(".DSA") ||
                  upper.endsWith(".EC"))))
                continue;
            if (!covered.contains(name))
                uncovered << name;
        }
        out +=
            QString("\n警告 / v1 覆盖范围\n未列入 v1 Manifest 的条目: %1\n").arg(uncovered.size());
        out += uncovered.join('\n');
        if (!uncovered.isEmpty())
            out += "\n这些条目不受 v1 摘要保护；是否受 v2/v3 保护须执行完整签名验证。\n";
    }
    return out + "\n以上为签名块与证书解析，未执行 APK 完整性或信任链验证。\n";
}
} // namespace Resources
