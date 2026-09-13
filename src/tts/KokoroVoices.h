#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>

namespace AetherSDR {

// Kokoro voice styles (voices-v1.0.bin): a zip of NumPy arrays, one per voice
// ("af_heart.npy", "am_michael.npy", …), each float32 shaped (510, 1, 256) —
// one style vector per input length. Untrusted input: every field is checked.
class KokoroVoices {
public:
    static constexpr int kStyleDim = 256;

    bool load(const QString& path, QString* error);
    bool loadFromZipBytes(const QByteArray& zip, QString* error);
    bool contains(const QString& voice) const { return m_entries.contains(voice); }
    QStringList names() const { return m_entries.keys(); }

    // The style vector for `tokenCount` tokens: row min(tokenCount, rows) - 1,
    // as the reference implementation picks it. Empty with *error on failure.
    QVector<float> style(const QString& voice, int tokenCount, QString* error) const;

    // A (rows, 1, dim) little-endian float32 .npy array, flattened row-major.
    static bool parseNpy(const QByteArray& npy, int& rows, int& dim,
                         QVector<float>& data, QString* error);

private:
    QMap<QString, QByteArray> m_entries;   // voice name -> raw .npy bytes
};

} // namespace AetherSDR
