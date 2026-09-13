#pragma once

#include "tts/KokoroVoices.h"

#include <QString>
#include <QVector>

#include <memory>

namespace AetherSDR {

// Kokoro-82M (v1.0, ONNX, Apache-2.0) speech synthesis: phonemes in, 24 kHz
// mono float audio out. Load once; synthesize() is safe to call repeatedly from
// one worker thread at a time.
class KokoroSynth {
public:
    static constexpr int kSampleRate = 24000;
    static constexpr int kMaxTokens = 510;   // the model's context, pad tokens excluded
    static constexpr int kTrailingSilenceMs = 500;   // appended after the speech

    KokoroSynth();
    ~KokoroSynth();

    bool load(const QString& modelPath, const QString& voicesPath, QString* error);
    bool isLoaded() const;

    bool synthesize(const QString& phonemes, const QString& voice, float speed,
                    QVector<float>& audio, QString* error);

    // Vocabulary ids for the phonemes; symbols the model lacks are dropped.
    static QVector<qint64> tokenize(const QString& phonemes);

    // Cut the near-silence the model leaves at either end, keeping 50 ms before
    // the speech and 150 ms after it so neither end is clipped.
    static void trimSilence(QVector<float>& audio, int sampleRate = kSampleRate);

private:
    struct Session;
    std::unique_ptr<Session> m_session;
    KokoroVoices m_voices;
};

} // namespace AetherSDR
