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

private:
    static constexpr int   kBlockSize        = 32;    // Goertzel window (samples)
    static constexpr float kReSweepInterval  = 0.5f;  // seconds between pitch sweeps
    static constexpr float kEnvSweepStep     = 25.0f; // Hz step for coarse sweep
    static constexpr float kEnvSweepRefine   = 5.0f;  // Hz step for fine sweep
    static constexpr int   kConfWindow        = 12;    // symbols in confidence average
    static constexpr int   kWarmupSymbols     = 8;    // fast EMA convergence for this many symbols

    // Pitch detection
    float goertzelPower(const float* samples, int n, float freq) const;
    float sweepPitch(const float* buf, int n) const;

    // Envelope
    void  updateEnvelope(float power);

    // Edge pipeline
    void  onEdge(bool rising);
    void  pushSymbol();

    // Timing PLL
    void  updateDot(float measuredSec, bool reference1T);
    void  recordTiming(float measured, float expected);

    // Morse lookup
    static char morseToAscii(const std::string& bits);

    Config m_cfg;

    // Pitch state
    float  m_pitch           = 600.0f;
    bool   m_pitchLocked     = false;
    float  m_reSweepCountdown= 0.0f;  // seconds remaining until next sweep

    // Timing PLL
    float  m_dotSec          = 0.060f;  // dot duration estimate (20 WPM)
    bool   m_speedLocked     = false;
    int    m_symbolCount     = 0;       // total symbols decoded (for warm-up)

    // Envelope / edge detector
    float  m_envSmooth       = 0.0f;
    float  m_envMax          = 1e-9f;
    bool   m_toneOn          = false;
    float  m_stateSec        = 0.0f;   // time in current on/off state

    // Symbol accumulation
    std::string m_symbolBits;           // current character's dots/dashes
    std::string m_output;               // decoded text ready to return

    // Confidence (rolling timing error)
    float  m_errors[kConfWindow] = {};
    int    m_errIdx              = 0;
    float  m_confidence          = 0.0f;
};

} // namespace AetherSDR
