#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// Which voice keyer the DVK panel drives (RFC #4214).
//
//  * Radio — the radio-hosted DVK: recordings on the radio, SmartSDR+ required.
//  * Local — the client-side keyer: recordings on this computer, no licence,
//            portable across radios.
enum class VoiceKeyerSource { Radio, Local };

// What the operator chose. Auto means "never chosen": the source then follows
// the radio's DVK entitlement. An explicit choice persists and wins — an
// operator with several radios may force Local so their recordings travel
// with them, and a licensed operator may still prefer Local.
enum class VoiceKeyerSourceSetting { Auto, Radio, Local };

inline VoiceKeyerSourceSetting parseVoiceKeyerSourceSetting(const QString& text)
{
    if (text == QLatin1String("radio")) return VoiceKeyerSourceSetting::Radio;
    if (text == QLatin1String("local")) return VoiceKeyerSourceSetting::Local;
    return VoiceKeyerSourceSetting::Auto;
}

inline QString voiceKeyerSourceSettingName(VoiceKeyerSourceSetting setting)
{
    switch (setting) {
    case VoiceKeyerSourceSetting::Radio: return QStringLiteral("radio");
    case VoiceKeyerSourceSetting::Local: return QStringLiteral("local");
    case VoiceKeyerSourceSetting::Auto:  break;
    }
    return QStringLiteral("auto");
}

inline VoiceKeyerSource resolveVoiceKeyerSource(VoiceKeyerSourceSetting setting,
                                                bool licenseSeen,
                                                bool licenseEnabled)
{
    if (setting == VoiceKeyerSourceSetting::Radio) return VoiceKeyerSource::Radio;
    if (setting == VoiceKeyerSourceSetting::Local) return VoiceKeyerSource::Local;
    // Auto follows the licence. Until the radio has reported the entitlement,
    // stay on the radio DVK — the same fail-open rule the DVK indicator uses
    // (DvkAvailabilityGate.h): the radio must SAY no before the UI decides no.
    return (licenseSeen && !licenseEnabled) ? VoiceKeyerSource::Local
                                            : VoiceKeyerSource::Radio;
}

} // namespace AetherSDR
