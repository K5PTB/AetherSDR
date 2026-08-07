#include "DeepCwEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace AetherSDR {

// The model alphabet, indices 0-40 (blank is 41, outside this string).
// Pinned to model.onnx.json "chars": , . / 0-9 ? A-Z space.
static constexpr const char* kChars = ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ ";

// M_PI is not guaranteed in <cmath> without _USE_MATH_DEFINES (MSVC); use our own.
static constexpr double kPi = 3.14159265358979323846;

// Self-contained iterative radix-2 FFT (N a power of two), in-place on re/im.
// Deliberately NOT FFTW: FFTW's planner (fftwf_plan_*/fftwf_destroy_plan) is not
// thread-safe and this runs on the CW decode worker thread concurrently with the
// app's other FFTW users (WDSP, spectral NR) — that race corrupts FFTW's global
// plan state and aborts (crash in hinsert). This routine holds no global state.
static void fftRadix2(float* re, float* im, int n)
{
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    // Butterflies (decimation-in-time), twiddles by incremental rotation.
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const float wr = static_cast<float>(std::cos(ang));
        const float wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cur = 1.0f, cui = 0.0f;   // current twiddle
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float xr = re[b] * cur - im[b] * cui;
                const float xi = re[b] * cui + im[b] * cur;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                const float ncur = cur * wr - cui * wi;
                cui = cur * wi + cui * wr;
                cur = ncur;
            }
        }
    }
}

DeepCwEngine::DeepCwEngine()
#ifdef HAVE_ONNX
    : m_env(ORT_LOGGING_LEVEL_WARNING, "AetherSDR-DeepCW")
    , m_memInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
#endif
{
#ifdef HAVE_ONNX
    // CW audio is slow and windows are short; one intra-op thread keeps it off
    // the hot path and deterministic (mirrors SignalClassifier).
    m_sessionOpts.SetIntraOpNumThreads(1);
    m_sessionOpts.SetGraphOptimizationLevel(ORT_ENABLE_BASIC);
#endif
}

DeepCwEngine::~DeepCwEngine() = default;

bool DeepCwEngine::loadModel(const std::string& path)
{
#ifdef HAVE_ONNX
    try {
#ifdef _WIN32
        const std::wstring wpath(path.begin(), path.end());
        m_session = std::make_unique<Ort::Session>(m_env, wpath.c_str(), m_sessionOpts);
#else
        m_session = std::make_unique<Ort::Session>(m_env, path.c_str(), m_sessionOpts);
#endif
        m_loaded = true;
        return true;
    } catch (const Ort::Exception& ex) {
        std::fprintf(stderr, "DeepCwEngine: failed to load model '%s': %s\n",
                     path.c_str(), ex.what());
        return false;
    }
#else
    (void)path;
    std::fprintf(stderr, "DeepCwEngine: built without HAVE_ONNX; cannot load %s\n",
                 path.c_str());
    return false;
#endif
}

// Byte-for-byte the reference decode_morse.py resample_linear().
std::vector<float> DeepCwEngine::resampleTo3200(const std::vector<float>& in, int srcRate) const
{
    if (srcRate == kModelSampleRate || in.empty()) {
        return in;
    }
    const long n = static_cast<long>(in.size());
    const long target = std::lround(static_cast<double>(n) * kModelSampleRate / srcRate);
    std::vector<float> out(static_cast<size_t>(std::max<long>(target, 0)));
    for (long i = 0; i < target; ++i) {
        const double pos = static_cast<double>(i) * srcRate / kModelSampleRate;
        const long left = static_cast<long>(std::floor(pos));
        const long right = std::min(left + 1, n - 1);
        const float frac = static_cast<float>(pos - left);
        out[static_cast<size_t>(i)] = in[static_cast<size_t>(left)] * (1.0f - frac)
                                    + in[static_cast<size_t>(right)] * frac;
    }
    return out;
}

