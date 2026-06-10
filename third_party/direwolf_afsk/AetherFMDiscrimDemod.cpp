// AetherFMDiscrimDemod.cpp
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Reference: src/demod_afsk.c (profile B) and src/dsp.c from Dire Wolf.

#include "AetherFMDiscrimDemod.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AetherDemod {

// ── Profile-B tuning constants (from Dire Wolf demod_afsk.c) ─────────────────

static constexpr float kPrefilterBaud       = 0.19f;   // BPF skirt each side
static constexpr float kPrefilterLenSym     = 8.163f;  // filter length in symbols
static constexpr float kRrcRolloff          = 0.40f;   // wider than profile-A (0.20)
static constexpr float kRrcWidthSym         = 2.00f;   // shorter than profile-A (2.80)
static constexpr float kPllLockedInertia    = 0.74f;
static constexpr float kPllSearchingInertia = 0.50f;
static constexpr int   kMaxFilterTaps       = 2048;

// ── File-local DSP helpers ────────────────────────────────────────────────────

static void pushSample(float val, float* buf, int& wrPos, int taps) noexcept
{
    buf[wrPos] = val;
    if (++wrPos >= taps) wrPos = 0;
}

static float convolve(const float* buf, int wrPos,
                       const float* __restrict__ coeffs, int taps) noexcept
{
    // Ring-buffer dot product: coeffs[0] matches the newest sample (wrPos-1), etc.
    float sum = 0.0f;
    int idx = wrPos - 1;
    if (idx < 0) idx += taps;
    for (int j = 0; j < taps; ++j) {
        sum += coeffs[j] * buf[idx];
        if (--idx < 0) idx += taps;
    }
    return sum;
}

// ── AetherFMDiscrimFrontEnd — static members ─────────────────────────────────

float             AetherFMDiscrimFrontEnd::s_cosTable[256];
std::once_flag    AetherFMDiscrimFrontEnd::s_cosOnce;

void AetherFMDiscrimFrontEnd::buildCosTable() noexcept
{
    for (int j = 0; j < 256; ++j)
        s_cosTable[j] = std::cos(static_cast<float>(j) * 2.0f * float(M_PI) / 256.0f);
}

// ── AetherFMDiscrimFrontEnd — filter design ───────────────────────────────────

void AetherFMDiscrimFrontEnd::buildPrefilter(double fMark, double fSpace,
                                              int bitrate, int sampleRate) noexcept
{
    buildBandpassCoeffs(fMark, fSpace, bitrate, sampleRate,
                        kPrefilterBaud, kPrefilterLenSym, kMaxFilterTaps,
                        preCoeffs_, preTaps_);
    preBuf_.assign(preTaps_, 0.0f);
}

void AetherFMDiscrimFrontEnd::buildRrcLowpass(int bitrate, int sampleRate) noexcept
{
    float sps  = static_cast<float>(sampleRate) / static_cast<float>(bitrate);
    int   taps = (static_cast<int>(kRrcWidthSym * sps)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    lpCoeffs_.resize(taps);
    for (int k = 0; k < taps; ++k) {
        float t = (k - (taps - 1.0f) * 0.5f) / sps;
        lpCoeffs_[k] = rrcKernel(t, kRrcRolloff);
    }

    float sum = std::accumulate(lpCoeffs_.begin(), lpCoeffs_.end(), 0.0f);
    if (sum != 0.0f)
        for (auto& c : lpCoeffs_) c /= sum;

    lpTaps_ = taps;
    cIBuf_.assign(taps, 0.0f);
    cQBuf_.assign(taps, 0.0f);
}

// ── AetherFMDiscrimFrontEnd — constructor ─────────────────────────────────────

AetherFMDiscrimFrontEnd::AetherFMDiscrimFrontEnd(
        double fMark, double fSpace, int bitrate, int sampleRate)
{
    std::call_once(s_cosOnce, buildCosTable);

    buildPrefilter(fMark, fSpace, bitrate, sampleRate);
    buildRrcLowpass(bitrate, sampleRate);

    const double fCenter = 0.5 * (fMark + fSpace);
    cOscDelta_ = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fCenter / sampleRate));

    // Scale factor: radians/sample → ±1.0 for expected mark/space tones.
    normalizeRpsam_ = static_cast<float>(
        1.0 / (0.5 * std::abs(fMark - fSpace) * 2.0 * M_PI / sampleRate));
}

// ── AetherFMDiscrimFrontEnd — processBlock ────────────────────────────────────

