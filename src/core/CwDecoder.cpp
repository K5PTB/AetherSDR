#include "CwDecoder.h"
#include "LogManager.h"
#include "DeepCwEngine.h"
#include "Resampler.h"
#include "ggmorse/ggmorse.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace AetherSDR {

CwDecoder::CwDecoder(QObject* parent)
    : QObject(parent)
{}

CwDecoder::~CwDecoder()
{
    stop();
}

bool CwDecoder::loadDeepCwModel(const QString& modelPath)
{
    if (!m_deepcw)
        m_deepcw = std::make_unique<DeepCwEngine>();
    const bool ok = m_deepcw->loadModel(modelPath.toStdString());
    m_deepLoaded = ok;
    qCInfo(lcDsp) << "CwDecoder: DeepCW model load" << (ok ? "ok" : "FAILED")
                  << modelPath;
    return ok;
}

void CwDecoder::start()
{
    if (m_running) return;

    const bool deep = (backend() == Backend::DeepCw);

    if (!deep) {
        // Create ggmorse instance for 24kHz mono int16 input
        GGMorse::Parameters params;
        params.sampleRateInp = 24000.0f;
        params.sampleRateOut = 24000.0f;
        params.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
        params.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_I16;
        params.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_I16;

        m_ggmorse = std::make_unique<GGMorse>(params);

        // Auto-detect pitch and speed
        GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
        dp.frequency_hz = -1;  // auto
        dp.speed_wpm = -1;     // auto
        m_ggmorse->setParametersDecode(dp);
    }

    m_running = true;

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.clear();
    }

    // Run decode loop on worker thread (CwDecoder stays on main thread)
    auto* worker = QThread::create([this, deep]() {
        if (deep) decodeLoopDeep();
        else      decodeLoop();
    });
    worker->setObjectName("CwDecoder");
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    m_workerThread = worker;
    worker->start();

    qCDebug(lcDsp) << "CwDecoder: started, backend:" << (deep ? "DeepCW" : "ggmorse");
}

void CwDecoder::stop()
{
    if (!m_running) return;
    m_running = false;

    if (m_workerThread) {
        m_workerThread->wait(2000);
        m_workerThread = nullptr;
    }

    m_ggmorse.reset();
    qCDebug(lcDsp) << "CwDecoder: stopped";
}

// Build and apply current ggmorse decode parameters from all stored state.
void CwDecoder::applyDecodeParameters()
{
    if (!m_ggmorse) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz          = m_pitchLocked ? m_pitch.load() : -1.0f;
    dp.speed_wpm             = m_speedLocked ? m_speed.load() : -1.0f;
    dp.frequencyRangeMin_hz  = m_pitchRangeMin;
    dp.frequencyRangeMax_hz  = m_pitchRangeMax;
    dp.speedRangeMin_wpm     = m_speedRangeMin;
    dp.speedRangeMax_wpm     = m_speedRangeMax;
    m_ggmorse->setParametersDecode(dp);
}

void CwDecoder::lockPitch(bool lock)
{
    m_pitchLocked = lock;
    applyDecodeParameters();
    qCDebug(lcDsp) << "CwDecoder: pitch" << (lock ? "locked at" : "unlocked from")
                   << m_pitch.load() << "Hz";
}

void CwDecoder::lockSpeed(bool lock)
{
    m_speedLocked = lock;
    applyDecodeParameters();
    qCDebug(lcDsp) << "CwDecoder: speed" << (lock ? "locked at" : "unlocked from")
                   << m_speed.load() << "WPM";
}

void CwDecoder::setKnownParameters(float pitchHz, float speedWpm)
{
    if (pitchHz <= 0.0f || speedWpm <= 0.0f) return;

    const bool unchanged = qFuzzyCompare(m_pitch.load(), pitchHz)
        && qFuzzyCompare(m_speed.load(), speedWpm)
        && m_pitchLocked && m_speedLocked;
    if (unchanged) return;

    // Lock both pitch and speed to the P/CW applet values.  The local
    // CWX keyer / iambic keyer / etc. all run at the slider WPM, so
    // sidetone is generated at exactly that rate — ggmorse with both
    // values locked gets a reliable unit length and correctly classifies
    // 1u / 3u / 7u gaps so inter-word boundaries become " " separators.
    m_pitch = pitchHz;
    m_speed = speedWpm;
    m_pitchLocked = true;
    m_speedLocked = true;

    // Widen pitch range to comfortably include the known value (default
    // is 500–700 Hz but operators commonly use 700 / 750 / 800).  Also
    // drives ggmorse's internal HPF cutoff.
    constexpr float kPitchRangePad = 150.0f;
    m_pitchRangeMin = std::max(100.0f, pitchHz - kPitchRangePad);
    m_pitchRangeMax = pitchHz + kPitchRangePad;

    applyDecodeParameters();
    qCDebug(lcDsp) << "CwDecoder: known params pitch=" << pitchHz
                   << "Hz speed=" << speedWpm << "WPM";
}

