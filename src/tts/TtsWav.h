#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace AetherSDR {
namespace TtsWav {

static constexpr int kRate = 48000;
static constexpr int kChannels = 2;

// 24 kHz mono speech as 48 kHz stereo float32 frames (interleaved).
QByteArray upsampleToStereo48k(const QVector<float>& mono24k);

// Write speech as a 48 kHz, 32-bit float, stereo WAV — the one format both
// voice keyers take: the radio DVK upload accepts only that, and the local
// keyer converts whatever it imports.
bool write(const QString& path, const QVector<float>& mono24k, QString* error);

// Length of the written file's audio, for the radio DVK's size limit.
inline int durationMs(const QVector<float>& mono24k) { return int(mono24k.size() * 1000LL / 24000); }

} // namespace TtsWav
} // namespace AetherSDR
