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
#include <cassert>
#include <cmath>
#include <numbers>
#include <numeric>

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
        s_cosTable[j] = std::cos(static_cast<float>(j) * 2.0f * std::numbers::pi_v<float> / 256.0f);
}

// ── AetherFMDiscrimFrontEnd — filter design ───────────────────────────────────

void AetherFMDiscrimFrontEnd::buildPrefilter(double fMark, double fSpace,
                                              int bitrate, int sampleRate)
{
    buildBandpassCoeffs(fMark, fSpace, bitrate, sampleRate,
                        kPrefilterBaud, kPrefilterLenSym, kMaxFilterTaps,
                        m_preCoeffs, m_preTaps);
    m_preBuf.assign(m_preTaps, 0.0f);
    m_preBufPos = 0;
}

void AetherFMDiscrimFrontEnd::buildRrcLowpass(int bitrate, int sampleRate)
{
    float sps  = static_cast<float>(sampleRate) / static_cast<float>(bitrate);
    int   taps = (static_cast<int>(kRrcWidthSym * sps)) | 1;
    taps = std::min(taps, kMaxFilterTaps);

    m_lpCoeffs.resize(taps);
    for (int k = 0; k < taps; ++k) {
        float t = (k - (taps - 1.0f) * 0.5f) / sps;
        m_lpCoeffs[k] = rrcKernel(t, kRrcRolloff);
    }

    float sum = std::accumulate(m_lpCoeffs.begin(), m_lpCoeffs.end(), 0.0f);
    if (sum != 0.0f)
        for (auto& c : m_lpCoeffs) c /= sum;

    m_lpTaps = taps;
    m_cIBuf.assign(taps, 0.0f);
    m_cQBuf.assign(taps, 0.0f);
    m_lpBufPos = 0;
}

// ── AetherFMDiscrimFrontEnd — constructor ─────────────────────────────────────

AetherFMDiscrimFrontEnd::AetherFMDiscrimFrontEnd(
        double fMark, double fSpace, int bitrate, int sampleRate)
{
    std::call_once(s_cosOnce, buildCosTable);

    buildPrefilter(fMark, fSpace, bitrate, sampleRate);
    buildRrcLowpass(bitrate, sampleRate);

    const double fCenter = 0.5 * (fMark + fSpace);
    m_cOscDelta = static_cast<uint32_t>(
        std::round(std::pow(2.0, 32.0) * fCenter / sampleRate));

    // Scale factor: radians/sample → ±1.0 for expected mark/space tones.
    assert(fMark != fSpace);
    m_normalizeRpsam = static_cast<float>(
        1.0 / (0.5 * std::abs(fMark - fSpace) * 2.0 * std::numbers::pi / sampleRate));
}

// ── AetherFMDiscrimFrontEnd — processBlock ────────────────────────────────────

void AetherFMDiscrimFrontEnd::processBlock(const float* samples, int count,
                                            float* normRates) noexcept
{
    for (int i = 0; i < count; ++i) {
        float fsam = samples[i];

        // 1. Bandpass prefilter.
        pushSample(fsam, m_preBuf.data(), m_preBufPos, m_preTaps);
        fsam = convolve(m_preBuf.data(), m_preBufPos, m_preCoeffs.data(), m_preTaps);

        // 2. Mix with center-frequency oscillator.
        const float cC = fcos(m_cOscPhase), cS = fsin(m_cOscPhase);
        m_cOscPhase += m_cOscDelta;

        // Write both LP channels at the same ring slot, then advance once.
        m_cIBuf[m_lpBufPos] = fsam * cC;
        m_cQBuf[m_lpBufPos] = fsam * cS;
        if (++m_lpBufPos >= m_lpTaps) m_lpBufPos = 0;

        // 3. RRC lowpass.
        const float cI = convolve(m_cIBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);
        const float cQ = convolve(m_cQBuf.data(), m_lpBufPos, m_lpCoeffs.data(), m_lpTaps);

        // 4. Instantaneous phase via atan2.
        const float phase = std::atan2(cQ, cI);

        // 5. Differentiate phase → frequency deviation; handle ±π wrap.
        float rate = phase - m_prevPhase;
        if (rate >  std::numbers::pi_v<float>) rate -= 2.0f * std::numbers::pi_v<float>;
        if (rate < -std::numbers::pi_v<float>) rate += 2.0f * std::numbers::pi_v<float>;
        m_prevPhase = phase;

        // 6. Normalize: mark deviation ≈ −1, space ≈ +1.
        normRates[i] = rate * m_normalizeRpsam;
    }
}

