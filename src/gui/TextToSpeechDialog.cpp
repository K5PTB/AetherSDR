#include "TextToSpeechDialog.h"

#include "core/ThemeManager.h"
#include "core/VoiceKeyerSettings.h"
#include "models/VoiceKeyer.h"
#include "tts/TextToSpeechEngine.h"
#include "tts/TtsWav.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr int kUserVoice = -1;   // combo data for the not-yet-trained operator voice
}

TextToSpeechDialog::TextToSpeechDialog(TextToSpeechEngine* engine, PreviewStart previewStart,
                                       PreviewStop previewStop, QWidget* parent)
    : QDialog(parent)
    , m_engine(engine)
    , m_previewStart(std::move(previewStart))
    , m_previewStop(std::move(previewStop))
{
    setWindowTitle(QStringLiteral("Text to Speech"));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);

    m_title = new QLabel;
    m_title->setAccessibleName(QStringLiteral("Target slot"));
    layout->addWidget(m_title);

    m_text = new QPlainTextEdit;
    m_text->setPlaceholderText(QStringLiteral("CQ contest, CQ contest, this is K5PTB, K5PTB, contest"));
    m_text->setAccessibleName(QStringLiteral("Message text"));
    m_text->setAccessibleDescription(QStringLiteral("The message to turn into speech"));
    m_text->setFixedHeight(96);
    layout->addWidget(m_text);

    auto* controls = new QHBoxLayout;
    auto* voiceLabel = new QLabel(QStringLiteral("Voice:"));
    controls->addWidget(voiceLabel);
    m_voice = new QComboBox;
    m_voice->setAccessibleName(QStringLiteral("Voice"));
    voiceLabel->setBuddy(m_voice);
    // The operator's own voice comes from training in Settings (a later
    // step); until then it is listed but cannot be chosen.
    m_voice->addItem(QStringLiteral("User voice — not trained yet"), kUserVoice);
    m_voice->addItem(QStringLiteral("Female"), int(TextToSpeechEngine::Voice::Female));
    m_voice->addItem(QStringLiteral("Male"), int(TextToSpeechEngine::Voice::Male));
    if (auto* model = qobject_cast<QStandardItemModel*>(m_voice->model()))
        model->item(0)->setEnabled(false);
    // Male until the operator has trained their own voice.
    m_voice->setCurrentIndex(m_voice->findData(int(TextToSpeechEngine::Voice::Male)));
    controls->addWidget(m_voice, 1);

    m_generateBtn = new QPushButton(QStringLiteral("Generate"));
    m_generateBtn->setToolTip(QStringLiteral("Turn the message into speech (the first time also downloads the voice model)"));
    controls->addWidget(m_generateBtn);
    m_playBtn = new QPushButton(QString::fromUtf8("▶ PLAY"));
    m_playBtn->setToolTip(QStringLiteral("Hear the generated speech on this computer — not transmitted"));
    m_playBtn->setAccessibleName(QStringLiteral("Play generated speech"));
    controls->addWidget(m_playBtn);
    layout->addLayout(controls);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setAccessibleName(QStringLiteral("Status"));
    m_statusIsError = true;   // force the first style
    setStatus(QStringLiteral("Type a message, choose a voice and press Generate."));
    layout->addWidget(m_status);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_saveBtn = new QPushButton;
    m_saveBtn->setToolTip(QStringLiteral("Replace the slot's recording with the generated speech"));
    buttons->addWidget(m_saveBtn);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    connect(m_generateBtn, &QPushButton::clicked, this, &TextToSpeechDialog::generate);
    connect(m_playBtn, &QPushButton::clicked, this, &TextToSpeechDialog::togglePlay);
    connect(m_saveBtn, &QPushButton::clicked, this, &TextToSpeechDialog::save);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_text, &QPlainTextEdit::textChanged, this, &TextToSpeechDialog::invalidateSpeech);
    connect(m_voice, &QComboBox::currentIndexChanged, this, &TextToSpeechDialog::invalidateSpeech);

    if (m_engine) {
        connect(m_engine, &TextToSpeechEngine::statusChanged, this, [this](const QString& message) {
            if (m_generating)
                setStatus(message);
        });
        connect(m_engine, &TextToSpeechEngine::generated, this, &TextToSpeechDialog::onGenerated);
        connect(m_engine, &TextToSpeechEngine::failed, this, &TextToSpeechDialog::onFailed);
    }
    refreshButtons();
}

