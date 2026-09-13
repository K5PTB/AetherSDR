#pragma once

// Client-side voice keyer persistence (RFC #4214). Per Constitution Principle V
// the configuration lives as ONE nested JSON blob under the single AppSettings
// key "VoiceKeyer" — never flat keys. These labels are this client's own; they
// never issue a `dvk` command, so renaming a local slot cannot touch a radio's
// stored DVK names.

#include "core/VoiceKeyerSource.h"

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class VoiceKeyerSettings {
public:
    // Radio vs Local. Auto until the operator chooses.
    static VoiceKeyerSourceSetting source();
    static void setSource(VoiceKeyerSourceSetting setting);

    // Local slot labels, 1-based slot ids. Empty when the slot has no label.
    static QString slotName(int id);
    static void setSlotName(int id, const QString& name);

    // The full text-to-speech message behind a slot (RFC #4334), kept apart
    // from its short label so the whole message can be edited and generated
    // again. `label` is the slot label it was saved under: once the slot is
    // renamed, re-recorded or re-imported its label no longer matches, and the
    // stored text is stale. Kept per source, since a radio's slot 3 and the
    // local slot 3 are different recordings.
    struct SpeechText {
        QString text;
        QString label;
    };
    static SpeechText speechText(VoiceKeyerSource source, int id);
    static void setSpeechText(VoiceKeyerSource source, int id,
                              const QString& text, const QString& label);

    // Where local recordings live: ~/Documents/AetherSDR/VoiceKeyer, next to
    // QsoRecorder's ~/Documents/AetherSDR/Recordings, so operators can find,
    // copy and drop in WAVs.
    static QString recordingsDir();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

} // namespace AetherSDR