// ── AetherFMDiscrimFrontEnd — reset ──────────────────────────────────────────

void AetherFMDiscrimFrontEnd::reset() noexcept
{
    std::fill(m_preBuf.begin(), m_preBuf.end(), 0.0f);
    std::fill(m_cIBuf.begin(), m_cIBuf.end(), 0.0f);
    std::fill(m_cQBuf.begin(), m_cQBuf.end(), 0.0f);
    m_cOscPhase = 0;
    m_prevPhase = 0.0f;
    m_preBufPos = 0;
    m_lpBufPos  = 0;
}

// ── AetherFMDiscrimSlicer — constructor ──────────────────────────────────────

AetherFMDiscrimSlicer::AetherFMDiscrimSlicer(int bitrate, int sampleRate, float sliceOffset)
    : m_sliceOffset(sliceOffset)
{
    m_pllStep = static_cast<int32_t>(
        std::round(4294967296.0 * bitrate / sampleRate));
}

// ── AetherFMDiscrimSlicer — DPLL ─────────────────────────────────────────────

void AetherFMDiscrimSlicer::nudgePll(float demodOut) noexcept
{
    m_prevPll = m_pll;
    m_pll = static_cast<int32_t>(
        static_cast<uint32_t>(m_pll) + static_cast<uint32_t>(m_pllStep));

    if (m_pll < 0 && m_prevPll >= 0) {
        float conf = std::min(std::fabs(demodOut), 1.0f);
        m_readyBit  = (demodOut > 0.0f) ? 1u : 0u;
        m_readyConf = conf;
        m_bitReady  = true;

        bool good = (conf > 0.1f);
        m_goodHist = (m_goodHist << 1) | (good ? 1u : 0u);
        m_badHist  = (m_badHist  << 1) | (good ? 0u : 1u);
        m_dcdScore = (m_dcdScore << 1);
        int g = std::popcount(m_goodHist & 0xffu);
        int b = std::popcount(m_badHist  & 0xffu);
        if (g - b >= 2) m_dcdScore |= 1u;
        int sc = std::popcount(m_dcdScore & 0xffu);
        if (!m_dataDetect && sc >= 6) m_dataDetect = true;
        if ( m_dataDetect && sc <  2) m_dataDetect = false;
    }

    bool d = (demodOut > 0.0f);
    if (d != m_prevDemod) {
        float inertia = m_dataDetect ? kPllLockedInertia : kPllSearchingInertia;
        m_pll = static_cast<int32_t>(static_cast<float>(m_pll) * inertia);
    }
    m_prevDemod = d;
}

// ── AetherFMDiscrimSlicer — process ──────────────────────────────────────────

bool AetherFMDiscrimSlicer::process(float normRate, demod_result& result) noexcept
{
    nudgePll(normRate + m_sliceOffset);
    if (!m_bitReady)
        return false;
    m_bitReady         = false;
    result.bit        = m_readyBit;
    result.confidence = static_cast<double>(m_readyConf);
    return true;
}

// ── AetherFMDiscrimSlicer — reset ────────────────────────────────────────────

void AetherFMDiscrimSlicer::reset() noexcept
{
    m_pll = m_prevPll = 0;
    m_prevDemod = m_dataDetect = false;
    m_goodHist = m_badHist = m_dcdScore = 0;
    m_bitReady = false;
    m_readyBit = 0;
    m_readyConf = 0.0f;
}

// ── AetherFMDiscrimDemod — convenience wrapper ────────────────────────────────

AetherFMDiscrimDemod::AetherFMDiscrimDemod(
        double fMark, double fSpace, int bitrate, int sampleRate, float sliceOffset)
    : m_frontEnd(fMark, fSpace, bitrate, sampleRate)
    , m_slicer(bitrate, sampleRate, sliceOffset)
{}

bool AetherFMDiscrimDemod::try_demodulate(double sample, demod_result& result) noexcept
{
    const float fsam = static_cast<float>(sample);
    float normRate;
    m_frontEnd.processBlock(&fsam, 1, &normRate);
    return m_slicer.process(normRate, result);
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
    m_frontEnd.reset();
    m_slicer.reset();
}

} // namespace AetherDemod