TextToSpeechDialog::~TextToSpeechDialog()
{
    if (m_playing && m_previewStop)
        m_previewStop();
}

void TextToSpeechDialog::setTarget(VoiceKeyer* keyer, int slot)
{
    if (m_transferConnection)
        disconnect(m_transferConnection);
    m_keyer = keyer;
    m_slot = slot;
    m_saving = false;
    if (keyer) {
        m_transferConnection = connect(keyer, &VoiceKeyer::transferFinished,
                                       this, &TextToSpeechDialog::onTransferFinished);
    }
    const QString where = keyer && keyer->sourceLabel() == QLatin1String("Radio")
        ? QStringLiteral("radio DVK") : QStringLiteral("local");
    m_title->setText(QStringLiteral("Speech for slot %1 (%2)").arg(slot).arg(where));
    m_saveBtn->setText(QStringLiteral("Save to Slot %1").arg(slot));
    // Start from the slot's message, ready to edit: the full text saved with
    // it when that is still what the slot holds (its label unchanged since),
    // otherwise the slot's own label. An unnamed slot keeps whatever is typed.
    if (keyer) {
        QString label;
        for (const VoiceKeyerRecording& rec : keyer->recordings()) {
            if (rec.id == slot) {
                label = rec.name.trimmed();
                break;
            }
        }
        const VoiceKeyerSettings::SpeechText stored =
            VoiceKeyerSettings::speechText(sourceOf(keyer), slot);
        if (!stored.text.isEmpty() && !label.isEmpty() && stored.label == label)
            m_text->setPlainText(stored.text);
        else if (!label.isEmpty() && label != QStringLiteral("Recording %1").arg(slot))
            m_text->setPlainText(label);
    }
    refreshButtons();
}

VoiceKeyerSource TextToSpeechDialog::sourceOf(const VoiceKeyer* keyer)
{
    return keyer && keyer->sourceLabel() == QLatin1String("Radio")
        ? VoiceKeyerSource::Radio : VoiceKeyerSource::Local;
}

QString TextToSpeechDialog::slotNameFor(const QString& text)
{
    constexpr int kMax = 30;
    QString flat = text.simplified();
    // The radio's set_name command quotes the name and cannot carry quotes;
    // Rename drops them the same way.
    flat.remove(QLatin1Char('"'));
    flat.remove(QLatin1Char('\''));
    if (flat.size() <= kMax)
        return flat;
    QString cut = flat.left(kMax);
    const int space = cut.lastIndexOf(QLatin1Char(' '));
    if (space > kMax / 2)
        cut.truncate(space);
    while (!cut.isEmpty() && (cut.back().isPunct() || cut.back().isSpace()))
        cut.chop(1);
    return cut + QString::fromUtf8("…");
}

void TextToSpeechDialog::generate()
{
    if (!m_engine || m_generating)
        return;
    const QVariant voice = m_voice->currentData();
    if (voice.toInt() == kUserVoice) {
        setStatus(QStringLiteral("Choose Female or Male — the user voice is not trained yet."), true);
        return;
    }
    if (m_playing)
        togglePlay();
    m_generating = true;
    refreshButtons();
    setStatus(QStringLiteral("Preparing…"));
    m_engine->generate(m_text->toPlainText(), TextToSpeechEngine::Voice(voice.toInt()));
}

void TextToSpeechDialog::onGenerated(const QVector<float>& speech)
{
    if (!m_generating)
        return;
    m_generating = false;
    m_speech = speech;
    m_speechText = m_text->toPlainText();
    m_speechVoice = m_voice->currentIndex();
    const double seconds = TtsWav::durationMs(speech) / 1000.0;
    setStatus(QStringLiteral("Generated %1 s of speech. Press PLAY to review it, then Save to Slot %2.")
                  .arg(seconds, 0, 'f', 1).arg(m_slot));
    refreshButtons();
}

void TextToSpeechDialog::onFailed(const QString& message)
{
    if (!m_generating)
        return;
    m_generating = false;
    setStatus(message, true);
    refreshButtons();
}

