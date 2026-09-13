#include "tts/KokoroSynth.h"

#include "asr/OrtPath.h"
#include "tts/KokoroVocab.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace AetherSDR {

struct KokoroSynth::Session {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "AetherSDR-TTS"};
    std::unique_ptr<Ort::Session> session;
};

KokoroSynth::KokoroSynth() = default;
KokoroSynth::~KokoroSynth() = default;

bool KokoroSynth::isLoaded() const
{
    return m_session && m_session->session;
}

bool KokoroSynth::load(const QString& modelPath, const QString& voicesPath, QString* error)
{
    if (!m_voices.load(voicesPath, error))
        return false;
    try {
        auto s = std::make_unique<Session>();
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(int(std::clamp(std::thread::hardware_concurrency(), 1u, 4u)));
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const std::string path = modelPath.toStdString();
        s->session = std::make_unique<Ort::Session>(s->env, asr_detail::toOrtPath(path).c_str(), opts);
        m_session = std::move(s);
    } catch (const Ort::Exception& e) {
        if (error) *error = QStringLiteral("Cannot load the speech model: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

QVector<qint64> KokoroSynth::tokenize(const QString& phonemes)
{
    QVector<qint64> ids;
    for (char32_t cp : phonemes.toUcs4()) {
        const int id = KokoroVocab::tokenFor(cp);
        if (id >= 0)
            ids.append(id);
    }
    return ids;
}

bool KokoroSynth::synthesize(const QString& phonemes, const QString& voice, float speed,
                             QVector<float>& audio, QString* error)
{
    audio.clear();
    if (!isLoaded()) {
        if (error) *error = QStringLiteral("The speech model is not loaded.");
        return false;
    }
    const QVector<qint64> ids = tokenize(phonemes);
    if (ids.isEmpty()) {
        if (error) *error = QStringLiteral("The text produced no speakable sounds.");
        return false;
    }
    if (ids.size() > kMaxTokens) {
        if (error) *error = QStringLiteral("The message is too long to speak in one go "
                                           "(%1 sounds, at most %2) — shorten it.")
                                .arg(ids.size()).arg(kMaxTokens);
        return false;
    }
    QVector<float> style = m_voices.style(voice, int(ids.size()), error);
    if (style.isEmpty())
        return false;

    std::vector<int64_t> tokens;
    tokens.reserve(size_t(ids.size()) + 2);
    tokens.push_back(0);   // pad
    tokens.insert(tokens.end(), ids.cbegin(), ids.cend());
    tokens.push_back(0);   // pad
    std::array<float, 1> speedValue{std::clamp(speed, 0.5f, 2.0f)};

    try {
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 2> tokenShape{1, int64_t(tokens.size())};
        const std::array<int64_t, 2> styleShape{1, KokoroVoices::kStyleDim};
        const std::array<int64_t, 1> speedShape{1};
        std::array<Ort::Value, 3> inputs{
            Ort::Value::CreateTensor<int64_t>(mem, tokens.data(), tokens.size(),
                                              tokenShape.data(), tokenShape.size()),
            Ort::Value::CreateTensor<float>(mem, style.data(), size_t(style.size()),
                                            styleShape.data(), styleShape.size()),
            Ort::Value::CreateTensor<float>(mem, speedValue.data(), speedValue.size(),
                                            speedShape.data(), speedShape.size()),
        };
        const std::array<const char*, 3> inNames{"tokens", "style", "speed"};
        const std::array<const char*, 1> outNames{"audio"};
        auto outputs = m_session->session->Run(Ort::RunOptions{nullptr}, inNames.data(),
                                               inputs.data(), inputs.size(),
                                               outNames.data(), outNames.size());
        if (outputs.empty() || !outputs[0].IsTensor()) {
            if (error) *error = QStringLiteral("The speech model returned no audio.");
            return false;
        }
        const size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        const float* samples = outputs[0].GetTensorData<float>();
        audio.resize(qsizetype(count));
        std::copy(samples, samples + count, audio.begin());
    } catch (const Ort::Exception& e) {
        if (error) *error = QStringLiteral("Speech synthesis failed: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
    for (float& s : audio) {
        if (!std::isfinite(s))
            s = 0.0f;   // never hand NaN/Inf toward the transmitter
    }
    trimSilence(audio);
    if (audio.isEmpty()) {
        if (error) *error = QStringLiteral("The speech model produced only silence.");
        return false;
    }
    // The model stops right after the last sound. Without a cushion the final
    // syllable is lost to whatever plays it: an audio device still holding its
    // buffer when playback is declared finished (~200 ms on Bluetooth), or an
    // unkey that lands a little early.
    audio.append(QVector<float>(qsizetype(kSampleRate) * kTrailingSilenceMs / 1000, 0.0f));
    return true;
}

void KokoroSynth::trimSilence(QVector<float>& audio, int sampleRate)
{
    const int frame = std::max(1, sampleRate / 100);         // 10 ms
    const qsizetype frames = audio.size() / frame;
    if (frames == 0) {
        audio.clear();
        return;
    }
    QVector<float> rms(frames);
    float peak = 0.0f;
    for (qsizetype f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (int i = 0; i < frame; ++i) {
            const float s = audio[f * frame + i];
            sum += double(s) * s;
        }
        rms[f] = float(std::sqrt(sum / frame));
        peak = std::max(peak, rms[f]);
    }
    const float threshold = std::max(peak * 0.01f, 1e-4f);   // 40 dB below the loudest frame
    qsizetype first = 0;
    while (first < frames && rms[first] < threshold)
        ++first;
    if (first == frames) {
        audio.clear();
        return;
    }
    qsizetype last = frames - 1;
    while (last > first && rms[last] < threshold)
        --last;
    // Keep a little more after the speech than before it: word endings (a
    // trailing "s", a released "t") fade below the threshold while still audible.
    constexpr qsizetype kLeadMarginFrames = 5;                 // 50 ms
    constexpr qsizetype kTailMarginFrames = 15;                // 150 ms
    const qsizetype start = std::max<qsizetype>(0, first - kLeadMarginFrames) * frame;
    const qsizetype end = std::min<qsizetype>(audio.size(), (last + 1 + kTailMarginFrames) * frame);
    audio = audio.mid(start, end - start);
}

} // namespace AetherSDR
