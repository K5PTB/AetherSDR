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
    AetherFMDiscrimFrontEnd() = default;
    AetherFMDiscrimFrontEnd(double fMark, double fSpace, int bitrate, int sampleRate);

    // Fill normRates[0..count-1] from samples[0..count-1].
    void processBlock(const float* samples, int count, float* normRates) noexcept;
    void reset() noexcept;

private:
    std::vector<float> preCoeffs_;
    std::vector<float> preBuf_;
    int preTaps_{0};
    int preBufPos_{0};  // ring-buffer write position

    uint32_t cOscPhase_{0};
    uint32_t cOscDelta_{0};

    std::vector<float> lpCoeffs_;
    std::vector<float> cIBuf_, cQBuf_;
    int lpTaps_{0};
    int lpBufPos_{0};   // ring-buffer write position (shared by cIBuf_ and cQBuf_)

    float prevPhase_{0.0f};
    float normalizeRpsam_{0.0f};

    static float             s_cosTable[256];
    static std::once_flag    s_cosOnce;
    static void              buildCosTable() noexcept;

    static inline float fcos(uint32_t phase) noexcept
        { return s_cosTable[(phase >> 24) & 0xffu]; }
    static inline float fsin(uint32_t phase) noexcept
        { return s_cosTable[((phase >> 24) - 64u) & 0xffu]; }

    void buildPrefilter(double fMark, double fSpace, int bitrate, int sampleRate) noexcept;
    void buildRrcLowpass(int bitrate, int sampleRate) noexcept;
};

// Per-slicer DPLL.  Consumes norm_rate values produced by AetherFMDiscrimFrontEnd.
// sliceOffset shifts the decision threshold: 0.0f for single-slicer B mode,
// evenly spread -0.5 → +0.5 for B+ multi-slicer mode.
class AetherFMDiscrimSlicer {
public:
    AetherFMDiscrimSlicer() = default;
    AetherFMDiscrimSlicer(int bitrate, int sampleRate, float sliceOffset = 0.0f);

    // Returns true and fills result when a bit clock fires.
    bool process(float normRate, demod_result& result) noexcept;
    void reset() noexcept;

private:
    float sliceOffset_{0.0f};

    int32_t pll_{0};
    int32_t prevPll_{0};
    int32_t pllStep_{0};
    bool    prevDemod_{false};
    bool    dataDetect_{false};

    uint32_t goodHist_{0};
    uint32_t badHist_{0};
    uint32_t dcdScore_{0};

    bool    bitReady_{false};
    uint8_t readyBit_{0};
    float   readyConf_{0.0f};

    void nudgePll(float demodOut) noexcept;
};

// Convenience single-sample wrapper: one front-end + one slicer.
// Preserves the try_demodulate() API for unit tests and standalone callers.
class AetherFMDiscrimDemod {
public:
    AetherFMDiscrimDemod() = default;

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
