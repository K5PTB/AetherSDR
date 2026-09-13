// Text-to-speech building blocks (RFC #4334): the voices file reader, the
// silence trim and the WAV writer — no model, no network.
// Run: ./build/kokoro_voices_test

#include "core/ZipArchive.h"
#include "tts/KokoroSynth.h"
#include "tts/KokoroVocab.h"
#include "tts/KokoroVoices.h"
#include "tts/TtsWav.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// A (rows, 1, dim) float32 .npy whose value at [r][c] is r * 1000 + c.
QByteArray npy(int rows, int dim, const char* descr = "<f4", const char* shapeFormat = "(%d, 1, %d)")
{
    char shape[64];
    std::snprintf(shape, sizeof shape, shapeFormat, rows, dim);
    QByteArray header = QByteArray("{'descr': '") + descr + "', 'fortran_order': False, 'shape': "
                        + shape + ", }";
    while ((10 + header.size() + 1) % 64 != 0)
        header += ' ';
    header += '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    char len[2];
    qToLittleEndian<quint16>(quint16(header.size()), len);
    out.append(len, 2);
    out += header;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < dim; ++c) {
            char v[4];
            qToLittleEndian<float>(float(r * 1000 + c), v);
            out.append(v, 4);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Voices file ─────────────────────────────────────────────────────────
    {
        KokoroVoices voices;
        QString error;
        const bool loaded = voices.loadFromZipBytes(
            writeStoredZip({{QStringLiteral("af_test.npy"), npy(3, 256)},
                            {QStringLiteral("readme.txt"), QByteArray("ignored")}}),
            &error);
        report("voices archive loads its .npy entries",
               loaded && voices.contains("af_test") && voices.names().size() == 1, error.toStdString());

        const QVector<float> one = voices.style("af_test", 1, &error);
        const QVector<float> two = voices.style("af_test", 2, &error);
        const QVector<float> many = voices.style("af_test", 999, &error);
        report("style row follows the token count (n tokens -> row n-1, clamped)",
               one.size() == 256 && one[0] == 0.0f && one[255] == 255.0f
                   && two[0] == 1000.0f && many[0] == 2000.0f,
               error.toStdString());

        error.clear();
        report("an unknown voice is refused with a reason",
               voices.style("zz_none", 5, &error).isEmpty() && error.contains("zz_none"));

        int rows = 0, dim = 0;
        QVector<float> data;
        report("float64 data is refused",
               !KokoroVoices::parseNpy(npy(3, 256, "<f8"), rows, dim, data, &error));
        report("a flat (rows, dim) shape is refused",
               !KokoroVoices::parseNpy(npy(3, 256, "<f4", "(%d, %d)"), rows, dim, data, &error));
        report("truncated data is refused",
               !KokoroVoices::parseNpy(npy(3, 256).left(200), rows, dim, data, &error));
        report("a non-archive voices file is refused",
               !KokoroVoices().loadFromZipBytes(QByteArray("not a zip at all"), &error));

        KokoroVoices narrow;
        narrow.loadFromZipBytes(writeStoredZip({{QStringLiteral("af_narrow.npy"), npy(2, 128)}}), &error);
        error.clear();
        report("a style width other than 256 is refused",
               narrow.style("af_narrow", 1, &error).isEmpty() && error.contains("256"), error.toStdString());
    }

    // ── Tokens ──────────────────────────────────────────────────────────────
    {
        const QVector<qint64> ids = KokoroSynth::tokenize(QStringLiteral("hə§ˈ"));
        report("tokenize maps symbols and drops unknown ones",
               ids == QVector<qint64>({KokoroVocab::tokenFor(U'h'), KokoroVocab::tokenFor(U'ə'),
                                       KokoroVocab::tokenFor(U'ˈ')})
                   && KokoroVocab::tokenFor(U'§') == -1);
    }

    // ── Silence trim ────────────────────────────────────────────────────────
    {
        QVector<float> audio(24000 / 2, 0.0f);                 // 0.5 s silence
        for (int i = 0; i < 24000 / 5; ++i)                     // 0.2 s tone
            audio.append(0.5f * float(std::sin(2 * M_PI * 440.0 * i / 24000.0)));
        audio.append(QVector<float>(24000 / 2, 0.0f));          // 0.5 s silence
        KokoroSynth::trimSilence(audio);
        const int ms = int(audio.size() * 1000 / 24000);
        report("silence trimmed to the speech plus 50 ms before and 150 ms after",
               ms >= 380 && ms <= 420, std::to_string(ms) + " ms");

        QVector<float> silent(24000, 0.0f);
        KokoroSynth::trimSilence(silent);
        report("all-silent audio trims to nothing", silent.isEmpty());
    }

    // ── WAV writer ──────────────────────────────────────────────────────────
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/speech.wav";
        QVector<float> mono(24000 / 2);
        for (int i = 0; i < mono.size(); ++i)
            mono[i] = 0.25f * float(std::sin(2 * M_PI * 600.0 * i / 24000.0));
        QString error;
        const bool wrote = TtsWav::write(path, mono, &error);
        QFile f(path);
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        const bool header = bytes.size() > 44 && bytes.startsWith("RIFF")
            && bytes.mid(8, 8) == "WAVEfmt "
            && qFromLittleEndian<quint16>(bytes.constData() + 20) == 3
            && qFromLittleEndian<quint16>(bytes.constData() + 22) == 2
            && qFromLittleEndian<quint32>(bytes.constData() + 24) == 48000
            && qFromLittleEndian<quint16>(bytes.constData() + 34) == 32;
        const quint32 dataBytes = bytes.size() > 44 ? qFromLittleEndian<quint32>(bytes.constData() + 40) : 0;
        report("WAV is 48 kHz float stereo, the format both keyers accept",
               wrote && header && dataBytes == 48000u / 2 * 2 * 4 && bytes.size() == 44 + int(dataBytes),
               error.toStdString() + " data=" + std::to_string(dataBytes));
        report("duration helper", TtsWav::durationMs(mono) == 500);
        report("empty speech is not written", !TtsWav::write(dir.path() + "/none.wav", {}, &error));
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
