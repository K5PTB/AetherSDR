#pragma once

#include <string>

namespace AetherSDR {

// PLL-based CW (Morse code) decoder operating at native 24 kHz.
//
// Unlike ggmorse (which resamples to 4 kHz and works on 32 ms frames),
// this decoder uses 32-sample Goertzel blocks (1.33 ms at 24 kHz), giving
// ~18 blocks per dot at 50 WPM for clean edge detection.
//
// Pipeline:
//   audio → Goertzel power (32-sample blocks) → AGC + hysteresis
//     → rising/falling edges → PLL timing recovery → dot/dash → ASCII
//
// Noise suppression between transmissions:
//   Each process() call measures the signal-to-noise ratio at the pitch
//   frequency versus two off-pitch reference points (±kNoiseOffset Hz).
//   Real CW concentrates energy at the carrier; broadband noise is flat.
//   Output is gated on SNR: nothing emits until SNR ≥ kSnrOnThresh, and
//   output stops as soon as SNR drops below kSnrOffThresh.  This reacts
//   within one 32 ms frame — far faster than any timing-based approach.
//
// Thread-safety: not thread-safe. Call process() from a single thread.
class CwPllDecoder {
public:
    struct Config {
        float sampleRate =  24000.0f;
        float pitchHz    =     -1.0f;  // -1 = auto-sweep each kReSweepInterval
        float pitchMin   =    300.0f;  // Hz
        float pitchMax   =   1200.0f;  // Hz
        float speedMin   =      5.0f;  // WPM — clamps dot estimate
        float speedMax   =     80.0f;
    };

    CwPllDecoder();
    explicit CwPllDecoder(const Config& cfg);

    void setConfig(const Config& cfg);
    void lockPitch(float hz);   // -1 = unlock and re-sweep
    void lockSpeed(float wpm);  // -1 = unlock
    void reset();

    // Feed mono float32 samples at cfg.sampleRate.
    // Returns all newly decoded characters since the last call (may be empty).
    std::string process(const float* samples, int n);

    float estimatedPitch() const { return m_pitch; }
    float estimatedSpeed() const;
    float confidence()     const { return m_confidence; }
    bool  signalPresent()  const { return m_signalPresent; }

private:
    static constexpr int   kBlockSize        = 32;    // Goertzel window (samples)
    static constexpr float kReSweepInterval  = 0.5f;  // seconds between pitch sweeps
    static constexpr float kEnvSweepStep     = 25.0f; // Hz step for coarse sweep
    static constexpr float kEnvSweepRefine   = 5.0f;  // Hz step for fine sweep
    static constexpr int   kConfWindow        = 12;    // symbols in confidence rolling window
    static constexpr int   kWarmupSymbols     = 8;    // fast EMA convergence for this many symbols
    static constexpr int   kOnDebounce        = 3;    // consecutive blocks required to declare tone-on
    static constexpr float kConfResetSilence  = 1.0f; // seconds of silence before confidence reset

    // SNR-based signal-presence gate.
    // kNoiseOffset must stay inside the user's IF filter passband so the
    // reference samples see real in-band noise, not filter roll-off.
    // 100 Hz fits inside a 250 Hz filter (±125 Hz half-bandwidth).
    static constexpr float kNoiseOffset  = 100.0f; // Hz offset for noise-floor measurement
    static constexpr float kSnrOnThresh  =   4.0f; // SNR to open output gate  (~6 dB)
    static constexpr float kSnrOffThresh =   2.0f; // SNR to close output gate (~3 dB)
    static constexpr int   kRetroBufMax  =      4; // chars to hold before gate opens

    // Pitch detection
    float goertzelPower(const float* samples, int n, float freq) const;
    float sweepPitch(const float* buf, int n) const;

    // Envelope
    void  updateEnvelope(float power);

    // Edge pipeline
    void  onEdge(bool rising);
    bool  pushSymbol();

    // Timing PLL
    void  updateDot(float measuredSec, bool reference1T);
    void  recordTiming(float measured, float expected);

    // Morse lookup
    static char morseToAscii(const std::string& bits);

    Config m_cfg;

    // Pitch state
    float  m_pitch           = 600.0f;
    bool   m_pitchLocked     = false;
    float  m_reSweepCountdown= 0.0f;

    // Timing PLL
    float  m_dotSec          = 0.060f;
    bool   m_speedLocked     = false;
    int    m_symbolCount     = 0;

    // Envelope / edge detector
    float  m_envSmooth       = 0.0f;
    float  m_envMax          = 1e-9f;
    bool   m_toneOn          = false;
    float  m_stateSec        = 0.0f;
    int    m_onCount         = 0;

    // Symbol accumulation
    std::string m_symbolBits;
    std::string m_output;

    // Confidence (rolling timing error — used for display)
    float  m_errors[kConfWindow] = {};
    int    m_errIdx              = 0;
    float  m_confidence          = 0.0f;
    float  m_lastErr             = 0.0f;

    // SNR-based signal gate
    bool        m_signalPresent     = false;
    bool        m_prevSignalPresent = false;

    // Retrospective buffer: chars decoded just before gate opens, flushed at onset
    std::string m_retroBuf;

    // Inter-transmission silence tracking (for confidence reset)
    float  m_silenceSec          = 0.0f;
    bool   m_didConfReset        = false;
};

} // namespace AetherSDR
