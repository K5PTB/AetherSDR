// AetherFMDiscrimDemod.h
//
// AFSK FM-discriminator demodulator — AetherSDR / Profile B
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Algorithm: Direwolf profile-B (formerly D, renamed in v1.7)
//   BPF → center-freq IQ mix → RRC LPF → atan2 → d/dt → normalize → DPLL
//
// Complements AetherAFSKDemod (profile A).  A uses separate mark/space
// correlators; B uses a single FM discriminator at the center frequency.
// They fail on different signal types so running both in parallel (AB+)
// captures the widest range of real-world VHF FM packets.
//
// For multi-slicer use (B+ mode) the pipeline is split into two classes that
// mirror Direwolf's own structure: one AetherFMDiscrimFrontEnd shared across
// all slicers in a group computes norm_rate once per sample; each
// AetherFMDiscrimSlicer adds its own threshold offset and runs an independent
// DPLL.  This matches Direwolf demod_afsk.c exactly and avoids recomputing
// the BPF/IQ/LP/atan2 chain N times for N slicers.

#pragma once

#include "AetherAFSKDemod.h"   // for demod_result

#include <cstdint>
#include <mutex>
#include <vector>

namespace AetherDemod {

// Shared signal-processing front-end.
// BPF → center-freq IQ mix → RRC LPF → atan2 → d/dt → normalize.
// Construct once per B-mode group; share across all AetherFMDiscrimSlicer
// instances in the group.
class AetherFMDiscrimFrontEnd {
public:
    AetherFMDiscrimFrontEnd() = delete;
    AetherFMDiscrimFrontEnd(double fMark, double fSpace, int bitrate, int sampleRate);

    // Fill normRates[0..count-1] from samples[0..count-1].
    void processBlock(const float* samples, int count, float* normRates) noexcept;
    void reset() noexcept;

private:
    std::vector<float> m_preCoeffs;
    std::vector<float> m_preBuf;
    int m_preTaps{0};
    int m_preBufPos{0};  // ring-buffer write position

    uint32_t m_cOscPhase{0};
    uint32_t m_cOscDelta{0};

    std::vector<float> m_lpCoeffs;
    std::vector<float> m_cIBuf, m_cQBuf;
    int m_lpTaps{0};
    int m_lpBufPos{0};   // ring-buffer write position (shared by m_cIBuf and m_cQBuf)

    float m_prevPhase{0.0f};
    float m_normalizeRpsam{0.0f};

    static float             s_cosTable[256];
    static std::once_flag    s_cosOnce;
    static void              buildCosTable() noexcept;

    static inline float fcos(uint32_t phase) noexcept
        { return s_cosTable[(phase >> 24) & 0xffu]; }
    static inline float fsin(uint32_t phase) noexcept
        { return s_cosTable[((phase >> 24) - 64u) & 0xffu]; }

    void buildPrefilter(double fMark, double fSpace, int bitrate, int sampleRate);
    void buildRrcLowpass(int bitrate, int sampleRate);
};

// Per-slicer DPLL.  Consumes norm_rate values produced by AetherFMDiscrimFrontEnd.
// sliceOffset shifts the decision threshold: 0.0f for single-slicer B mode,
// evenly spread -0.5 → +0.5 for B+ multi-slicer mode.
class AetherFMDiscrimSlicer {
public:
    AetherFMDiscrimSlicer() = delete;
    AetherFMDiscrimSlicer(int bitrate, int sampleRate, float sliceOffset = 0.0f);

    // Returns true and fills result when a bit clock fires.
    bool process(float normRate, demod_result& result) noexcept;
    void reset() noexcept;

private:
    float m_sliceOffset{0.0f};

    int32_t m_pll{0};
    int32_t m_prevPll{0};
    int32_t m_pllStep{0};
    bool    m_prevDemod{false};
    bool    m_dataDetect{false};

    uint32_t m_goodHist{0};
    uint32_t m_badHist{0};
    uint32_t m_dcdScore{0};

    bool    m_bitReady{false};
    uint8_t m_readyBit{0};
    float   m_readyConf{0.0f};

    void nudgePll(float demodOut) noexcept;
};

// Convenience single-sample wrapper: one front-end + one slicer.
// Preserves the try_demodulate() API for unit tests and standalone callers.
class AetherFMDiscrimDemod {
public:
    AetherFMDiscrimDemod() = delete;

    AetherFMDiscrimDemod(double fMark, double fSpace, int bitrate, int sampleRate,
                         float sliceOffset = 0.0f);

    bool try_demodulate(double sample, demod_result& result) noexcept;
    bool try_demodulate(double sample, uint8_t& bit)         noexcept;
    void reset() noexcept;

private:
    AetherFMDiscrimFrontEnd frontEnd_;
    AetherFMDiscrimSlicer   slicer_;
};

} // namespace AetherDemod
