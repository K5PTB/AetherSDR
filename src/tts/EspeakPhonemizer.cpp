#include "tts/EspeakPhonemizer.h"

#include "tts/KokoroVocab.h"

#include <QRegularExpression>

#include <espeak-ng/speak_lib.h>

namespace AetherSDR {

namespace EspeakText {

namespace {

// phonemizer's default marks ;:,.!?¡¿—…"«»“”(){}[] — with ',' and '.' only
// counted as punctuation when not sitting between digits.
const QRegularExpression& marksRe()
{
    static const QRegularExpression re(QStringLiteral(
        "(\\s*(?:[;:!?¡¿—…\"«»“”(){}\\[\\]]|(?<![0-9])[,.]|[,.](?![0-9]))+\\s*)+"));
    return re;
}

} // namespace

void preservePunctuation(const QString& line, QStringList& chunks, QVector<Mark>& marks)
{
    chunks.clear();
    marks.clear();
    QVector<QRegularExpressionMatch> matches;
    for (auto it = marksRe().globalMatch(line); it.hasNext();)
        matches.append(it.next());
    if (matches.isEmpty()) {
        chunks << line;
        return;
    }
    if (matches.size() == 1 && matches.first().captured(0) == line) {
        marks.append({line, 'A'});
        return;
    }
    qsizetype cursor = 0;
    for (int i = 0; i < matches.size(); ++i) {
        const auto& m = matches[i];
        char position = 'I';
        if (i == 0 && m.capturedStart(0) == 0)
            position = 'B';
        else if (i == matches.size() - 1 && m.capturedEnd(0) == line.size())
            position = 'E';
        marks.append({m.captured(0), position});
        const QString prefix = line.mid(cursor, m.capturedStart(0) - cursor);
        if (!prefix.isEmpty())
            chunks << prefix;
        cursor = m.capturedEnd(0);
    }
    const QString tail = line.mid(cursor);
    if (!tail.isEmpty())
        chunks << tail;
}

QString restorePunctuation(QStringList text, QVector<Mark> marks)
{
    const QString sep = QStringLiteral(" ");
    QStringList out;
    int pos = 0;
    while (!text.isEmpty() || !marks.isEmpty()) {
        if (marks.isEmpty()) {
            for (QString line : text) {
                if (!line.endsWith(sep))
                    line += sep;
                out << line;
            }
            text.clear();
        } else if (text.isEmpty()) {
            QString joined;
            for (const Mark& m : marks)
                joined += m.text;
            out << joined;
            marks.clear();
        } else if (pos == 0) {
            const Mark mark = marks.takeFirst();
            if (text[0].endsWith(sep))
                text[0].chop(sep.size());
            const QString tail = mark.text.endsWith(sep) ? QString() : sep;
            switch (mark.position) {
            case 'B':
                text[0] = mark.text + text[0];
                break;
            case 'E':
                out << text.takeFirst() + mark.text + tail;
                ++pos;
                break;
            case 'A':
                out << mark.text + tail;
                ++pos;
                break;
            default:   // 'I'
                if (text.size() == 1) {
                    text[0] += mark.text;
                } else {
                    const QString first = text.takeFirst();
                    text[0] = first + mark.text + text[0];
                }
                break;
            }
        } else {
            out << text.takeFirst();
            ++pos;
        }
    }
    return out.join(QString());
}

QString postprocessEspeakLine(const QString& raw)
{
    QString line = raw.trimmed();
    line.replace(QLatin1Char('\n'), QLatin1Char(' '));
    line.replace(QStringLiteral("  "), QStringLiteral(" "));
    static const QRegularExpression runs(QStringLiteral("_+"));
    line.replace(runs, QStringLiteral("_"));
    line.replace(QStringLiteral("_ "), QStringLiteral(" "));
    if (line.isEmpty())
        return {};
    QString out;
    for (QString word : line.split(QLatin1Char(' '))) {
        word = word.trimmed();
        word.remove(QLatin1Char('_'));   // phone separator is empty
        out += word + QLatin1Char(' ');
    }
    return out;
}

QString keepKokoroSymbols(const QString& phonemes)
{
    QString kept;
    for (char32_t cp : phonemes.toUcs4()) {
        if (KokoroVocab::tokenFor(cp) >= 0)
            kept.append(QString::fromUcs4(&cp, 1));
    }
    return kept.simplified();
}

} // namespace EspeakText

EspeakPhonemizer& EspeakPhonemizer::instance()
{
    static EspeakPhonemizer phonemizer;
    return phonemizer;
}

bool EspeakPhonemizer::initialize(QString* error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized)
        return true;
    if (!m_initError.isEmpty()) {
        if (error) *error = m_initError;
        return false;
    }
    // DONT_EXIT: without it espeak-ng calls exit() when its data is missing,
    // taking the whole application down instead of reporting a failure.
    const int rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, nullptr,
                                       espeakINITIALIZE_DONT_EXIT);
    if (rate <= 0) {
        m_initError = QStringLiteral("espeak-ng could not start (is its data installed?)");
    } else if (espeak_SetVoiceByName("en-us") != EE_OK) {
        m_initError = QStringLiteral("espeak-ng has no en-us voice");
    } else {
        m_initialized = true;
        return true;
    }
    if (error) *error = m_initError;
    return false;
}

QString EspeakPhonemizer::espeakIpa(const QString& chunk)
{
    const QByteArray utf8 = chunk.toUtf8();
    const void* cursor = utf8.constData();
    QStringList clauses;
    // IPA, phonemes separated by '_' (phonemizer's non-tie mode).
    const int mode = (int('_') << 8) | 0x02;
    while (cursor) {
        const char* phonemes = espeak_TextToPhonemes(&cursor, espeakCHARS_UTF8, mode);
        if (phonemes && *phonemes)
            clauses << QString::fromUtf8(phonemes);
    }
    return clauses.join(QLatin1Char(' '));
}

QString EspeakPhonemizer::phonemize(const QString& text, QString* error)
{
    if (!initialize(error))
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    QStringList lines;
    for (const QString& line : text.trimmed().split(QLatin1Char('\n'))) {
        const QString trimmedLine = line.trimmed();
        if (trimmedLine.isEmpty())
            continue;
        QStringList chunks;
        QVector<EspeakText::Mark> marks;
        EspeakText::preservePunctuation(trimmedLine, chunks, marks);
        QStringList phonemized;
        for (const QString& chunk : chunks)
            phonemized << EspeakText::postprocessEspeakLine(espeakIpa(chunk));
        lines << EspeakText::restorePunctuation(phonemized, marks);
    }
    const QString result = EspeakText::keepKokoroSymbols(lines.join(QLatin1Char(' ')));
    if (result.isEmpty() && error)
        *error = QStringLiteral("The text produced no speakable sounds.");
    return result;
}

} // namespace AetherSDR
