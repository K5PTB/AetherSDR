#include "tts/TextToSpeechEngine.h"

#include "asr/AsrModelManager.h"
#include "tts/EspeakPhonemizer.h"
#include "tts/KokoroSynth.h"
#include "tts/TtsModelCatalog.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>

#include <mutex>

namespace AetherSDR {

struct TextToSpeechEngine::Worker {
    std::mutex mutex;   // one synthesis at a time on the pool
    KokoroSynth synth;
};

namespace {

struct SynthResult {
    bool ok{false};
    QVector<float> audio;
    QString error;
};

constexpr int kFileCount = 2;

QString megabytes(qint64 bytes)
{
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 0);
}

} // namespace

QString TextToSpeechEngine::voiceName(Voice voice)
{
    return voice == Voice::Male ? QStringLiteral("am_michael") : QStringLiteral("af_heart");
}

TextToSpeechEngine::TextToSpeechEngine(QObject* parent)
    : TextToSpeechEngine(AsrModelManager::defaultModelsDir(), nullptr, parent)
{
}

TextToSpeechEngine::TextToSpeechEngine(const QString& modelsDir, QNetworkAccessManager* nam,
                                       QObject* parent)
    : QObject(parent)
    , m_worker(std::make_shared<Worker>())
    , m_models(new AsrModelManager(modelsDir, nam, this))
{
    connect(m_models, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        const QString what = fileAt(m_fileIndex).displayName.toLower();
        emit statusChanged(total > 0
            ? QStringLiteral("Downloading the %1 (first use): %2 of %3 MB")
                  .arg(what, megabytes(got), megabytes(total))
            : QStringLiteral("Downloading the %1 (first use): %2 MB").arg(what, megabytes(got)));
    });
    connect(m_models, &AsrModelManager::verifying, this, [this] {
        emit statusChanged(QStringLiteral("Checking the %1…").arg(fileAt(m_fileIndex).displayName.toLower()));
    });
    const auto fileReady = [this](const QString&) {
        if (!m_busy)
            return;
        ++m_fileIndex;
        // Queued: the manager is still unwinding the signal that got us here.
        QTimer::singleShot(0, this, [this, job = m_job] {
            if (m_busy && job == m_job)
                ensureNextFile();
        });
    };
    connect(m_models, &AsrModelManager::alreadyPresent, this, fileReady);
    connect(m_models, &AsrModelManager::finished, this, fileReady);
    connect(m_models, &AsrModelManager::failed, this, [this](const QString& error) {
        if (!m_busy)
            return;
        finish();
        emit failed(QStringLiteral("Could not get the %1: %2")
                        .arg(fileAt(m_fileIndex).displayName.toLower(), error));
    });
}

TextToSpeechEngine::~TextToSpeechEngine() = default;

const AsrModelTier& TextToSpeechEngine::fileAt(int index) const
{
    return index <= 0 ? TtsModelCatalog::kokoroModel() : TtsModelCatalog::kokoroVoices();
}

void TextToSpeechEngine::generate(const QString& text, Voice voice)
{
    if (m_busy) {
        emit failed(QStringLiteral("Speech is already being generated."));
        return;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        emit failed(QStringLiteral("Type the message to speak first."));
        return;
    }
    m_busy = true;
    ++m_job;
    m_text = trimmed;
    m_voice = voice;
    m_fileIndex = 0;
    ensureNextFile();
}

void TextToSpeechEngine::ensureNextFile()
{
    if (m_fileIndex >= kFileCount) {
        startSynthesis();
        return;
    }
    m_models->ensure(fileAt(m_fileIndex));
}

void TextToSpeechEngine::startSynthesis()
{
    const bool firstUse = !m_worker->synth.isLoaded();
    emit statusChanged(firstUse ? QStringLiteral("Starting the speech engine (first use)…")
                                : QStringLiteral("Generating speech…"));
    const QString modelPath = m_models->modelPath(TtsModelCatalog::kokoroModel());
    const QString voicesPath = m_models->modelPath(TtsModelCatalog::kokoroVoices());
    const QString text = m_text;
    const QString voice = voiceName(m_voice);
    const std::shared_ptr<Worker> worker = m_worker;
    const QPointer<TextToSpeechEngine> self(this);
    const quint64 job = m_job;

    auto* watcher = new QFutureWatcher<SynthResult>(this);
    connect(watcher, &QFutureWatcher<SynthResult>::finished, this, [this, watcher, job] {
        const SynthResult result = watcher->result();
        watcher->deleteLater();
        if (!m_busy || job != m_job)
            return;   // canceled, or superseded
        finish();
        if (result.ok)
            emit generated(result.audio);
        else
            emit failed(result.error);
    });
    watcher->setFuture(QtConcurrent::run([worker, modelPath, voicesPath, text, voice, self, job] {
        SynthResult r;
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (!worker->synth.isLoaded()) {
            if (!EspeakPhonemizer::instance().initialize(&r.error)
                || !worker->synth.load(modelPath, voicesPath, &r.error)) {
                return r;
            }
            QMetaObject::invokeMethod(self, [self, job] {
                if (self && self->m_busy && job == self->m_job)
                    emit self->statusChanged(QStringLiteral("Generating speech…"));
            }, Qt::QueuedConnection);
        }
        const QString phonemes = EspeakPhonemizer::instance().phonemize(text, &r.error);
        if (phonemes.isEmpty())
            return r;
        r.ok = worker->synth.synthesize(phonemes, voice, 1.0f, r.audio, &r.error);
        return r;
    }));
}

void TextToSpeechEngine::cancel()
{
    if (!m_busy)
        return;
    m_models->cancel();
    finish();
    emit failed(QStringLiteral("Canceled."));
}

void TextToSpeechEngine::finish()
{
    m_busy = false;
    ++m_job;
}

} // namespace AetherSDR
