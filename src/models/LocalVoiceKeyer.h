#pragma once

#include "VoiceKeyer.h"
#include "core/GeneratedAudioTransmitter.h"
#include "core/LocalVoiceKeyerStore.h"

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

namespace AetherSDR {

// The client-side voice keyer (RFC #4214): the DVK panel's slots, with the
// recordings kept on this computer instead of on the radio — no SmartSDR+
// licence, portable between radios, never touching a radio's stored DVK.
//
// Recording taps the audio AetherSDR already captures from the PC microphone
// (AudioEngine::txFinalMonitorPcmReady, after the TX voice chain), so it never
// keys the transmitter (Principle VI). That tap only carries the PC mic; with
// a radio-side mic source selected, REC refuses and says why rather than
// recording silence.
//
// On-air playback hands the slot's audio to the shared
// GeneratedAudioTransmitter, which owns keying, pacing and the unkey; every
// transmission starts from an operator PLAY or F-key and ends on its own.
class LocalVoiceKeyer : public VoiceKeyer {
    Q_OBJECT
public:
    static constexpr int kSlotCount = LocalVoiceKeyerStore::kSlotCount;

    explicit LocalVoiceKeyer(const QString& recordingsDir, QObject* parent = nullptr);

    Status status() const override { return m_status; }
    int activeId() const override { return m_activeId; }
    const QVector<VoiceKeyerRecording>& recordings() const override { return m_recordings; }

    void recStart(int id) override;
    void recStop(int id) override;
    void previewStart(int id) override;
    void previewStop(int id) override;
    void playbackStart(int id) override;
    void playbackStop(int id) override;
    void clear(int id) override;
    void remove(int id) override;
    void setName(int id, const QString& name) override;

    void importWav(int id, const QString& path) override;
    void exportWav(int id, const QString& path) override;
    bool canTransferWav() const override { return true; }
    bool isTransferring() const override { return false; }

    QString sourceLabel() const override { return QStringLiteral("Local"); }

    QString recordingsDir() const { return m_store.dir(); }

    // ── Wiring (MainWindow) ──────────────────────────────────────────────
    // The radio's current mic source ("PC", "MIC", …). Recording needs "PC".
    using MicSourceProbe = std::function<QString()>;
    void setMicSourceProbe(MicSourceProbe probe) { m_micSource = std::move(probe); }

    // Preview plays a slot's file on this computer's speakers. start returns
    // false with a reason; the player reports the end through onPreviewFinished.
    using PreviewStart = std::function<bool(const QString& path, QString& error)>;
    using PreviewStop  = std::function<void()>;
    void setPreviewHandlers(PreviewStart start, PreviewStop stop);

    // On-air playback. Without one, PLAY refuses and says so.
    void setTransmitter(GeneratedAudioTransmitter* transmitter);

public slots:
    // AudioEngine::txFinalMonitorPcmReady — 24 kHz stereo int16. Only the
    // operator's own mic is recorded: client-leveled audio is a TCI/DAX
    // application's, not the operator's voice.
    void onMicPcm(const QByteArray& int16Stereo, bool clientLeveled);
    void onPreviewFinished();

    // Re-read the folder and labels, e.g. after WAVs were dropped in.
    void reload();

private:
    void refreshSlot(int id);
    void setStatus(Status status, int id);
    void refuse(const QString& verb, int id, const QString& message);
    bool busyRefusal(const QString& verb, int id);
    void onTransmitFinished(GeneratedAudioTransmitter::Outcome outcome, const QString& reason);

    LocalVoiceKeyerStore m_store;
    QVector<VoiceKeyerRecording> m_recordings;
    Status m_status{Idle};
    int m_activeId{-1};

    MicSourceProbe m_micSource;
    PreviewStart m_previewStart;
    PreviewStop m_previewStop;
    QPointer<GeneratedAudioTransmitter> m_transmitter;
    QMetaObject::Connection m_transmitterFinished;

    QByteArray m_recordBuffer;
    bool m_recordCapped{false};
};

} // namespace AetherSDR
