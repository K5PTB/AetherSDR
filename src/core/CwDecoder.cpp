#include "CwDecoder.h"
#include "CwPllDecoder.h"
#include "LogManager.h"
#include "ggmorse/ggmorse.h"
#include <cstring>

namespace AetherSDR {

CwDecoder::CwDecoder(QObject* parent)
    : QObject(parent)
{}

CwDecoder::~CwDecoder()
{
    stop();
}

void CwDecoder::start()
{
    if (m_running) return;

    // Create ggmorse instance for 24kHz mono int16 input
    GGMorse::Parameters params;
    params.sampleRateInp = 24000.0f;
    params.sampleRateOut = 24000.0f;
    params.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
    params.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_I16;
    params.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_I16;

    m_ggmorse = std::make_unique<GGMorse>(params);
    applyDecodeParameters();

    // Create PLL decoder with pitch/speed ranges matching current state
    CwPllDecoder::Config pllCfg;
    pllCfg.pitchMin = m_pitchRangeMin;
    pllCfg.pitchMax = m_pitchRangeMax;
    pllCfg.speedMin = (m_speedRangeMin > 0) ? m_speedRangeMin.load() : 5.0f;
    pllCfg.speedMax = (m_speedRangeMax > 0) ? m_speedRangeMax.load() : 60.0f;
    if (m_pitchLocked) pllCfg.pitchHz = m_pitch;
    m_pllDecoder = std::make_unique<CwPllDecoder>(pllCfg);
    if (m_speedLocked) m_pllDecoder->lockSpeed(m_speed);

    m_running = true;

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.clear();
    }
    {
        QMutexLocker lock(&m_pllMutex);
        m_pllBuf.clear();
    }

    // Run decode loop on worker thread (CwDecoder stays on main thread)
    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("CwDecoder");
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    m_workerThread = worker;
    worker->start();

    qCDebug(lcDsp) << "CwDecoder: started";
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
    m_pllDecoder.reset();
    qCDebug(lcDsp) << "CwDecoder: stopped";
}

void CwDecoder::setDecoderMode(CwDecoderMode mode)
{
    m_decoderMode = mode;
    // Reset PLL state on switch so stale timing doesn't confuse the new session
    if (mode == CwDecoderMode::PLL && m_pllDecoder)
        m_pllDecoder->reset();
    qCDebug(lcDsp) << "CwDecoder: mode →" << (mode == CwDecoderMode::PLL ? "PLL" : "ggmorse");
}

