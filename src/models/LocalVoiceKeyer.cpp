#include "LocalVoiceKeyer.h"

#include "core/VoiceKeyerSettings.h"
#include "core/VoiceKeyerWavDecoder.h"

#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace AetherSDR {

namespace {

bool validSlot(int id)
{
    return id >= 1 && id <= LocalVoiceKeyer::kSlotCount;
}

qsizetype maxRecordBytes()
{
    return qsizetype(LocalVoiceKeyerStore::kMaxDurationMs) * LocalVoiceKeyerStore::kSampleRate / 1000
           * LocalVoiceKeyerStore::kChannels * qsizetype(sizeof(qint16));
}

} // namespace

LocalVoiceKeyer::LocalVoiceKeyer(const QString& recordingsDir, QObject* parent)
    : VoiceKeyer(parent)
    , m_store(recordingsDir)
{
    m_recordings.reserve(kSlotCount);
    for (int id = 1; id <= kSlotCount; ++id)
        m_recordings.append({id, QString(), 0});
    reload();
}

void LocalVoiceKeyer::setPreviewHandlers(PreviewStart start, PreviewStop stop)
{
    m_previewStart = std::move(start);
    m_previewStop = std::move(stop);
}

void LocalVoiceKeyer::reload()
{
    for (int id = 1; id <= kSlotCount; ++id)
        refreshSlot(id);
}

void LocalVoiceKeyer::refreshSlot(int id)
{
    auto& rec = m_recordings[id - 1];
    const QString name = VoiceKeyerSettings::slotName(id);
    rec.name = name.isEmpty() ? QStringLiteral("Recording %1").arg(id) : name;
    rec.durationMs = m_store.durationMs(id);
    emit recordingChanged(id);
}

void LocalVoiceKeyer::setStatus(Status status, int id)
{
    if (status == m_status && id == m_activeId)
        return;
    m_status = status;
    m_activeId = id;
    emit statusChanged(m_status, m_activeId);
}

void LocalVoiceKeyer::refuse(const QString& verb, int id, const QString& message)
{
    emit commandFailed(verb, id, 0, message);
}

bool LocalVoiceKeyer::busyRefusal(const QString& verb, int id)
{
    if (m_status == Idle)
        return false;
    refuse(verb, id, QStringLiteral("Stop the current recording, preview or playback first."));
    return true;
}

// ── Recording ───────────────────────────────────────────────────────────────

void LocalVoiceKeyer::recStart(int id)
{
    if (!validSlot(id) || busyRefusal(QStringLiteral("rec_start"), id))
        return;
    const QString source = m_micSource ? m_micSource() : QStringLiteral("PC");
    if (source.compare(QLatin1String("PC"), Qt::CaseInsensitive) != 0) {
        refuse(QStringLiteral("rec_start"), id,
               QStringLiteral("Recording uses the PC microphone, but the mic source is %1 "
                              "— set the mic source to PC to record.")
                   .arg(source.isEmpty() ? QStringLiteral("not set") : source));
        return;
    }
    m_recordBuffer.clear();
    m_recordCapped = false;
    setStatus(Recording, id);
}

void LocalVoiceKeyer::onMicPcm(const QByteArray& int16Stereo, TxAudioSource source)
{
    // Positively the microphone, rather than "not a client": a recording slot
    // holds the operator's voice, and every other origin on this tap belongs to
    // something else that happened to be transmitting.
    if (m_status != Recording || source != TxAudioSource::Microphone)
        return;
    const qsizetype room = maxRecordBytes() - m_recordBuffer.size();
    if (room > 0)
        m_recordBuffer.append(int16Stereo.constData(), std::min(room, int16Stereo.size()));
    if (m_recordBuffer.size() >= maxRecordBytes()) {
        m_recordCapped = true;
        recStop(m_activeId);
    }
}

void LocalVoiceKeyer::recStop(int id)
{
    Q_UNUSED(id);  // the slot being recorded is the one that stops
    if (m_status != Recording)
        return;
    const int slot = m_activeId;
    const QByteArray pcm = std::exchange(m_recordBuffer, QByteArray());
    setStatus(Idle, -1);

    if (pcm.isEmpty()) {
        refuse(QStringLiteral("rec_stop"), slot,
               QStringLiteral("No microphone audio arrived — check that the PC microphone "
                              "is capturing."));
        return;
    }
    QString error;
    if (!m_store.writeSlot(slot, pcm, error)) {
        refuse(QStringLiteral("rec_stop"), slot, error);
        return;
    }
    refreshSlot(slot);
    if (m_recordCapped) {
        emit transferStatusChanged(
            QStringLiteral("Recording %1 stopped at the %2 s limit")
                .arg(slot).arg(LocalVoiceKeyerStore::kMaxDurationMs / 1000));
    }
}

// ── Preview (local speakers) ────────────────────────────────────────────────