void AetherFMDiscrimFrontEnd::processBlock(const float* samples, int count,
                                            float* normRates) noexcept
{
    for (int i = 0; i < count; ++i) {
        float fsam = samples[i];

        // 1. Bandpass prefilter.
        pushSample(fsam, preBuf_.data(), preBufPos_, preTaps_);
        fsam = convolve(preBuf_.data(), preBufPos_, preCoeffs_.data(), preTaps_);

        // 2. Mix with center-frequency oscillator.
        const float cC = fcos(cOscPhase_), cS = fsin(cOscPhase_);
        cOscPhase_ += cOscDelta_;

        // Write both LP channels at the same ring slot, then advance once.
        cIBuf_[lpBufPos_] = fsam * cC;
        cQBuf_[lpBufPos_] = fsam * cS;
        if (++lpBufPos_ >= lpTaps_) lpBufPos_ = 0;

        // 3. RRC lowpass.
        const float cI = convolve(cIBuf_.data(), lpBufPos_, lpCoeffs_.data(), lpTaps_);
        const float cQ = convolve(cQBuf_.data(), lpBufPos_, lpCoeffs_.data(), lpTaps_);

        // 4. Instantaneous phase via atan2.
        const float phase = std::atan2(cQ, cI);

        // 5. Differentiate phase → frequency deviation; handle ±π wrap.
        float rate = phase - prevPhase_;
        if (rate >  float(M_PI)) rate -= 2.0f * float(M_PI);
        if (rate < -float(M_PI)) rate += 2.0f * float(M_PI);
        prevPhase_ = phase;

        // 6. Normalize: mark deviation ≈ −1, space ≈ +1.
        normRates[i] = rate * normalizeRpsam_;
    }
}

// ── AetherFMDiscrimFrontEnd — reset ──────────────────────────────────────────

void AetherFMDiscrimFrontEnd::reset() noexcept
{
    std::fill(preBuf_.begin(), preBuf_.end(), 0.0f);
    std::fill(cIBuf_.begin(), cIBuf_.end(), 0.0f);
    std::fill(cQBuf_.begin(), cQBuf_.end(), 0.0f);
    cOscPhase_ = 0;
    prevPhase_ = 0.0f;
    preBufPos_ = 0;
    lpBufPos_  = 0;
}

// ── AetherFMDiscrimSlicer — constructor ──────────────────────────────────────

AetherFMDiscrimSlicer::AetherFMDiscrimSlicer(int bitrate, int sampleRate, float sliceOffset)
    : sliceOffset_(sliceOffset)
{
    pllStep_ = static_cast<int32_t>(
        std::round(4294967296.0 * bitrate / sampleRate));
}

// ── AetherFMDiscrimSlicer — DPLL ─────────────────────────────────────────────

void AetherFMDiscrimSlicer::nudgePll(float demodOut) noexcept
{
    prevPll_ = pll_;
    pll_ = static_cast<int32_t>(
        static_cast<uint32_t>(pll_) + static_cast<uint32_t>(pllStep_));

    if (pll_ < 0 && prevPll_ >= 0) {
        float conf = std::min(std::fabs(demodOut), 1.0f);
        readyBit_  = (demodOut > 0.0f) ? 1u : 0u;
        readyConf_ = conf;
        bitReady_  = true;

        bool good = (conf > 0.1f);
        goodHist_ = (goodHist_ << 1) | (good ? 1u : 0u);
        badHist_  = (badHist_  << 1) | (good ? 0u : 1u);
        dcdScore_ = (dcdScore_ << 1);
        int g = std::popcount(goodHist_ & 0xffu);
        int b = std::popcount(badHist_  & 0xffu);
        if (g - b >= 2) dcdScore_ |= 1u;
        int sc = std::popcount(dcdScore_ & 0xffu);
        if (!dataDetect_ && sc >= 6) dataDetect_ = true;
        if ( dataDetect_ && sc <  2) dataDetect_ = false;
    }

    bool d = (demodOut > 0.0f);
    if (d != prevDemod_) {
        float inertia = dataDetect_ ? kPllLockedInertia : kPllSearchingInertia;
        pll_ = static_cast<int32_t>(static_cast<float>(pll_) * inertia);
    }
    prevDemod_ = d;
}

// ── AetherFMDiscrimSlicer — process ──────────────────────────────────────────

bool AetherFMDiscrimSlicer::process(float normRate, demod_result& result) noexcept
{
    nudgePll(normRate + sliceOffset_);
    if (!bitReady_)
        return false;
    bitReady_         = false;
    result.bit        = readyBit_;
    result.confidence = static_cast<double>(readyConf_);
    return true;
}

// ── AetherFMDiscrimSlicer — reset ────────────────────────────────────────────

void AetherFMDiscrimSlicer::reset() noexcept
{
    pll_ = prevPll_ = 0;
    prevDemod_ = dataDetect_ = false;
    goodHist_ = badHist_ = dcdScore_ = 0;
    bitReady_ = false;
    readyBit_ = 0;
    readyConf_ = 0.0f;
}

// ── AetherFMDiscrimDemod — convenience wrapper ────────────────────────────────

AetherFMDiscrimDemod::AetherFMDiscrimDemod(
        double fMark, double fSpace, int bitrate, int sampleRate, float sliceOffset)
    : frontEnd_(fMark, fSpace, bitrate, sampleRate)
    , slicer_(bitrate, sampleRate, sliceOffset)
{}

bool AetherFMDiscrimDemod::try_demodulate(double sample, demod_result& result) noexcept
{
    const float fsam = static_cast<float>(sample);
    float normRate;
    frontEnd_.processBlock(&fsam, 1, &normRate);
    return slicer_.process(normRate, result);
}

bool AetherFMDiscrimDemod::try_demodulate(double sample, uint8_t& bit) noexcept
{
    demod_result r;
    if (try_demodulate(sample, r)) {
        bit = r.bit;
        return true;
    }
    return false;
}

void AetherFMDiscrimDemod::reset() noexcept
{
    frontEnd_.reset();
    slicer_.reset();
}

} // namespace AetherDemod
