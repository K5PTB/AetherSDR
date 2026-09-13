#include "tts/TtsWav.h"

#include "core/Resampler.h"

#include <QSaveFile>
#include <QtEndian>

#include <array>

namespace AetherSDR {
namespace TtsWav {

QByteArray upsampleToStereo48k(const QVector<float>& mono24k)
{
    constexpr int kBlock = 4096;
    const qsizetype expectedBytes = mono24k.size() * 2 * kChannels * qsizetype(sizeof(float));
    QByteArray stereo;
    if (mono24k.isEmpty())
        return stereo;
    stereo.reserve(expectedBytes);
    Resampler resampler(24000, kRate, kBlock);
    // r8brain has latency: feed the speech, then silence, until the expected
    // length has come out (the same drain VoiceKeyerWavDecoder uses).
    static const std::array<float, kBlock> kSilence{};
    qsizetype fed = 0;
    int flushGuard = 64;
    while (stereo.size() < expectedBytes && flushGuard > 0) {
        if (fed < mono24k.size()) {
            const int n = int(std::min<qsizetype>(kBlock, mono24k.size() - fed));
            stereo.append(resampler.processMonoToStereo(mono24k.constData() + fed, n));
            fed += n;
        } else {
            stereo.append(resampler.processMonoToStereo(kSilence.data(), kBlock));
            --flushGuard;
        }
    }
    stereo.truncate(std::min(stereo.size(), expectedBytes));
    stereo.truncate(stereo.size() - stereo.size() % (kChannels * qsizetype(sizeof(float))));
    return stereo;
}

bool write(const QString& path, const QVector<float>& mono24k, QString* error)
{
    const QByteArray frames = upsampleToStereo48k(mono24k);
    if (frames.isEmpty()) {
        if (error) *error = QStringLiteral("There is no speech to save.");
        return false;
    }
    QByteArray header(44, '\0');
    char* h = header.data();
    const quint32 dataBytes = quint32(frames.size());
    memcpy(h, "RIFF", 4);
    qToLittleEndian<quint32>(36 + dataBytes, h + 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, h + 16);
    qToLittleEndian<quint16>(3, h + 20);                           // IEEE float
    qToLittleEndian<quint16>(kChannels, h + 22);
    qToLittleEndian<quint32>(kRate, h + 24);
    qToLittleEndian<quint32>(kRate * kChannels * 4, h + 28);       // byte rate
    qToLittleEndian<quint16>(kChannels * 4, h + 32);               // block align
    qToLittleEndian<quint16>(32, h + 34);
    memcpy(h + 36, "data", 4);
    qToLittleEndian<quint32>(dataBytes, h + 40);

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(header) != header.size()
        || f.write(frames) != frames.size() || !f.commit()) {
        if (error) *error = QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

} // namespace TtsWav
} // namespace AetherSDR
