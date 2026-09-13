#pragma once

#include "core/VoiceKeyerSource.h"

#include <QDialog>
#include <QPointer>
#include <QTemporaryDir>
#include <QVector>

#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace AetherSDR {

class TextToSpeechEngine;
class VoiceKeyer;

// "Text to Speech…" from a DVK slot's menu (RFC #4334): type a message, pick a
// voice, Generate, review it with PLAY, then Save to Slot. Saving goes through
// the keyer's own Import WAV, so it lands on the radio's DVK or in the local
// recordings exactly as an imported file would. Nothing touches the slot until
// Save, and nothing is ever transmitted from here.
class TextToSpeechDialog : public QDialog {
    Q_OBJECT
public:
    using PreviewStart = std::function<bool(const QString& path, QString& error)>;
    using PreviewStop = std::function<void()>;

    TextToSpeechDialog(TextToSpeechEngine* engine, PreviewStart previewStart,
                       PreviewStop previewStop, QWidget* parent = nullptr);
    ~TextToSpeechDialog() override;

    // Which keyer and slot Save writes to; the dialog is reused per slot.
    void setTarget(VoiceKeyer* keyer, int slot);

    // A short slot label from a message: its start, cut at a word, with an
    // ellipsis, without quotes. The full message is stored separately.
    static QString slotNameFor(const QString& text);

    // Radio DVK slots take at most 5 MB of 48 kHz stereo float — about 13.6 s.
    static constexpr int kRadioMaxMs = 13600;

public slots:
    void onPreviewFinished();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void generate();
    void togglePlay();
    void save();
    void onGenerated(const QVector<float>& speech);
    void onFailed(const QString& message);
    void onTransferFinished(bool ok, const QString& message);
    void invalidateSpeech();
    void setStatus(const QString& text, bool error = false);
    void refreshButtons();
    static VoiceKeyerSource sourceOf(const VoiceKeyer* keyer);

    QPointer<TextToSpeechEngine> m_engine;
    QPointer<VoiceKeyer> m_keyer;
    QMetaObject::Connection m_transferConnection;
    int m_slot{1};
    PreviewStart m_previewStart;
    PreviewStop m_previewStop;

    QLabel* m_title{nullptr};
    QPlainTextEdit* m_text{nullptr};
    QComboBox* m_voice{nullptr};
    QPushButton* m_generateBtn{nullptr};
    QPushButton* m_playBtn{nullptr};
    QPushButton* m_saveBtn{nullptr};
    QLabel* m_status{nullptr};

    QTemporaryDir m_tempDir;
    QVector<float> m_speech;          // last generated, 24 kHz mono
    QString m_speechText;             // the text it was generated from
    int m_speechVoice{-1};
    bool m_generating{false};
    bool m_playing{false};
    bool m_saving{false};
    bool m_statusIsError{false};
};

} // namespace AetherSDR