// Replicates decode_morse.py audio_to_spectrogram(): reflect-pad by fft/2,
// periodic Hann (np.hanning(N+1)[:-1]), rFFT magnitude over bins [32,97),
// then log1p. Row-major [frames * kFreqBins].
std::vector<float> DeepCwEngine::spectrogram(const std::vector<float>& audio3200, int* frames) const
{
    *frames = 0;
    if (static_cast<int>(audio3200.size()) < kFftLength) {
        return {};
    }

    // Frequency bin range: bin_hz = sr/fft; start=ceil(min/bin), stop=floor(max/bin)+1.
    const double binHz = static_cast<double>(kModelSampleRate) / kFftLength;
    const int startBin = static_cast<int>(std::ceil(kMinFreqHz / binHz));
    const int stopBin  = static_cast<int>(std::floor(kMaxFreqHz / binHz)) + 1;
    if (stopBin - startBin != kFreqBins) {
        std::fprintf(stderr, "DeepCwEngine: bin range %d..%d != %d bins\n",
                     startBin, stopBin, kFreqBins);
        return {};
    }

    const int pad = kFftLength / 2;

    // Reflect-pad the signal by `pad` on each side (numpy mode="reflect":
    // mirror without repeating the edge sample).
    std::vector<float> a;
    a.reserve(audio3200.size() + 2 * pad);
    for (int i = 0; i < pad; ++i) a.push_back(audio3200[pad - i]);
    a.insert(a.end(), audio3200.begin(), audio3200.end());
    const int last = static_cast<int>(audio3200.size()) - 1;
    for (int i = 0; i < pad; ++i) a.push_back(audio3200[last - 1 - i]);

    // Periodic Hann of length fft: np.hanning(fft+1)[:-1] -> 0.5-0.5cos(2*pi*n/fft).
    std::vector<float> window(kFftLength);
    for (int n = 0; n < kFftLength; ++n) {
        window[n] = 0.5f - 0.5f * std::cos(2.0 * kPi * n / kFftLength);
    }

    const int nFrames = 1 + (static_cast<int>(a.size()) - kFftLength) / kHopLength;

    // Full N-point complex FFT of the windowed real frame (imag = 0); bins
    // 0..N/2 hold the one-sided spectrum, so bin[startBin..stopBin) are the
    // 400-1200 Hz magnitudes we keep. Matches numpy.fft.rfft (unnormalized).
    std::vector<float> spec(static_cast<size_t>(nFrames) * kFreqBins);
    std::vector<float> re(kFftLength), im(kFftLength);
    for (int f = 0; f < nFrames; ++f) {
        const int start = f * kHopLength;
        for (int n = 0; n < kFftLength; ++n) {
            re[n] = a[start + n] * window[n];
            im[n] = 0.0f;
        }
        fftRadix2(re.data(), im.data(), kFftLength);
        float* row = &spec[static_cast<size_t>(f) * kFreqBins];
        for (int k = 0; k < kFreqBins; ++k) {
            const int bin = startBin + k;
            const float mag = std::sqrt(re[bin] * re[bin] + im[bin] * im[bin]);
            row[k] = std::log1p(mag);   // normalization == "log1p"
        }
    }

    *frames = nFrames;
    return spec;
}

std::string DeepCwEngine::ctcDecode(const float* logProbs, int frames, float* avgConfidence) const
{
    std::string text;
    int previous = -1;
    double confSum = 0.0;
    int    confCount = 0;
    for (int t = 0; t < frames; ++t) {
        const float* row = logProbs + static_cast<size_t>(t) * kNumClasses;
        int best = 0;
        float bestVal = row[0];
        for (int c = 1; c < kNumClasses; ++c) {
            if (row[c] > bestVal) { bestVal = row[c]; best = c; }
        }
        if (best == kBlankIndex) { previous = -1; continue; }
        if (best != previous) {
            text.push_back(kChars[best]);
            // Confidence = softmax(row)[best], computed stably via logsumexp so
            // it is a valid [0,1] probability whether the model emits logits or
            // log-probabilities.
            double sumExp = 0.0;
            for (int c = 0; c < kNumClasses; ++c)
                sumExp += std::exp(static_cast<double>(row[c]) - bestVal);
            confSum += 1.0 / sumExp;   // exp(bestVal-bestVal)/sumExp = 1/sumExp
            ++confCount;
        }
        previous = best;
    }
    if (avgConfidence)
        *avgConfidence = confCount > 0 ? static_cast<float>(confSum / confCount) : 1.0f;
    return text;
}

std::string DeepCwEngine::decode(const std::vector<float>& audio, int sampleRateHz,
                                 float* avgConfidence, float* pitchHz) const
{
    if (avgConfidence) *avgConfidence = 1.0f;
    if (pitchHz) *pitchHz = 0.0f;
#ifdef HAVE_ONNX
    if (!m_loaded || !m_session) { return {}; }

    const std::vector<float> a = resampleTo3200(audio, sampleRateHz);
    int frames = 0;
    const std::vector<float> spec = spectrogram(a, &frames);
    if (frames == 0 || spec.empty()) { return {}; }

    // Dominant tone = peak-energy frequency bin summed across frames. Used for
    // zero-beat and the pitch readout (the model itself takes no pitch input).
    if (pitchHz) {
        const double binHz = static_cast<double>(kModelSampleRate) / kFftLength;
        const int startBin = static_cast<int>(std::ceil(kMinFreqHz / binHz));
        int peak = 0; double peakE = -1.0;
        for (int k = 0; k < kFreqBins; ++k) {
            double e = 0.0;
            for (int f = 0; f < frames; ++f) e += spec[static_cast<size_t>(f) * kFreqBins + k];
            if (e > peakE) { peakE = e; peak = k; }
        }
        *pitchHz = static_cast<float>((startBin + peak) * binHz);
    }

    try {
        const std::array<int64_t, 4> shape{1, 1, frames, kFreqBins};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            m_memInfo,
            const_cast<float*>(spec.data()), spec.size(),
            shape.data(), shape.size());

        const char* inputNames[]  = {"spectrogram"};
        const char* outputNames[] = {"log_probs"};

        auto outputs = m_session->Run(Ort::RunOptions{nullptr},
                                      inputNames, &input, 1,
                                      outputNames, 1);

        const float* logProbs = outputs[0].GetTensorData<float>();
        // Output shape [1, frames, kNumClasses]; frames may be re-derived from
        // the tensor, but the model's time dim tracks our input frame count.
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto outShape = info.GetShape();
        const int outFrames = outShape.size() >= 2 ? static_cast<int>(outShape[outShape.size() - 2]) : frames;
        return ctcDecode(logProbs, outFrames, avgConfidence);
    } catch (const Ort::Exception& ex) {
        std::fprintf(stderr, "DeepCwEngine: inference error: %s\n", ex.what());
        return {};
    }
#else
    (void)audio; (void)sampleRateHz;
    return {};
#endif
}

} // namespace AetherSDR
