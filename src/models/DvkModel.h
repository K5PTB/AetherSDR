#pragma once

#include "VoiceKeyer.h"

#include <QMap>
#include <QString>
#include <QVector>

#include <functional>

namespace AetherSDR {

// The recording record is shared with the client-side keyer; the historical
// name stays for existing callers.
using DvkRecording = VoiceKeyerRecording;

// The radio-hosted voice keyer: SmartSDR `dvk` verbs, recordings stored on the
// radio (SmartSDR+ required). The panel drives it through VoiceKeyer.
class DvkModel : public VoiceKeyer {
    Q_OBJECT
public:
    explicit DvkModel(QObject* parent = nullptr);

    // State
    Status status() const override { return m_status; }
    int activeId() const override { return m_activeId; }
    bool enabled() const { return m_enabled; }
    const QVector<VoiceKeyerRecording>& recordings() const override { return m_recordings; }

    // Commands
    void recStart(int id) override;
    void recStop(int id) override;
    void previewStart(int id) override;
    void previewStop(int id) override;
    void playbackStart(int id) override;
    void playbackStop(int id) override;
    void clear(int id) override;
    void remove(int id) override;
    void setName(int id, const QString& name) override;

    // WAV import/export. The transfer itself (DvkWavTransfer) is Flex wire
    // code this model must not reach, so the model asks for it by signal and
    // MainWindow connects the transfer — see wavUploadRequested. The busy
    // probe reports whether that transfer is mid-flight; with none installed,
    // import/export is unavailable.
    void importWav(int id, const QString& path) override;
    void exportWav(int id, const QString& path) override;
    bool canTransferWav() const override { return static_cast<bool>(m_transferBusyProbe); }
    bool isTransferring() const override { return m_transferBusyProbe && m_transferBusyProbe(); }
    void setWavTransferBusyProbe(std::function<bool()> probe) { m_transferBusyProbe = std::move(probe); }

    // Status parsing (called from RadioModel)
    void applyStatus(const QString& object, const QMap<QString, QString>& kvs);

    // Called by RadioModel when a reply to a DVK command arrives.  Non-zero
    // codes are forwarded as commandFailed() so the UI can surface them
    // instead of leaving the operation silently rejected. (#3377)
    void handleCommandResponse(const QString& verb, int id, uint code, const QString& body);

    // Map a SmartSDR response code to a human-readable hint.  Known codes
    // come from FlexLib's SsdrErrors enum (Principle I); unknown codes
    // render as bare hex.
    static QString dvkErrorString(uint code);

signals:
    // Emitted for commands that need response correlation.  RadioModel
    // attaches a callback that invokes handleCommandResponse() with the
    // verb + slot id captured here. (#3377)
    void replyCommandReady(const QString& cmd, const QString& verb, int id);
    // The operator asked to import or export a slot's WAV; MainWindow routes
    // these to DvkWavTransfer.
    void wavUploadRequested(int id, const QString& path);
    void wavDownloadRequested(int id, const QString& path);

private:
    Status m_status{Unknown};
    int m_activeId{-1};
    bool m_enabled{false};
    QVector<VoiceKeyerRecording> m_recordings;
    std::function<bool()> m_transferBusyProbe;

    VoiceKeyerRecording* findRecording(int id);
};

} // namespace AetherSDR
