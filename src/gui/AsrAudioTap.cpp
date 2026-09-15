#include "AsrAudioTap.h"

#include "asr/AsrEngine.h"
#include "core/AudioEngine.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QVector>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrTap, "aether.asr.tap")

AsrAudioTap::AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent)
    : QObject(parent)
    , m_audio(audio)
    , m_asr(asr)
{
}

void AsrAudioTap::setEnabled(bool on)
{
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    if (m_asr != nullptr) {
        m_asr->setEnabled(on);
    }

    if (on) {
        // A fresh session picks its receiver again — the one that was live last
        // time may be gone, and the operator would otherwise have to wait out
        // the release window before anything was transcribed.
        m_policy.reset();
        m_clock.start();
        m_warnedUndecodable = false;
        connectSelectedTap();
    } else {
        disconnect(m_conn);
        m_policy.reset();
        // m_asr->setEnabled(false) above already resets and drops any queued
        // backlog — no separate reset() needed here.
    }
}

void AsrAudioTap::setTapPoint(AsrTapPoint point)
{
    if (point == m_tapPoint) {
        return;
    }
    m_tapPoint = point;
    if (!m_enabled) {
        return; // the next setEnabled(true) connects the new point
    }

    disconnect(m_conn);
    // Start over rather than carry on. An utterance begun through one chain
    // and finished through the other is spliced across a level step and a
    // latency step (an NR stage delays its output), and the receiver lock was
    // earned on the other signal. The engine reset drops the partial
    // utterance, carried context and speaker clusters — a speaker's embedding
    // shifts with NR anyway, so clusters would not survive the switch intact.
    m_policy.reset();
    m_clock.restart();
    m_warnedUndecodable = false;
    if (m_asr != nullptr) {
        m_asr->reset();
    }
    connectSelectedTap();
}

void AsrAudioTap::connectSelectedTap()
{
    if (m_audio == nullptr) {
        return;
    }
    // Queued so the audio-thread emit lands on this (main) thread; the heavy
    // resample+inference then happens on the ASR worker thread.
    //
    // Both signals are unthrottled by design — see the note in the header.
    // Every block they carry must reach the engine.
    if (m_tapPoint == AsrTapPoint::PreDsp) {
        m_conn = connect(m_audio, &AudioEngine::receivePresentationPreDspAudioReady,
                         this, &AsrAudioTap::onPreDspAudio, Qt::QueuedConnection);
    } else {
        m_conn = connect(m_audio, &AudioEngine::receivePresentationPostDspAudioReady,
                         this, &AsrAudioTap::onPostDspAudio, Qt::QueuedConnection);
    }
}

void AsrAudioTap::onPostDspAudio(const QString& source, const QString& sourceId,
                                 const QByteArray& pcmFloat, int sampleRate,
                                 int channels)
{
    onRxAudio(AsrTapPoint::PostDsp, source, sourceId, pcmFloat, sampleRate, channels);
}

void AsrAudioTap::onPreDspAudio(const QString& source, const QString& sourceId,
                                const QByteArray& pcmFloat, int sampleRate,
                                int channels)
{
    onRxAudio(AsrTapPoint::PreDsp, source, sourceId, pcmFloat, sampleRate, channels);
}

void AsrAudioTap::onRxAudio(AsrTapPoint from,
                            const QString& source,
                            const QString& sourceId,
                            const QByteArray& pcmFloat,
                            int sampleRate,
                            int channels)
{
    // disconnect() does not withdraw calls a queued connection has already
    // posted, so blocks from the old point can still arrive after a switch —
    // after the engine reset above, where they would open the new session
    // with audio from the wrong chain. They name their point; drop them.
    if (!m_enabled || m_asr == nullptr || from != m_tapPoint) {
        return;
    }
    if (!m_policy.accepts(source, sourceId,
                          m_clock.isValid() ? m_clock.elapsed() : 0)) {
        return;
    }
    // channels comes straight off the signal (#4489) instead of being
    // assumed here — a future mono RX source is then a one-line change at
    // AudioEngine's emit site, and toMono() rejects a caller that gets it
    // wrong (and says so below) instead of silently mis-decoding.
    const QVector<float> mono = AsrTapPolicy::toMono(pcmFloat, channels);
    if (mono.isEmpty()) {
        // Rejecting a block toMono() cannot decode is right, but it is also
        // invisible: Copy Assist simply produces nothing, which looks exactly
        // like a model that failed to load or a tap that never connected. The
        // guard exists to catch a FUTURE emit site stating the wrong channel
        // count, so the one person who needs this message is the one who has
        // no reason to suspect this code at all — say it out loud.
        //
        // Once per enable, not per block: this runs on every audio block
        // (~5 ms apart on a Flex LAN stream), and a mis-stated channel count
        // is permanent for a given emit site, so the first block says
        // everything the log needs. setEnabled() clears the latch so a later
        // session reports again.
        if (!m_warnedUndecodable) {
            m_warnedUndecodable = true;
            qCWarning(lcAsrTap) << "dropping undecodable RX audio from" << source
                                << "id" << sourceId << "- bytes" << pcmFloat.size()
                                << "channels" << channels
                                << "(expected 1 or 2, a whole number of frames)";
        }
        return;
    }
    m_asr->pushAudio(mono, sampleRate);
}

} // namespace AetherSDR