// Apply all stored state to both decode engines.
void CwDecoder::applyDecodeParameters()
{
    if (m_ggmorse) {
        GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
        dp.frequency_hz          = m_pitchLocked ? m_pitch.load() : -1.0f;
        dp.speed_wpm             = m_speedLocked ? m_speed.load() : -1.0f;
        dp.frequencyRangeMin_hz  = m_pitchRangeMin;
        dp.frequencyRangeMax_hz  = m_pitchRangeMax;
        dp.speedRangeMin_wpm     = m_speedRangeMin;
        dp.speedRangeMax_wpm     = m_speedRangeMax;
        m_ggmorse->setParametersDecode(dp);
    }
    if (m_pllDecoder) {
        CwPllDecoder::Config cfg;
        cfg.pitchHz  = m_pitchLocked ? m_pitch.load()  : -1.0f;
        cfg.pitchMin = m_pitchRangeMin;
        cfg.pitchMax = m_pitchRangeMax;
        cfg.speedMin = (m_speedRangeMin > 0) ? m_speedRangeMin.load() : 5.0f;
        cfg.speedMax = (m_speedRangeMax > 0) ? m_speedRangeMax.load() : 60.0f;
        m_pllDecoder->setConfig(cfg);
        m_pllDecoder->lockPitch(m_pitchLocked ? m_pitch.load() : -1.0f);
        m_pllDecoder->lockSpeed(m_speedLocked ? m_speed.load() : -1.0f);
    }
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

    // Downmix stereo float32 → mono int16 for ggmorse
    QByteArray monoI16(stereoSamples * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
    auto* dst16 = reinterpret_cast<int16_t*>(monoI16.data());

    // Downmix stereo float32 → mono float32 for PLL
    QByteArray monoF32(stereoSamples * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dstF = reinterpret_cast<float*>(monoF32.data());

    for (int i = 0; i < stereoSamples; ++i) {
        float avg = (src[2 * i] + src[2 * i + 1]) * 0.5f;
        dst16[i] = static_cast<int16_t>(std::clamp(avg * 32768.0f, -32768.0f, 32767.0f));
        dstF[i]  = avg;
    }

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.append(monoI16);
        if (m_ringBuf.size() > RING_CAPACITY)
            m_ringBuf.remove(0, m_ringBuf.size() - RING_CAPACITY);
    }
    {
        QMutexLocker lock(&m_pllMutex);
        m_pllBuf.append(monoF32);
        if (m_pllBuf.size() > PLL_RING_CAPACITY)
            m_pllBuf.remove(0, m_pllBuf.size() - PLL_RING_CAPACITY);
    }
}

void CwDecoder::decodeLoop()
{
    // ggmorse frame size: 128 * 6 * 2 = 1536 bytes of mono int16 at 24kHz
    const int resampleFactor  = static_cast<int>(m_ggmorse->getSampleRateInp() / GGMorse::kBaseSampleRate);
    const int ggmorseFrameBytes = m_ggmorse->getSamplesPerFrame() * resampleFactor
                                  * m_ggmorse->getSampleSizeBytesInp();

    // PLL frame: same 768 samples expressed as float32 = 3072 bytes
    const int pllFrameSamples = m_ggmorse->getSamplesPerFrame() * resampleFactor;  // 768
    const int pllFrameBytes   = pllFrameSamples * static_cast<int>(sizeof(float));

    int feedCount = 0;
    qCDebug(lcDsp) << "CwDecoder: decode loop running";

    while (m_running) {
        const CwDecoderMode mode = m_decoderMode.load();

        if (mode == CwDecoderMode::PLL) {
            // ── PLL path ─────────────────────────────────────────────────────
            {
                QMutexLocker lock(&m_pllMutex);
                if (m_pllBuf.size() < pllFrameBytes) {
                    lock.unlock();
                    QThread::msleep(10);
                    continue;
                }
            }

            QByteArray chunk;
            {
                QMutexLocker lock(&m_pllMutex);
                chunk = m_pllBuf.left(pllFrameBytes);
                m_pllBuf.remove(0, pllFrameBytes);
            }

            const float* samples = reinterpret_cast<const float*>(chunk.constData());
            std::string decoded  = m_pllDecoder->process(samples, pllFrameSamples);

            if (!decoded.empty()) {
                emit textDecoded(QString::fromLatin1(decoded.c_str(),
                                                     static_cast<int>(decoded.size())),
                                 1.0f - m_pllDecoder->confidence());
            }

            float pitch = m_pllDecoder->estimatedPitch();
            float speed = m_pllDecoder->estimatedSpeed();
            if (pitch > 0) {
                m_pitch = pitch;
                m_speed = speed;
                emit statsUpdated(pitch, speed);
            }

            ++feedCount;

        } else {
            // ── ggmorse path ─────────────────────────────────────────────────
            {
                QMutexLocker lock(&m_bufMutex);
                if (m_ringBuf.size() < ggmorseFrameBytes) {
                    lock.unlock();
                    QThread::msleep(20);
                    continue;
                }
            }

            int framesThisCall = 0;
            bool gotData = m_ggmorse->decode([this, ggmorseFrameBytes, &framesThisCall]
                                             (void* data, uint32_t nMaxBytes) -> uint32_t {
                if (!m_running) return 0;
                QMutexLocker lock(&m_bufMutex);
                if (static_cast<uint32_t>(m_ringBuf.size()) < nMaxBytes) return 0;
                std::memcpy(data, m_ringBuf.constData(), nMaxBytes);
                m_ringBuf.remove(0, nMaxBytes);
                ++framesThisCall;
                return nMaxBytes;
            });
            feedCount += framesThisCall;

            if (feedCount % 200 == 0 && feedCount > 0) {
                const auto& stats = m_ggmorse->getStatistics();
                qCDebug(lcDsp) << "CwDecoder(ggmorse):" << feedCount
                         << "frames, pitch:" << stats.estimatedPitch_Hz
                         << "Hz, speed:" << stats.estimatedSpeed_wpm
                         << "WPM, decoded:" << gotData;
            }

            const auto& stats = m_ggmorse->getStatistics();
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
    }

    qCDebug(lcDsp) << "CwDecoder: decode loop exiting, total frames:" << feedCount;
}

} // namespace AetherSDR
