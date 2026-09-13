// Text-to-speech phonemizer (RFC #4334): the C++ port of phonemizer's espeak
// pipeline must produce exactly what Kokoro was trained on. The golden strings
// and token ids below were captured from kokoro_onnx 's Tokenizer (phonemizer +
// espeak-ng 1.52, en-us) — a difference here means mispronounced speech.
// Run: ./build/tts_phonemizer_test

#include "tts/EspeakPhonemizer.h"
#include "tts/KokoroSynth.h"
#include "tts/KokoroVocab.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

std::string s(const QString& q) { return q.toStdString(); }

struct Golden {
    const char* text;
    const char* phonemes;
    std::vector<qint64> tokens;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    report("vocabulary has Kokoro's 114 symbols", KokoroVocab::size() == 114,
           std::to_string(KokoroVocab::size()));

    // ── Pure steps ──────────────────────────────────────────────────────────
    {
        QStringList chunks;
        QVector<EspeakText::Mark> marks;
        EspeakText::preservePunctuation(QStringLiteral("hello, my world!"), chunks, marks);
        report("punctuation split around marks",
               chunks == QStringList({"hello", "my world"}) && marks.size() == 2
                   && marks[0].text == ", " && marks[0].position == 'I'
                   && marks[1].text == "!" && marks[1].position == 'E',
               s(chunks.join("|")));

        EspeakText::preservePunctuation(QStringLiteral("19.99 dollars"), chunks, marks);
        report("a decimal point is not punctuation",
               chunks == QStringList({"19.99 dollars"}) && marks.isEmpty(), s(chunks.join("|")));

        EspeakText::preservePunctuation(QStringLiteral("(maybe) yes"), chunks, marks);
        report("a leading mark is placed at the beginning",
               marks.size() == 2 && marks[0].position == 'B' && chunks.first() == "maybe",
               s(chunks.join("|")));

        EspeakText::preservePunctuation(QStringLiteral("?!"), chunks, marks);
        report("a line of only marks is one Alone mark",
               chunks.isEmpty() && marks.size() == 1 && marks[0].position == 'A');

        const QString restored = EspeakText::restorePunctuation(
            {QStringLiteral("həloʊ "), QStringLiteral("maɪ wɜːld ")},
            {{QStringLiteral(", "), 'I'}, {QStringLiteral("!"), 'E'}});
        report("marks restored tight against the word before",
               restored == QStringLiteral("həloʊ, maɪ wɜːld! "), s(restored));

        const QString post = EspeakText::postprocessEspeakLine(QStringLiteral("h_ə_l_ˈoʊ__ w_ˈɜː_l_d_"));
        report("espeak phone separators removed, stress kept",
               post == QStringLiteral("həlˈoʊ wˈɜːld "), s(post));

        const QString kept = EspeakText::keepKokoroSymbols(QStringLiteral("  a§b   c  "));
        report("symbols outside the vocabulary dropped, spaces collapsed",
               kept == QStringLiteral("ab c"), s(kept));
    }

