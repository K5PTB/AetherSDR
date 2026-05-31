#include "CwPllDecoder.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace AetherSDR {

// ── Morse code table ─────────────────────────────────────────────────────────

static const struct { const char* bits; char ch; } kMorse[] = {
    {".-",    'A'}, {"-...",  'B'}, {"-.-.",  'C'}, {"-..",   'D'},
    {".",     'E'}, {"..-.",  'F'}, {"--.",   'G'}, {"....",  'H'},
    {"..",    'I'}, {".---",  'J'}, {"-.-",   'K'}, {".-..",  'L'},
    {"--",    'M'}, {"-.",    'N'}, {"---",   'O'}, {".--.",  'P'},
    {"--.-",  'Q'}, {".-.",   'R'}, {"...",   'S'}, {"-",     'T'},
    {"..-",   'U'}, {"...-",  'V'}, {".--",   'W'}, {"-..-",  'X'},
    {"-.--",  'Y'}, {"--..",  'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
    {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
    {"---..", '8'}, {"----.", '9'},
    {".-.-.-",'.'}, {"--..--",','}, {"..--..",'?'}, {"-..-..", '/'},
    {"-....-",'-'}, {".-.-..", '+'}, {"-...-", '='}, {"-.--.", '('},
    {"...-.-", '\0'}, // SK prosign — end of contact, emit nothing
};

char CwPllDecoder::morseToAscii(const std::string& bits)
{
    for (auto& e : kMorse)
        if (bits == e.bits) return e.ch;
    return '\0';  // unknown sequence
}

// ── Constructor / config ─────────────────────────────────────────────────────

CwPllDecoder::CwPllDecoder()
    : CwPllDecoder(Config{})
{}

CwPllDecoder::CwPllDecoder(const Config& cfg)
    : m_cfg(cfg)
{
    reset();
}

void CwPllDecoder::setConfig(const Config& cfg)
{
    m_cfg = cfg;
    // Re-clamp dot estimate to new speed range
    float minDot = 1.2f / m_cfg.speedMax;
    float maxDot = 1.2f / m_cfg.speedMin;
    m_dotSec = std::clamp(m_dotSec, minDot, maxDot);
    // If pitch changed, use it directly
    if (m_cfg.pitchHz > 0) {
        m_pitch = m_cfg.pitchHz;
        m_pitchLocked = true;
    }
}

void CwPllDecoder::lockPitch(float hz)
{
    if (hz > 0) {
        m_pitch = hz;
        m_pitchLocked = true;
    } else {
        m_pitchLocked = false;
        m_reSweepCountdown = 0;  // trigger immediate re-sweep
    }
}

void CwPllDecoder::lockSpeed(float wpm)
{
    if (wpm > 0) {
        m_dotSec = 1.2f / wpm;
        m_speedLocked = true;
    } else {
        m_speedLocked = false;
    }
}

void CwPllDecoder::reset()
{
    m_envSmooth = 0;
    m_envMax    = 1e-9f;
    m_toneOn    = false;
    m_stateSec  = 0;
    m_onCount   = 0;
    m_symbolBits.clear();
    m_output.clear();
    m_reSweepCountdown = 0;
    m_confidence   = 0;
    m_errIdx       = 0;
    m_lastErr      = 0;
    m_silenceSec   = 0;
    m_didConfReset = false;
    m_signalPresent     = false;
    m_prevSignalPresent = false;
    m_retroBuf.clear();
    std::fill(std::begin(m_errors), std::end(m_errors), 0);
    m_symbolCount = 0;
    // Reset dot estimate to midpoint of speed range
    float mid = (m_cfg.speedMin + m_cfg.speedMax) / 2.0f;
    if (!m_speedLocked)
        m_dotSec = 1.2f / mid;
    if (!m_pitchLocked) {
        m_pitch = (m_cfg.pitchMin + m_cfg.pitchMax) / 2.0f;
        if (m_cfg.pitchHz > 0) m_pitch = m_cfg.pitchHz;
    }
}

float CwPllDecoder::estimatedSpeed() const
{
    return m_dotSec > 0 ? 1.2f / m_dotSec : 0.0f;
}

// ── Goertzel ─────────────────────────────────────────────────────────────────
//
// Returns unnormalized power at freq over n samples.
// Simple DFT magnitude^2 at one bin — O(n), no FFT needed.

float CwPllDecoder::goertzelPower(const float* samples, int n, float freq) const
{
    const float w     = 2.0f * static_cast<float>(M_PI) * freq / m_cfg.sampleRate;
    const float coeff = 2.0f * std::cos(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        float s0 = samples[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - s1 * s2 * coeff;
}

// ── Pitch sweep ───────────────────────────────────────────────────────────────

float CwPllDecoder::sweepPitch(const float* buf, int n) const
{
    // Coarse sweep across full range
    float bestP = -1.0f, bestF = m_pitch;
    for (float f = m_cfg.pitchMin; f <= m_cfg.pitchMax; f += kEnvSweepStep) {
        float p = goertzelPower(buf, n, f);
        if (p > bestP) { bestP = p; bestF = f; }
    }
    // Fine sweep ±25 Hz around peak
    for (float f = bestF - 25.0f; f <= bestF + 25.0f; f += kEnvSweepRefine) {
        if (f < m_cfg.pitchMin || f > m_cfg.pitchMax) continue;
        float p = goertzelPower(buf, n, f);
        if (p > bestP) { bestP = p; bestF = f; }
    }
    return bestF;
}

// ── Envelope + edge detection ─────────────────────────────────────────────────

void CwPllDecoder::updateEnvelope(float power)
{
    // Running max for AGC — very slow decay (τ ≈ 6.6 s) so the peak tracks
    // the signal level and noise never normalises to 1.0 during inter-word pauses.
    m_envMax = std::max(m_envMax * 0.9998f, power);
    float norm = power / m_envMax;

    // Fast attack (τ ≈ 2 ms), fast release (τ ≈ 5 ms).
    // Release MUST drop below kOffThresh within one inter-element gap.
    // At 50 WPM that gap is 24 ms; with α=0.25 we reach 0.12 in ~10 ms.
    const float kAttack  = 0.60f;
    const float kRelease = 0.25f;
    float alpha = (norm > m_envSmooth) ? kAttack : kRelease;
    m_envSmooth = m_envSmooth + alpha * (norm - m_envSmooth);

    // Rising edge: require kOnDebounce consecutive blocks above threshold.
    // A single-block noise spike cannot trigger tone-on; a real CW element
    // (shortest dot ~20 ms = 15 blocks) will pass easily.
    // Falling edge: single block below threshold is enough — fast release is
    // critical for detecting inter-element gaps at high speeds.
    const float kOnThresh  = 0.35f;  // raised from 0.25 for noise immunity
    const float kOffThresh = 0.15f;
    bool newToneOn = m_toneOn;
    if (!m_toneOn) {
        if (m_envSmooth > kOnThresh) {
            if (++m_onCount >= kOnDebounce) newToneOn = true;
        } else {
            m_onCount = 0;
        }
    } else {
        m_onCount = 0;
        if (m_envSmooth < kOffThresh) newToneOn = false;
    }

    if (newToneOn != m_toneOn) {
        onEdge(newToneOn);
        m_toneOn   = newToneOn;
        m_stateSec = 0.0f;
    }
}

// ── Edge → symbol pipeline ────────────────────────────────────────────────────

void CwPllDecoder::onEdge(bool rising)
{
    const float dur = m_stateSec;

    if (!rising) {
        // Noise gate: discard tone bursts shorter than 30 % of the minimum
        // possible dot. Real dots at speedMax are ~20–30 ms; noise spikes are
        // typically 1–4 Goertzel blocks (1–5 ms).
        const float kMinDot = 1.2f / m_cfg.speedMax;
        if (dur < 0.30f * kMinDot) return;

        // Tone just ended — classify as dot or dash
        bool isDot = (dur < 2.0f * m_dotSec);
        m_symbolBits += isDot ? '.' : '-';
        float expected = isDot ? m_dotSec : 3.0f * m_dotSec;
        recordTiming(dur, expected);
        // Normalize dash duration to 1T before updating the dot estimate.
        // Passing raw dash duration (3T) would corrupt the timing PLL.
        updateDot(isDot ? dur : dur / 3.0f, /*reference1T=*/true);
    } else {
        // Silence just ended — classify spacing
        if (dur < 2.0f * m_dotSec) {
            // Inter-element: stay in current character, use as 1T reference
            updateDot(dur, /*reference1T=*/true);
        } else if (dur < 5.0f * m_dotSec) {
            // Inter-character: decode accumulated symbol
            recordTiming(dur, 3.0f * m_dotSec);
            pushSymbol();
            updateDot(dur / 3.0f, /*reference1T=*/true);
        } else {
            // Inter-word: decode accumulated symbol and insert space
            if (pushSymbol() && !m_output.empty() && m_output.back() != ' ')
                m_output += ' ';
        }
    }
}

bool CwPllDecoder::pushSymbol()
{
    if (m_symbolBits.empty()) return false;
    char c = morseToAscii(m_symbolBits);
    m_symbolBits.clear();
    ++m_symbolCount;
    if (c == '\0') return false;
    if (!m_signalPresent) {
        // Gate closed: buffer the char so it can be recovered at TX onset.
        // Drop the oldest if the buffer is full (keeps only the most recent chars).
        m_retroBuf += c;
        if (static_cast<int>(m_retroBuf.size()) > kRetroBufMax)
            m_retroBuf.erase(0, 1);
        return false;
    }
    m_output += c;
    return true;
}

// ── Timing PLL ────────────────────────────────────────────────────────────────

void CwPllDecoder::updateDot(float measuredSec, bool reference1T)
{
    if (m_speedLocked) return;

    const float minDot = 1.2f / m_cfg.speedMax;
    const float maxDot = 1.2f / m_cfg.speedMin;
    const float newDot = std::clamp(measuredSec, minDot, maxDot);

    // Two-phase warmup: snap hard on first 4 symbols, moderate for next 4, then track slowly
    const float alpha = (m_symbolCount < 4) ? 0.60f
                      : (m_symbolCount < kWarmupSymbols) ? 0.35f
                      : 0.10f;
    (void)reference1T;  // both dot and inter-element contribute the same way
    m_dotSec = m_dotSec + alpha * (newDot - m_dotSec);
    m_dotSec = std::clamp(m_dotSec, minDot, maxDot);
}

void CwPllDecoder::recordTiming(float measured, float expected)
{
    if (expected <= 0) return;
    float err = std::abs(measured - expected) / expected;
    err = std::min(err, 1.0f);
    m_lastErr = err;  // used by appendOutput for hunt/emit gating
    m_errors[m_errIdx % kConfWindow] = err;
    ++m_errIdx;
    float sum = 0;
    for (float e : m_errors) sum += e;
    m_confidence = std::max(0.0f, 1.0f - sum / kConfWindow);
}

// ── Main processing loop ──────────────────────────────────────────────────────

std::string CwPllDecoder::process(const float* samples, int n)
{
    m_output.clear();

    // ── SNR-based signal-presence gate ───────────────────────────────────────
    // Measure Goertzel power at pitch and at two in-band reference points
    // (±kNoiseOffset Hz). Real CW concentrates energy at the carrier;
    // broadband noise is flat across the passband, giving SNR ≈ 1.
    // kNoiseOffset (100 Hz) stays inside a 250 Hz IF filter (±125 Hz).
    const float pitchPwr = goertzelPower(samples, n, m_pitch);
    const float noisePwr = std::max(1e-10f,
        0.5f * (goertzelPower(samples, n, m_pitch - kNoiseOffset) +
                goertzelPower(samples, n, m_pitch + kNoiseOffset)));
    const float snr = pitchPwr / noisePwr;
    if (!m_signalPresent && snr >= kSnrOnThresh)  m_signalPresent = true;
    if ( m_signalPresent && snr <  kSnrOffThresh) m_signalPresent = false;

    // On gate open: flush the retrospective buffer so chars decoded in the
    // pre-gate window appear at the head of the output.
    if (m_signalPresent && !m_prevSignalPresent) {
        m_output += m_retroBuf;
        m_retroBuf.clear();
    }
    m_prevSignalPresent = m_signalPresent;

    // ── Pitch sweep ───────────────────────────────────────────────────────────
    // Only sweep when the SNR says a signal is present; reuse the computation
    // above rather than re-computing average power. Retry every 50 ms while
    // absent so we lock quickly at transmission onset.
    m_reSweepCountdown -= static_cast<float>(n) / m_cfg.sampleRate;
    if (!m_pitchLocked && m_reSweepCountdown <= 0.0f) {
        if (m_signalPresent) {
            m_pitch = sweepPitch(samples, n);
            m_reSweepCountdown = kReSweepInterval;
        } else {
            m_reSweepCountdown = 0.05f;  // retry in 50 ms
        }
    }

    // ── Element detection (32-sample Goertzel blocks) ─────────────────────────
    const float blockDur = static_cast<float>(kBlockSize) / m_cfg.sampleRate;
    for (int i = 0; i + kBlockSize <= n; i += kBlockSize) {
        float power = goertzelPower(samples + i, kBlockSize, m_pitch);
        updateEnvelope(power);
        m_stateSec += blockDur;
        if (m_toneOn) {
            m_silenceSec   = 0.0f;
            m_didConfReset = false;
        } else {
            m_silenceSec += blockDur;
        }
    }

    // ── Confidence reset on long silence (display only) ───────────────────────
    if (!m_toneOn && m_silenceSec > kConfResetSilence && !m_didConfReset) {
        std::fill(std::begin(m_errors), std::end(m_errors), 1.0f);
        m_errIdx       = kConfWindow;
        m_confidence   = 0.0f;
        m_lastErr      = 1.0f;
        m_didConfReset = true;
    }

    // ── Flush pending character on operator pause (>7T silence) ───────────────
    if (!m_toneOn && m_stateSec > 7.0f * m_dotSec && !m_symbolBits.empty()) {
        if (pushSymbol() && !m_output.empty() && m_output.back() != ' ')
            m_output += ' ';
    }

    return m_output;
}

} // namespace AetherSDR
