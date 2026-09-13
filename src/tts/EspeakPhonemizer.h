#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <mutex>

namespace AetherSDR {

// Text -> IPA phonemes the way Kokoro was trained to read them: espeak-ng
// (en-us) through the `phonemizer` package with preserve_punctuation and
// with_stress, then filtered to Kokoro's vocabulary with whitespace collapsed
// (kokoro_onnx Tokenizer.phonemize). A mismatch here is not cosmetic — the
// model mispronounces or runs words together — so the pieces below are ports of
// phonemizer's own steps, checked against its output in tts_phonemizer_test.
namespace EspeakText {

// A punctuation run cut out of a line, with where it sat.
struct Mark {
    QString text;     // the matched run, including surrounding spaces
    char position;    // 'B'egin, 'I'nside, 'E'nd, or 'A'lone (the whole line)
};

// phonemizer Punctuation.preserve(): the line's text chunks between marks
// (empty chunks dropped) and the marks themselves. A '.' or ',' between two
// digits is part of a number, not punctuation ("19.99").
void preservePunctuation(const QString& line, QStringList& chunks, QVector<Mark>& marks);

// phonemizer Punctuation.restore() for one line, joined into one string.
QString restorePunctuation(QStringList phonemizedChunks, QVector<Mark> marks);

// phonemizer EspeakBackend._postprocess_line(): espeak's "_"-separated IPA
// clauses become words with stress kept, each followed by one space.
QString postprocessEspeakLine(const QString& raw);

// kokoro_onnx: drop symbols the model lacks, collapse whitespace, trim.
QString keepKokoroSymbols(const QString& phonemes);

} // namespace EspeakText

// espeak-ng holds process-global state, so every call is serialized.
class EspeakPhonemizer {
public:
    static EspeakPhonemizer& instance();

    // Idempotent. False (with a reason) when espeak-ng or its data is missing.
    bool initialize(QString* error);

    // The full pipeline above. Empty with *error set on failure.
    QString phonemize(const QString& text, QString* error);

private:
    EspeakPhonemizer() = default;
    QString espeakIpa(const QString& chunk);   // caller holds m_mutex

    std::mutex m_mutex;
    bool m_initialized{false};
    QString m_initError;
};

} // namespace AetherSDR
