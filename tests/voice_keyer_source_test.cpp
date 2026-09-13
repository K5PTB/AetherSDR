// Radio vs Local voice keyer selection (RFC #4214) — pure policy, no Qt event loop.
// Run: ./build/voice_keyer_source_test

#include "core/VoiceKeyerSource.h"

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

const char* name(VoiceKeyerSource s)
{
    return s == VoiceKeyerSource::Radio ? "Radio" : "Local";
}

} // namespace

int main()
{
    using S = VoiceKeyerSourceSetting;
    using R = VoiceKeyerSource;

    // Auto follows the licence once the radio has reported it.
    report("auto_licensed_uses_radio",
           resolveVoiceKeyerSource(S::Auto, true, true) == R::Radio);
    report("auto_unlicensed_uses_local",
           resolveVoiceKeyerSource(S::Auto, true, false) == R::Local);

    // Before the radio reports the entitlement, stay on the radio DVK (fail
    // open, like the indicator) — an unknown licence is not a missing one.
    report("auto_unknown_licence_stays_on_radio",
           resolveVoiceKeyerSource(S::Auto, false, false) == R::Radio,
           name(resolveVoiceKeyerSource(S::Auto, false, false)));

    // An explicit choice wins over the licence either way.
    report("forced_local_on_licensed_radio",
           resolveVoiceKeyerSource(S::Local, true, true) == R::Local);
    report("forced_radio_on_unlicensed_radio",
           resolveVoiceKeyerSource(S::Radio, true, false) == R::Radio);

    // Persisted text round-trips; anything unrecognised is Auto.
    report("setting_names_round_trip",
           parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Radio)) == S::Radio
               && parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Local)) == S::Local
               && parseVoiceKeyerSourceSetting(voiceKeyerSourceSettingName(S::Auto)) == S::Auto);
    report("unrecognised_text_is_auto",
           parseVoiceKeyerSourceSetting(QStringLiteral("Local")) == S::Auto
               && parseVoiceKeyerSourceSetting(QString()) == S::Auto);

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