    // ── The whole pipeline, against the reference implementation ────────────
    const std::vector<Golden> goldens = {
        {"CQ contest, CQ contest, this is K5PTB.",
         "sˌiːkjˈuː kˈɑːntɛst, sˌiːkjˈuː kˈɑːntɛst, ðɪs ɪz kˈeɪ fˈaɪv pˌiːtˌiːbˈiː.",
         {61, 157, 51, 158, 53, 52, 156, 63, 158, 16, 53, 156, 69, 158, 56, 62, 86, 61, 62, 3, 16, 61, 157, 51, 158, 53, 52, 156, 63, 158, 16, 53, 156, 69, 158, 56, 62, 86, 61, 62, 3, 16, 81, 102, 61, 16, 102, 68, 16, 53, 156, 47, 102, 16, 48, 156, 43, 102, 64, 16, 58, 157, 51, 158, 62, 157, 51, 158, 44, 156, 51, 158, 4}},
        {"Five nine, Texas. Five nine, Texas.",
         "fˈaɪv nˈaɪn, tˈɛksəs. fˈaɪv nˈaɪn, tˈɛksəs.",
         {48, 156, 43, 102, 64, 16, 56, 156, 43, 102, 56, 3, 16, 62, 156, 86, 53, 61, 83, 61, 4, 16, 48, 156, 43, 102, 64, 16, 56, 156, 43, 102, 56, 3, 16, 62, 156, 86, 53, 61, 83, 61, 4}},
        {"Your report is five niner; QTH is grid EM10!",
         "jʊɹ ɹᵻpˈɔːɹt ɪz fˈaɪv nˈaɪnɚ; kjˌuːtˌiːˈeɪtʃ ɪz ɡɹˈɪd ˌiːˈɛm tˈɛn!",
         {52, 135, 123, 16, 123, 177, 58, 156, 76, 158, 123, 62, 16, 102, 68, 16, 48, 156, 43, 102, 64, 16, 56, 156, 43, 102, 56, 85, 1, 16, 53, 52, 157, 63, 158, 62, 157, 51, 158, 156, 47, 102, 62, 131, 16, 102, 68, 16, 92, 123, 156, 102, 46, 16, 157, 51, 158, 156, 86, 55, 16, 62, 156, 86, 56, 5}},
        {"Kilo Five Papa Tango Bravo",
         "kˈiːloʊ fˈaɪv pˈɑːpə tˈæŋɡoʊ bɹˈɑːvoʊ",
         {53, 156, 51, 158, 54, 57, 135, 16, 48, 156, 43, 102, 64, 16, 58, 156, 69, 158, 58, 83, 16, 62, 156, 72, 112, 92, 57, 135, 16, 44, 123, 156, 69, 158, 64, 57, 135}},
        {"QRZ?",
         "kjˌuːˌɑːɹzˈiː?",
         {53, 52, 157, 63, 158, 157, 69, 158, 123, 68, 156, 51, 158, 6}},
        {"Price is 19.99 dollars, (maybe) more...",
         "pɹˈaɪs ɪz nˈaɪntiːn pɔɪnt nˈaɪn nˈaɪn dˈɑːlɚz, (mˈeɪbiː) mˈɔːɹ...",
         {58, 123, 156, 43, 102, 61, 16, 102, 68, 16, 56, 156, 43, 102, 56, 62, 51, 158, 56, 16, 58, 76, 102, 56, 62, 16, 56, 156, 43, 102, 56, 16, 56, 156, 43, 102, 56, 16, 46, 156, 69, 158, 54, 85, 68, 3, 16, 12, 55, 156, 47, 102, 44, 51, 158, 13, 16, 55, 156, 76, 158, 123, 4, 4, 4}},
    };

    QString initError;
    const bool ready = EspeakPhonemizer::instance().initialize(&initError);
    report("espeak-ng starts", ready, s(initError));
    if (ready) {
        int index = 0;
        for (const Golden& g : goldens) {
            ++index;
            QString error;
            const QString got = EspeakPhonemizer::instance().phonemize(QString::fromUtf8(g.text), &error);
            const QString want = QString::fromUtf8(g.phonemes);
            const std::string label = "phonemes match the reference #" + std::to_string(index);
            report(label.c_str(), got == want,
                   got == want ? std::string() : "got \"" + s(got) + "\" want \"" + s(want) + "\"");

            const QVector<qint64> tokens = KokoroSynth::tokenize(got);
            const std::string tlabel = "token ids match the reference #" + std::to_string(index);
            report(tlabel.c_str(),
                   tokens == QVector<qint64>(g.tokens.begin(), g.tokens.end()),
                   std::to_string(tokens.size()) + " vs " + std::to_string(g.tokens.size()));
        }
        QString error;
        const QString none = EspeakPhonemizer::instance().phonemize(QStringLiteral("   "), &error);
        report("blank text gives nothing and says why", none.isEmpty() && !error.isEmpty(), s(error));
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