void CwDecoder::setPitchRange(int minHz, int maxHz)
{
    m_pitchRangeMin = static_cast<float>(minHz);
    m_pitchRangeMax = static_cast<float>(maxHz);
    applyDecodeParameters();
    qCDebug(lcDsp) << "CwDecoder: pitch range" << minHz << "-" << maxHz << "Hz";
}

void CwDecoder::setSpeedRange(int minWpm, int maxWpm)
{
    m_speedRangeMin = static_cast<float>(minWpm);
    m_speedRangeMax = static_cast<float>(maxWpm);
    applyDecodeParameters();
    qCDebug(lcDsp) << "CwDecoder: speed range" << minWpm << "-" << maxWpm << "WPM";
}

void CwDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    if (!m_running) return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoSamples = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // Downmix stereo → mono once. ggmorse needs int16; the neural path keeps
    // float32 so the 3200 Hz resample (worker thread) sees no quantization.
    QByteArray mono;
    if (backend() == Backend::DeepCw) {
        mono.resize(stereoSamples * static_cast<int>(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(mono.data());
        for (int i = 0; i < stereoSamples; ++i)
            dst[i] = (src[2 * i] + src[2 * i + 1]) * 0.5f;
    } else {
        mono.resize(stereoSamples * static_cast<int>(sizeof(int16_t)));
        auto* dst = reinterpret_cast<int16_t*>(mono.data());
        for (int i = 0; i < stereoSamples; ++i) {
            float avg = (src[2 * i] + src[2 * i + 1]) * 0.5f;
            dst[i] = static_cast<int16_t>(std::clamp(avg * 32768.0f, -32768.0f, 32767.0f));
        }
    }

    QMutexLocker lock(&m_bufMutex);
    m_ringBuf.append(mono);

    // Trim to capacity (drop oldest, keeping sample alignment for both formats)
    if (m_ringBuf.size() > RING_CAPACITY) {
        m_ringBuf.remove(0, m_ringBuf.size() - RING_CAPACITY);
    }
}

void CwDecoder::decodeLoop()
{
    // ggmorse requests samplesPerFrame * resampleFactor * sampleSize bytes per callback.
    // At 24kHz int16, factor=6 (24000/4000), frame=128: 128*6*2 = 1536 bytes.
    const int resampleFactor = static_cast<int>(m_ggmorse->getSampleRateInp() / GGMorse::kBaseSampleRate);
    const int bytesPerFrame = m_ggmorse->getSamplesPerFrame() * resampleFactor * m_ggmorse->getSampleSizeBytesInp();
    int feedCount = 0;

    qCDebug(lcDsp) << "CwDecoder: decode loop running, bytesPerFrame:" << bytesPerFrame;

    while (m_running) {
        // Wait until we have at least one frame of data
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_ringBuf.size() < bytesPerFrame) {
                lock.unlock();
                QThread::msleep(20);
                continue;
            }
        }

        int framesThisCall = 0;

        bool gotData = m_ggmorse->decode([this, &framesThisCall](void* data, uint32_t nMaxBytes) -> uint32_t {
            if (!m_running) return 0;

            QMutexLocker lock(&m_bufMutex);
            // ggmorse requires exactly nMaxBytes — partial returns cause it to abort
            if (static_cast<uint32_t>(m_ringBuf.size()) < nMaxBytes) return 0;

            std::memcpy(data, m_ringBuf.constData(), nMaxBytes);
            m_ringBuf.remove(0, nMaxBytes);
            ++framesThisCall;
            return nMaxBytes;
        });

        feedCount += framesThisCall;

        // Log periodically
        if (feedCount % 200 == 0 && feedCount > 0) {
            const auto& stats = m_ggmorse->getStatistics();
            const auto& rxData = m_ggmorse->getRxData();
            qCDebug(lcDsp) << "CwDecoder:" << feedCount << "frames fed, pitch:"
                     << stats.estimatedPitch_Hz << "Hz, speed:"
                     << stats.estimatedSpeed_wpm << "WPM, decode:" << gotData
                     << "rxLen:" << rxData.size()
                     << "lastResult:" << m_ggmorse->lastDecodeResult();
        }

        const auto& stats = m_ggmorse->getStatistics();

        // Accept all decodes — color-coded by confidence in the UI
        GGMorse::TxRx rxData;
        if (m_ggmorse->takeRxData(rxData) > 0 && stats.costFunction < 1.0f) {
            QString text = QString::fromLatin1(
                reinterpret_cast<const char*>(rxData.data()),
                static_cast<int>(rxData.size()));
            emit textDecoded(text, stats.costFunction);
        }

        if (stats.estimatedPitch_Hz > 0) {
            m_pitch = stats.estimatedPitch_Hz;
            m_speed = stats.estimatedSpeed_wpm;
            emit statsUpdated(m_pitch, m_speed);
        }
    }

    qCDebug(lcDsp) << "CwDecoder: decode loop exiting, total frames:" << feedCount;
}

