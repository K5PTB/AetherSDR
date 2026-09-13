#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

struct VoiceKeyerRecording {
    int id{0};
    QString name;
    int durationMs{0};  // milliseconds; 0 = empty slot
};

// A voice keyer as the DVK panel drives it: numbered slots holding short
// recordings, record / preview / on-air playback intent, and a status the
// panel mirrors. Slot ids are 1-based, matching the radio DVK and F1-F12.
//
// This is the core half of the split the touchpoint audit already names for
// DvkModel ("Core: voice-keyer slots/status + rec/preview/playback intent").
// DvkModel is the radio-hosted implementation — SmartSDR `dvk` verbs,
// recordings stored on the radio, SmartSDR+ required. RFC #4214's client-side
// keyer is the second — recordings stored on this computer, no licence. The
// panel sees only this interface, so one surface serves both.
class VoiceKeyer : public QObject {
    Q_OBJECT
public:
    enum Status { Unknown, Disabled, Idle, Recording, Preview, Playback };
    Q_ENUM(Status)

    explicit VoiceKeyer(QObject* parent = nullptr);
    ~VoiceKeyer() override;

    virtual Status status() const = 0;
    virtual int activeId() const = 0;  // -1 when nothing is active
    virtual const QVector<VoiceKeyerRecording>& recordings() const = 0;

    virtual void recStart(int id) = 0;
    virtual void recStop(int id) = 0;
    virtual void previewStart(int id) = 0;
    virtual void previewStop(int id) = 0;
    virtual void playbackStart(int id) = 0;  // on-air
    virtual void playbackStop(int id) = 0;
    virtual void clear(int id) = 0;
    virtual void remove(int id) = 0;
    virtual void setName(int id, const QString& name) = 0;

    // Copy a WAV into or out of a slot. Progress and the outcome arrive as
    // transferStatusChanged / transferFinished.
    virtual void importWav(int id, const QString& path) = 0;
    virtual void exportWav(int id, const QString& path) = 0;
    virtual bool canTransferWav() const = 0;  // false: import/export unavailable
    virtual bool isTransferring() const = 0;

signals:
    void statusChanged(AetherSDR::VoiceKeyer::Status status, int id);
    void recordingChanged(int id);
    void recordingsLoaded();
    // A command was refused. The panel shows the message and re-syncs its
    // buttons so a refused press doesn't leave one latched. (#3377)
    void commandFailed(const QString& verb, int id, uint code, const QString& message);
    void transferStatusChanged(const QString& message);
    void transferFinished(bool success, const QString& message);
};

} // namespace AetherSDR