void TextToSpeechDialog::invalidateSpeech()
{
    if (m_speech.isEmpty())
        return;
    if (m_text->toPlainText() == m_speechText && m_voice->currentIndex() == m_speechVoice)
        return;
    if (m_playing)
        togglePlay();
    m_speech.clear();
    setStatus(QStringLiteral("The message or voice changed — press Generate again."));
    refreshButtons();
}

void TextToSpeechDialog::togglePlay()
{
    if (m_playing) {
        if (m_previewStop)
            m_previewStop();
        onPreviewFinished();
        return;
    }
    if (m_speech.isEmpty() || !m_previewStart)
        return;
    QString error;
    const QString path = m_tempDir.filePath(QStringLiteral("preview.wav"));
    if (!TtsWav::write(path, m_speech, &error) || !m_previewStart(path, error)) {
        setStatus(error, true);
        return;
    }
    m_playing = true;
    refreshButtons();
}

void TextToSpeechDialog::onPreviewFinished()
{
    if (!m_playing)
        return;
    m_playing = false;
    refreshButtons();
}

void TextToSpeechDialog::save()
{
    if (m_speech.isEmpty() || !m_keyer || m_saving)
        return;
    const int ms = TtsWav::durationMs(m_speech);
    if (m_keyer->sourceLabel() == QLatin1String("Radio") && ms > kRadioMaxMs) {
        setStatus(QStringLiteral("This message is %1 s; a radio DVK slot holds about %2 s. "
                                 "Shorten it, or switch the keyer to Local.")
                      .arg(ms / 1000.0, 0, 'f', 1).arg(kRadioMaxMs / 1000.0, 0, 'f', 1), true);
        return;
    }
    QString error;
    const QString path = m_tempDir.filePath(QStringLiteral("slot-%1.wav").arg(m_slot));
    if (!TtsWav::write(path, m_speech, &error)) {
        setStatus(error, true);
        return;
    }
    m_saving = true;
    refreshButtons();
    setStatus(QStringLiteral("Saving to slot %1…").arg(m_slot));
    m_keyer->importWav(m_slot, path);   // Local answers at once; Radio after the upload
}

void TextToSpeechDialog::onTransferFinished(bool ok, const QString& message)
{
    if (!m_saving)
        return;
    m_saving = false;
    if (ok && m_keyer) {
        // A short label for the panel; the whole message is kept alongside it
        // so the next Text to Speech on this slot starts from all of it.
        const QString name = slotNameFor(m_speechText);
        m_keyer->setName(m_slot, name);
        VoiceKeyerSettings::setSpeechText(sourceOf(m_keyer), m_slot, m_speechText.trimmed(), name);
        setStatus(QStringLiteral("Saved to slot %1 as “%2”.").arg(m_slot).arg(name));
    } else {
        setStatus(QStringLiteral("Could not save to slot %1: %2").arg(m_slot).arg(message), true);
    }
    refreshButtons();
}

void TextToSpeechDialog::setStatus(const QString& text, bool error)
{
    m_status->setText(text);
    if (error == m_statusIsError)
        return;
    m_statusIsError = error;
    ThemeManager::instance().applyStyleSheet(m_status, error
        ? "QLabel { color: {{color.accent.danger}}; font-weight: bold; }"
        : "QLabel { color: {{color.text.primary}}; }");
}

void TextToSpeechDialog::refreshButtons()
{
    const bool haveSpeech = !m_speech.isEmpty();
    m_generateBtn->setEnabled(!m_generating && !m_saving && m_engine);
    m_text->setReadOnly(m_generating);
    m_voice->setEnabled(!m_generating);
    m_playBtn->setEnabled(haveSpeech && !m_generating);
    m_playBtn->setText(m_playing ? QString::fromUtf8("■ STOP") : QString::fromUtf8("▶ PLAY"));
    m_saveBtn->setEnabled(haveSpeech && !m_generating && !m_saving && m_keyer);
}

void TextToSpeechDialog::closeEvent(QCloseEvent* event)
{
    if (m_playing)
        togglePlay();
    if (m_generating && m_engine) {
        m_generating = false;
        m_engine->cancel();
    }
    QDialog::closeEvent(event);
}

} // namespace AetherSDR
