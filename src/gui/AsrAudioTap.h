#pragma once

#include "gui/AsrTapPolicy.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>

class QByteArray;

namespace AetherSDR {

class AudioEngine;
class AsrEngine;

// App-layer bridge from the engine's post-NR RX audio to the ASR engine
// (RFC #4333, Phase 4). This is the "audio tap": it composes aethercore's
// AudioEngine with aetherasr's AsrEngine without either knowing about the other,
// keeping both libraries decoupled (the whole point of the aetherasr split).
//
// When enabled it forwards post-client-DSP mono RX audio to AsrEngine, which
// resamples to 16 kHz on its own worker thread — so nothing here runs on the
// audio callback. When disabled it disconnects entirely, so an idle ASR feature
// costs nothing.
//
// ── Which engine signal, and why it matters (#4486) ───────────────────────
//
// This subscribes to receivePresentationPostDspAudioReady, NOT to
// rxPostChainScopeReady. The latter is a VISUALISATION signal: it drops any
// block arriving within 8 ms of the previous one. That is correct for a scope
// and ruinous for a recogniser, and it was not hypothetical — with NR2 enabled
// the engine queues whole radio packets and drains them in a tight loop, so
// blocks arrive microseconds apart at 5.33 ms each (128 frames at 24 kHz on a
// Flex LAN stream). Only the first survived each drain tick, which discarded
// roughly HALF the speech samples and spliced what remained into a stream with
// a discontinuity at every phoneme. ASR "barely worked" with NR2 on while the
// speaker sounded better than ever, because the speaker path was never lossy.
//
// The presentation signal is unthrottled, carries the same post-DSP audio from
// the same place in writeAudio(), and is additionally tagged with its source —
// which is what lets the tap follow ONE receiver instead of an interleaved mix
// of every Kiwi on the system. See AsrTapPolicy.
//
// Do NOT "fix" this by relaxing the 8 ms throttle: StripWaveformPanel and
// MainWindow are its intended consumers and frame-dropping is right for them.
//
// ── Which point in the chain (AsrTapPoint) ────────────────────────────────
//
// By default the tap follows the post-DSP signal above. The operator can move
// it to receivePresentationPreDspAudioReady, the same unthrottled, source-
// tagged stream taken before client NR and the RX effects — NR artifacts can
// confuse the model more than the noise NR removes. Both signals are connected
// to separate slots that name their own point, so a block queued from the old
// point before a switch is recognised and dropped instead of being spliced
// into the new stream.
//
// In the aetherd future this glue moves to the engine/daemon side; the thin UI
// then subscribes to AsrEngine::finalText streamed over the wire and never sees
// audio or whisper.
class AsrAudioTap : public QObject {
    Q_OBJECT
public:
    AsrAudioTap(AudioEngine* audio, AsrEngine* asr, QObject* parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    // Applied live. Switching while enabled starts transcription over (see the
    // .cpp); switching while disabled only takes effect at the next enable.
    void setTapPoint(AsrTapPoint point);
    AsrTapPoint tapPoint() const { return m_tapPoint; }

private:
    void connectSelectedTap();
    void onPostDspAudio(const QString& source, const QString& sourceId,
                        const QByteArray& pcmFloat, int sampleRate, int channels);
    void onPreDspAudio(const QString& source, const QString& sourceId,
                       const QByteArray& pcmFloat, int sampleRate, int channels);
    void onRxAudio(AsrTapPoint from,
                   const QString& source,
                   const QString& sourceId,
                   const QByteArray& pcmFloat,
                   int sampleRate,
                   int channels);

    AudioEngine* m_audio = nullptr;
    AsrEngine* m_asr = nullptr;
    QMetaObject::Connection m_conn;
    bool m_enabled = false;
    AsrTapPoint m_tapPoint = AsrTapPoint::PostDsp;
    AsrTapPolicy m_policy;
    QElapsedTimer m_clock;   // monotonic source for the policy's release window
    // Latch so a block toMono() cannot decode warns once per enable rather
    // than once per audio block. Cleared in setEnabled(true).
    bool m_warnedUndecodable = false;
};

} // namespace AetherSDR