// DeepCW (neural) worker loop. The model is a whole-window CTC decoder trained
// on 5-20 s clips, so we accumulate a rolling audio segment and re-decode it as
// it grows, emitting only the newly-decoded suffix (the decode of a longer clip
// is normally a prefix-extension of the shorter one). Near the model's 20 s cap
// we finalize the segment and start fresh so inference stays in-distribution.
//
// feedAudio() has downmixed the RX audio to mono float32 @24 kHz into m_ringBuf;
// here we drain it, resample to the model's 3200 Hz with an anti-aliased r8brain
// SRC (a 7.5x decimation — a naive drop/linear resample would fold energy into
// the 400-1200 Hz analysis band), and grow a rolling 3200 Hz segment that we
// re-decode as it lengthens. The SRC stays continuous across segment resets
// (the audio stream is continuous even though the analysis window restarts).
// Prototype heuristics — a later revision can add overlap-merge and silence
// segmentation.
void CwDecoder::decodeLoopDeep()
{
    constexpr int   kRate        = DeepCwEngine::kModelSampleRate;  // 3200 Hz (post-resample)
    const size_t    kMinDecode   = kRate * 5;    // model floor: 5 s
    const size_t    kHopSamples  = kRate * 2;    // re-decode every ~2 s of new audio
    const size_t    kMaxSamples  = kRate * 15;   // finalize before the 20 s cap (headroom)

    // Anti-aliased 24k -> 3200 Hz SRC (r8brain via the in-tree wrapper). Owned by
    // and used only on this worker thread, so its non-thread-safety is moot.
    Resampler resampler(24000.0, static_cast<double>(kRate));

    std::vector<float> seg;               // accumulated audio at 3200 Hz
    seg.reserve(kMaxSamples + kRate);
    std::string emitted;                  // text already emitted for the current segment
    size_t lastDecodeSamples = 0;

    qCDebug(lcDsp) << "CwDecoder: DeepCW loop running, modelLoaded:" << m_deepLoaded.load();

    while (m_running) {
        // Drain the handoff ring (mono float32 @24k) and resample to 3200 Hz.
        std::vector<float> in24k;
        {
            QMutexLocker lock(&m_bufMutex);
            const int n = m_ringBuf.size() / static_cast<int>(sizeof(float));
            if (n > 0) {
                const auto* s = reinterpret_cast<const float*>(m_ringBuf.constData());
                in24k.assign(s, s + n);
                m_ringBuf.clear();
            }
        }
        if (!in24k.empty()) {
            const QByteArray out = resampler.process(in24k.data(), static_cast<int>(in24k.size()));
            const auto* r = reinterpret_cast<const float*>(out.constData());
            const int m = out.size() / static_cast<int>(sizeof(float));
            seg.insert(seg.end(), r, r + m);
        }

        const bool haveMin    = seg.size() >= kMinDecode;
        const bool grewEnough = seg.size() >= lastDecodeSamples + kHopSamples;

        if (m_deepLoaded && m_deepcw && haveMin && grewEnough) {
            float conf = 1.0f;
            float pitchHz = 0.0f;
            const std::string text = m_deepcw->decode(seg, kRate, &conf, &pitchHz);
            lastDecodeSamples = seg.size();
            // Map mean CTC confidence to the panel's cost convention (lower =
            // better) so the Sensitivity slider filters shaky neural decodes.
            const float cost = 1.0f - conf;

            // Publish the dominant-tone pitch (no speed estimate for a CTC model)
            // so zero-beat and the pitch readout work in neural mode too.
            if (pitchHz > 0.0f) {
                m_pitch = pitchHz;
                emit statsUpdated(pitchHz, 0.0f);
            }

            // Emit the suffix beyond what we've shown when the new decode extends
            // the old as a prefix; on a divergent revision, silently adopt the new
            // baseline (a rare correction may drop/duplicate a few chars — accepted
            // for the prototype).
            if (text.size() >= emitted.size()
                && text.compare(0, emitted.size(), emitted) == 0) {
                const std::string delta = text.substr(emitted.size());
                if (!delta.empty()) {
                    emit textDecoded(QString::fromStdString(delta), cost);
                    emitted = text;
                }
            } else {
                emitted = text;
            }
        }

        // Finalize near the model's max window and start a fresh segment.
        if (seg.size() >= kMaxSamples) {
            seg.clear();
            emitted.clear();
            lastDecodeSamples = 0;
        }

        QThread::msleep(200);
    }

    qCDebug(lcDsp) << "CwDecoder: DeepCW loop exiting";
}

} // namespace AetherSDR
