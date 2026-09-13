// End-to-end text-to-speech with the real Kokoro model (RFC #4334). The model
// files are a ~115 MB download and are not in the tree, so this runs only when
// AETHER_TTS_MODELS_DIR points at a folder holding kokoro-v1.0.int8.onnx and
// voices-v1.0.bin; otherwise it reports SKIPPED and passes.
// Run: AETHER_TTS_MODELS_DIR=/path ./build/tts_kokoro_smoke_test [out.wav]

#include "tts/EspeakPhonemizer.h"
#include "tts/KokoroSynth.h"
#include "tts/TextToSpeechEngine.h"
#include "tts/TtsWav.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString dir = qEnvironmentVariable("AETHER_TTS_MODELS_DIR");
    const QString model = dir + "/kokoro-v1.0.int8.onnx";
    const QString voices = dir + "/voices-v1.0.bin";
    if (dir.isEmpty() || !QFile::exists(model) || !QFile::exists(voices)) {
        std::printf("SKIPPED: set AETHER_TTS_MODELS_DIR to a folder with the Kokoro files\n");
        return 0;
    }

    int failed = 0;
    const auto report = [&failed](const char* name, bool ok, const QString& detail = {}) {
        std::printf("%s %-50s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.toUtf8().constData());
        if (!ok) ++failed;
    };

    QString error;
    KokoroSynth synth;
    QElapsedTimer t;
    t.start();
    report("model loads", synth.load(model, voices, &error), QStringLiteral("%1 ms %2").arg(t.elapsed()).arg(error));

    const QString text = QStringLiteral("CQ contest, CQ contest, this is Kilo Five Papa Tango Bravo, contest.");
    const QString phonemes = EspeakPhonemizer::instance().phonemize(text, &error);
    report("text phonemized", !phonemes.isEmpty(), error);

    for (const auto voice : {TextToSpeechEngine::Voice::Female, TextToSpeechEngine::Voice::Male}) {
        QVector<float> audio;
        t.restart();
        const bool ok = synth.synthesize(phonemes, TextToSpeechEngine::voiceName(voice), 1.0f, audio, &error);
        const double seconds = audio.size() / double(KokoroSynth::kSampleRate);
        double peak = 0.0;
        for (float s : audio)
            peak = std::max(peak, double(std::fabs(s)));
        // The message ends in a cushion of silence, so the last syllable survives playback.
        double tailPeak = 1.0;
        const qsizetype tail = qsizetype(KokoroSynth::kSampleRate) * (KokoroSynth::kTrailingSilenceMs - 50) / 1000;
        if (audio.size() > tail) {
            tailPeak = 0.0;
            for (qsizetype i = audio.size() - tail; i < audio.size(); ++i)
                tailPeak = std::max(tailPeak, double(std::fabs(audio[i])));
        }
        report(voice == TextToSpeechEngine::Voice::Female ? "female voice ends in silence" : "male voice ends in silence",
               ok && tailPeak == 0.0, QStringLiteral("tail peak %1").arg(tailPeak));
        report(voice == TextToSpeechEngine::Voice::Female ? "female voice speaks" : "male voice speaks",
               ok && seconds > 2.0 && seconds < 12.0 && peak > 0.05,
               QStringLiteral("%1 s audio in %2 ms, peak %3 %4").arg(seconds, 0, 'f', 2)
                   .arg(t.elapsed()).arg(peak, 0, 'f', 2).arg(error));
        if (ok && argc > 1) {
            const QString out = QString::fromLocal8Bit(argv[1]).replace(".wav",
                voice == TextToSpeechEngine::Voice::Female ? "-female.wav" : "-male.wav");
            report("WAV written", TtsWav::write(out, audio, &error), out);
        }
    }

    std::printf("\n%s (%d failed)\n", failed ? "FAILED" : "PASSED", failed);
    return failed ? 1 : 0;
}
