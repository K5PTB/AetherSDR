#include "tts/KokoroVoices.h"

#include "core/ZipArchive.h"

#include <QFile>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr qint64 kMaxVoicesFileBytes = 64LL * 1024 * 1024;
}

bool KokoroVoices::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open the voices file: %1").arg(f.errorString());
        return false;
    }
    if (f.size() > kMaxVoicesFileBytes) {
        if (error) *error = QStringLiteral("The voices file is unexpectedly large.");
        return false;
    }
    return loadFromZipBytes(f.readAll(), error);
}

bool KokoroVoices::loadFromZipBytes(const QByteArray& zip, QString* error)
{
    QString zipError;
    const QMap<QString, QByteArray> entries = readZipEntries(zip, &zipError);
    if (!zipError.isEmpty() || entries.isEmpty()) {
        if (error) *error = QStringLiteral("The voices file is not a valid archive: %1").arg(zipError);
        return false;
    }
    m_entries.clear();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        QString name = it.key();
        if (!name.endsWith(QLatin1String(".npy")))
            continue;
        name.chop(4);
        m_entries.insert(name, it.value());
    }
    if (m_entries.isEmpty()) {
        if (error) *error = QStringLiteral("The voices file holds no voices.");
        return false;
    }
    return true;
}

bool KokoroVoices::parseNpy(const QByteArray& npy, int& rows, int& dim,
                            QVector<float>& data, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error) *error = QStringLiteral("Bad voice data: %1").arg(why);
        return false;
    };
    if (npy.size() < 12 || !npy.startsWith("\x93NUMPY"))
        return fail(QStringLiteral("not a NumPy array"));
    const int major = quint8(npy[6]);
    qsizetype headerLen = 0;
    qsizetype dataStart = 0;
    if (major == 1) {
        headerLen = qFromLittleEndian<quint16>(npy.constData() + 8);
        dataStart = 10 + headerLen;
    } else if (major == 2 || major == 3) {
        headerLen = qFromLittleEndian<quint32>(npy.constData() + 8);
        dataStart = 12 + headerLen;
    } else {
        return fail(QStringLiteral("unsupported format version %1").arg(major));
    }
    if (dataStart > npy.size())
        return fail(QStringLiteral("truncated header"));
    const QString header = QString::fromLatin1(npy.mid(dataStart - headerLen, headerLen));
    if (!header.contains(QLatin1String("'descr': '<f4'")))
        return fail(QStringLiteral("not little-endian float32"));
    if (!header.contains(QLatin1String("'fortran_order': False")))
        return fail(QStringLiteral("column-major order"));
    static const QRegularExpression shapeRe(
        QStringLiteral("'shape':\\s*\\((\\d+),\\s*1,\\s*(\\d+),?\\s*\\)"));
    const auto m = shapeRe.match(header);
    if (!m.hasMatch())
        return fail(QStringLiteral("unexpected shape"));
    rows = m.captured(1).toInt();
    dim = m.captured(2).toInt();
    if (rows <= 0 || rows > 4096 || dim <= 0 || dim > 4096)
        return fail(QStringLiteral("implausible shape"));
    const qsizetype needed = qsizetype(rows) * dim * 4;
    if (npy.size() - dataStart < needed)
        return fail(QStringLiteral("truncated data"));
    data.resize(qsizetype(rows) * dim);
    const char* p = npy.constData() + dataStart;
    for (qsizetype i = 0; i < data.size(); ++i)
        data[i] = qFromLittleEndian<float>(p + i * 4);
    return true;
}

QVector<float> KokoroVoices::style(const QString& voice, int tokenCount, QString* error) const
{
    const auto it = m_entries.constFind(voice);
    if (it == m_entries.cend()) {
        if (error) *error = QStringLiteral("The voice \"%1\" is not in the voices file.").arg(voice);
        return {};
    }
    int rows = 0;
    int dim = 0;
    QVector<float> data;
    if (!parseNpy(it.value(), rows, dim, data, error))
        return {};
    if (dim != kStyleDim) {
        if (error) *error = QStringLiteral("The voice \"%1\" has %2-wide styles, expected %3.")
                                .arg(voice).arg(dim).arg(kStyleDim);
        return {};
    }
    const int row = std::clamp(tokenCount, 1, rows) - 1;
    return data.mid(qsizetype(row) * dim, dim);
}

} // namespace AetherSDR
