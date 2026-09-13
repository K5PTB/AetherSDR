#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QNetworkAccessManager;

namespace AetherSDR {

class AsrModelManager;
struct AsrModelTier;

// Text in, speech out, for the voice keyer (RFC #4334). The first generate()
// downloads and verifies the model files, then starts espeak-ng and the ONNX
// model; later calls go straight to synthesis. Downloading and synthesis never
// block the UI thread; progress arrives through statusChanged(). One request
// at a time; exactly one generated() or failed() ends each accepted request.
class TextToSpeechEngine : public QObject {
    Q_OBJECT
public:
    enum class Voice { Female, Male };
    Q_ENUM(Voice)

    // The Kokoro voice behind each choice.
    static QString voiceName(Voice voice);

    explicit TextToSpeechEngine(QObject* parent = nullptr);
    // Tests: explicit models folder and network manager.
    TextToSpeechEngine(const QString& modelsDir, QNetworkAccessManager* nam, QObject* parent = nullptr);
    ~TextToSpeechEngine() override;

    bool isBusy() const { return m_busy; }
    void generate(const QString& text, Voice voice);
    // Stop a download in progress. A synthesis already running finishes, but
    // its result is discarded.
    void cancel();

signals:
    void statusChanged(const QString& message);
    void generated(const QVector<float>& speech24kMono);
    void failed(const QString& message);

private:
    void ensureNextFile();
    void startSynthesis();
    void finish();
    const AsrModelTier& fileAt(int index) const;

    struct Worker;
    std::shared_ptr<Worker> m_worker;
    AsrModelManager* m_models{nullptr};
    QString m_text;
    Voice m_voice{Voice::Female};
    int m_fileIndex{0};
    bool m_busy{false};
    quint64 m_job{0};
};

} // namespace AetherSDR