void LocalVoiceKeyer::previewStart(int id)
{
    if (!validSlot(id) || busyRefusal(QStringLiteral("preview_start"), id))
        return;
    if (m_store.durationMs(id) <= 0) {
        refuse(QStringLiteral("preview_start"), id,
               QStringLiteral("Slot %1 has no recording.").arg(id));
        return;
    }
    if (!m_previewStart) {
        refuse(QStringLiteral("preview_start"), id, QStringLiteral("Preview is not available."));
        return;
    }
    QString error;
    if (!m_previewStart(m_store.slotPath(id), error)) {
        refuse(QStringLiteral("preview_start"), id, error);
        return;
    }
    setStatus(Preview, id);
}

void LocalVoiceKeyer::previewStop(int id)
{
    Q_UNUSED(id);
    if (m_status != Preview)
        return;
    if (m_previewStop)
        m_previewStop();
    setStatus(Idle, -1);
}

void LocalVoiceKeyer::onPreviewFinished()
{
    if (m_status == Preview)
        setStatus(Idle, -1);
}

// ── On-air playback ─────────────────────────────────────────────────────────

void LocalVoiceKeyer::setTransmitter(GeneratedAudioTransmitter* transmitter)
{
    if (m_transmitterFinished)
        disconnect(m_transmitterFinished);
    m_transmitter = transmitter;
    if (transmitter)
        m_transmitterFinished = connect(transmitter, &GeneratedAudioTransmitter::finished,
                                        this, &LocalVoiceKeyer::onTransmitFinished);
}

void LocalVoiceKeyer::playbackStart(int id)
{
    const QString verb = QStringLiteral("playback_start");
    if (!validSlot(id) || busyRefusal(verb, id))
        return;
    if (m_store.durationMs(id) <= 0) {
        refuse(verb, id, QStringLiteral("Slot %1 has no recording.").arg(id));
        return;
    }
    if (!m_transmitter) {
        refuse(verb, id, QStringLiteral("On-air playback is not available."));
        return;
    }
    // Read fresh each time: the folder is the operator's, and a WAV dropped
    // in since the last play is what they expect to hear on the air.
    QByteArray mono;
    int rate = 0;
    QString error;
    if (!VoiceKeyerWavDecoder::decodeToMonoFloat(m_store.slotPath(id), mono, rate, error)) {
        refuse(verb, id, error);
        return;
    }
    if (!m_transmitter->start(VoiceKeyerWavDecoder::toTxStereo24k(mono, rate), error)) {
        refuse(verb, id, error);
        return;
    }
    setStatus(Playback, id);
}

void LocalVoiceKeyer::playbackStop(int id)
{
    Q_UNUSED(id);
    if (m_status == Playback && m_transmitter)
        m_transmitter->stop();   // finished() returns the keyer to Idle
}

void LocalVoiceKeyer::onTransmitFinished(GeneratedAudioTransmitter::Outcome outcome,
                                         const QString& reason)
{
    if (m_status != Playback)
        return;   // the transmitter is shared; this one was not ours
    const int slot = m_activeId;
    setStatus(Idle, -1);
    if (outcome == GeneratedAudioTransmitter::Outcome::Failed)
        refuse(QStringLiteral("playback"), slot, reason);
}

// ── Slot management ─────────────────────────────────────────────────────────

void LocalVoiceKeyer::clear(int id)
{
    if (!validSlot(id) || busyRefusal(QStringLiteral("clear"), id))
        return;
    QString error;
    if (!m_store.removeSlot(id, error))
        refuse(QStringLiteral("clear"), id, error);
    refreshSlot(id);
}

void LocalVoiceKeyer::remove(int id)
{
    if (!validSlot(id) || busyRefusal(QStringLiteral("remove"), id))
        return;
    QString error;
    if (!m_store.removeSlot(id, error))
        refuse(QStringLiteral("remove"), id, error);
    VoiceKeyerSettings::setSlotName(id, QString());
    refreshSlot(id);
}

void LocalVoiceKeyer::setName(int id, const QString& name)
{
    if (!validSlot(id))
        return;
    VoiceKeyerSettings::setSlotName(id, name.trimmed());
    refreshSlot(id);
}

void LocalVoiceKeyer::importWav(int id, const QString& path)
{
    if (!validSlot(id))
        return;
    if (m_status != Idle) {
        emit transferFinished(false, QStringLiteral("Stop the current recording or preview first."));
        return;
    }
    QString error;
    const bool ok = m_store.importSlot(id, path, error);
    refreshSlot(id);
    emit transferFinished(ok, ok ? QStringLiteral("Imported %1 into slot %2")
                                       .arg(QFileInfo(path).fileName()).arg(id)
                                 : error);
}

void LocalVoiceKeyer::exportWav(int id, const QString& path)
{
    if (!validSlot(id))
        return;
    QString error;
    const bool ok = m_store.exportSlot(id, path, error);
    emit transferFinished(ok, ok ? QStringLiteral("Exported slot %1 to %2")
                                       .arg(id).arg(QFileInfo(path).fileName())
                                 : error);
}

} // namespace AetherSDR
