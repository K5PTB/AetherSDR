// Client-side voice keyer (RFC #4214): recording from the mic tap, preview,
// slot management and WAV import/export — no radio, no audio device.
// Run: ./build/local_voice_keyer_test

#include "FakeTxAudioRoute.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LocalVoiceKeyerStore.h"
#include "models/LocalVoiceKeyer.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;
constexpr double kPi = 3.14159265358979323846;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Records what the keyer told the panel.
struct Listener {
    QStringList failures;
    QStringList transfers;
    QList<int> changed;
    explicit Listener(VoiceKeyer* k)
    {
        QObject::connect(k, &VoiceKeyer::commandFailed,
                         [this](const QString& verb, int id, uint, const QString& msg) {
                             failures << QString("%1 %2: %3").arg(verb).arg(id).arg(msg);
                         });
        QObject::connect(k, &VoiceKeyer::transferFinished,
                         [this](bool ok, const QString& msg) {
                             transfers << QString("%1 %2").arg(ok ? "ok" : "fail", msg);
                         });
        QObject::connect(k, &VoiceKeyer::transferStatusChanged,
                         [this](const QString& msg) { transfers << QString("status %1").arg(msg); });
        QObject::connect(k, &VoiceKeyer::recordingChanged, [this](int id) { changed << id; });
    }
};

// frames of 24 kHz stereo int16, a steady tone so the file is not silent.
QByteArray micBlock(int frames, int startFrame = 0)
{
    QByteArray pcm(frames * 4, Qt::Uninitialized);
    auto* s = reinterpret_cast<qint16*>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<qint16>(8000 * std::sin(2 * kPi * 700.0 * (startFrame + i) / 24000.0));
        s[2 * i] = v;
        s[2 * i + 1] = v;
    }
    return pcm;
}

// A 48 kHz mono 16-bit WAV of `ms` milliseconds.
bool writeMonoWav48k(const QString& path, int ms)
{
    const int frames = 48000 * ms / 1000;
    QByteArray data(frames * 2, '\0');
    auto* s = reinterpret_cast<qint16*>(data.data());
    for (int i = 0; i < frames; ++i)
        s[i] = static_cast<qint16>(6000 * std::sin(2 * kPi * 440.0 * i / 48000.0));
    QByteArray h(44, '\0');
    char* p = h.data();
    std::memcpy(p, "RIFF", 4);
    qToLittleEndian<quint32>(36 + data.size(), p + 4);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, p + 16);
    qToLittleEndian<quint16>(1, p + 20);
    qToLittleEndian<quint16>(1, p + 22);
    qToLittleEndian<quint32>(48000, p + 24);
    qToLittleEndian<quint32>(96000, p + 28);
    qToLittleEndian<quint16>(2, p + 32);
    qToLittleEndian<quint16>(16, p + 34);
    std::memcpy(p + 36, "data", 4);
    qToLittleEndian<quint32>(data.size(), p + 40);
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(h + data) == h.size() + data.size();
}

QByteArray wavData(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().mid(44) : QByteArray();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("local_voice_keyer_test"));
    QCoreApplication app(argc, argv);
    // A real load, so label changes are saved to the (sandboxed) settings store
    // instead of being refused and only ever living in memory.
    AppSettings::instance().load();
    QTemporaryDir dir;
    const QString recDir = dir.path() + QStringLiteral("/VoiceKeyer");

    LocalVoiceKeyer keyer(recDir);
    Listener heard(&keyer);
    QString micSource = QStringLiteral("PC");
    keyer.setMicSourceProbe([&] { return micSource; });

    // A fresh folder: twelve empty, default-named slots.
    {
        const auto& recs = keyer.recordings();
        bool ok = recs.size() == 12;
        for (int i = 0; ok && i < recs.size(); ++i)
            ok = recs[i].id == i + 1 && recs[i].durationMs == 0
                 && recs[i].name == QString("Recording %1").arg(i + 1);
        report("fresh_folder_has_twelve_empty_slots", ok);
    }

    // With a radio-side mic source, REC refuses and says why.
    micSource = QStringLiteral("MIC");
    keyer.recStart(3);
    report("record_refused_unless_mic_source_is_pc",
           keyer.status() == VoiceKeyer::Idle && heard.failures.size() == 1
               && heard.failures[0].contains("PC microphone") && heard.failures[0].contains("MIC"),
           heard.failures.join(" | ").toStdString());
    micSource = QStringLiteral("PC");
    heard.failures.clear();

    // Recording keeps only the operator's mic: a TCI/DAX client's blocks and an
    // unattended engine beacon's both share this tap and are both skipped.
    {
        keyer.recStart(3);
        const bool recording = keyer.status() == VoiceKeyer::Recording && keyer.activeId() == 3;
        QByteArray expected;
        for (int b = 0; b < 25; ++b) {       // 25 × 480 frames = 0.5 s
            const QByteArray block = micBlock(480, b * 480);
            keyer.onMicPcm(block, TxAudioSource::Microphone);
            expected += block;
            keyer.onMicPcm(micBlock(480), TxAudioSource::ClientLeveled);  // a TCI app's
            keyer.onMicPcm(micBlock(480), TxAudioSource::EngineGenerated); // a WSPR beacon's
        }
        keyer.recStop(3);
        const QByteArray saved = wavData(LocalVoiceKeyerStore(recDir).slotPath(3));
        report("record_saves_only_mic_audio_to_slot",
               recording && keyer.status() == VoiceKeyer::Idle && saved == expected
                   && keyer.recordings()[2].durationMs == 500 && heard.changed.contains(3)
                   && heard.failures.isEmpty(),
               QString("bytes=%1 expected=%2 ms=%3").arg(saved.size()).arg(expected.size())
                   .arg(keyer.recordings()[2].durationMs).toStdString());
    }

    // Stopping with nothing captured says so instead of saving silence.
    keyer.recStart(4);
    keyer.recStop(4);
    report("record_with_no_audio_is_reported",
           heard.failures.size() == 1 && heard.failures[0].startsWith("rec_stop 4")
               && keyer.recordings()[3].durationMs == 0,
           heard.failures.join(" | ").toStdString());
    heard.failures.clear();

    // A recording that runs past the cap stops itself at the cap.
    {
        heard.transfers.clear();
        keyer.recStart(5);
        for (int b = 0; b < 70 && keyer.status() == VoiceKeyer::Recording; ++b)
            keyer.onMicPcm(micBlock(24000, b * 24000), TxAudioSource::Microphone); // 1 s blocks
        report("record_stops_itself_at_the_cap",
               keyer.status() == VoiceKeyer::Idle
                   && keyer.recordings()[4].durationMs == LocalVoiceKeyerStore::kMaxDurationMs
                   && heard.transfers.size() == 1 && heard.transfers[0].contains("limit"),
               heard.transfers.join(" | ").toStdString());
    }

    // Preview hands the slot file to the player, and ends when the player does.
    {
        QString previewed;
        int stops = 0;
        keyer.setPreviewHandlers(
            [&](const QString& path, QString&) { previewed = path; return true; },
            [&] { ++stops; });
        keyer.previewStart(3);
        const bool started = keyer.status() == VoiceKeyer::Preview && keyer.activeId() == 3
                             && previewed == LocalVoiceKeyerStore(recDir).slotPath(3);
        keyer.recStart(6);   // busy
        const bool busyRefused = heard.failures.size() == 1 && heard.failures[0].startsWith("rec_start 6");
        keyer.onPreviewFinished();
        const bool finished = keyer.status() == VoiceKeyer::Idle && stops == 0;
        keyer.previewStart(3);
        keyer.previewStop(3);
        report("preview_plays_slot_file_and_ends_with_player",
               started && busyRefused && finished && stops == 1 && keyer.status() == VoiceKeyer::Idle);
        heard.failures.clear();
        keyer.previewStart(7);
        report("preview_of_empty_slot_is_refused",
               heard.failures.size() == 1 && heard.failures[0].contains("no recording"),
               heard.failures.join(" | ").toStdString());
        heard.failures.clear();
    }

    // On-air playback with no transmitter wired refuses and says so.
    keyer.playbackStart(3);
    report("playback_without_transmitter_is_refused",
           heard.failures.size() == 1 && heard.failures[0].startsWith("playback_start 3")
               && heard.failures[0].contains("not available") && keyer.status() == VoiceKeyer::Idle,
           heard.failures.join(" | ").toStdString());
    heard.failures.clear();

    // On-air playback sends the slot's own audio through the transmitter.
    {
        const auto waitIdle = [&] {
            QElapsedTimer t;
            t.start();
            while (keyer.status() != VoiceKeyer::Idle && t.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return keyer.status() == VoiceKeyer::Idle;
        };
        GeneratedAudioTiming fast;
        fast.settleMs = 1;
        fast.leadMs = 1;
        fast.chunkMs = 2;
        fast.leadBufferMs = 1000;   // the whole slot in the first tick
        fast.tailMs = 1;
        FakeTxAudioRoute route;
        GeneratedAudioTransmitter tx(&route, nullptr, fast);
        route.tx = &tx;
        keyer.setTransmitter(&tx);

        keyer.playbackStart(3);
        const bool playing = keyer.status() == VoiceKeyer::Playback && keyer.activeId() == 3;
        keyer.recStart(6);
        const bool busyRefused = heard.failures.size() == 1
                                 && heard.failures[0].startsWith("rec_start 6");
        heard.failures.clear();
        const bool idle = waitIdle();
        // 500 ms of 24 kHz stereo float; frame 100 is the recorded tone.
        const bool sizeOk = route.sent.size() == 12000 * 8;
        const auto* f = reinterpret_cast<const float*>(route.sent.constData());
        const float want = float(qint16(8000 * std::sin(2 * kPi * 700.0 * 100 / 24000.0))) / 32768.0f;
        const bool contentOk = sizeOk && std::abs(f[200] - want) < 1e-3f && f[200] == f[201];
        report("playback_transmits_the_slot_and_returns_to_idle",
               playing && busyRefused && idle && contentOk && heard.failures.isEmpty()
                   && route.calls.contains("keyOff"),
               QString("sent=%1 calls=%2").arg(route.sent.size()).arg(route.calls.join(","))
                   .toStdString());

        // The route's refusal reaches the panel as the reason.
        route.refusal = QStringLiteral("The transmit slice is in CW.");
        keyer.playbackStart(3);
        report("playback_refusal_is_reported_with_reason",
               keyer.status() == VoiceKeyer::Idle && heard.failures.size() == 1
                   && heard.failures[0].contains("in CW"),
               heard.failures.join(" | ").toStdString());
        route.refusal.clear();
        heard.failures.clear();

        // A failure after the start (PTT refused) ends playback with the reason.
        route.keyEngages = false;
        keyer.playbackStart(3);
        const bool failedIdle = waitIdle();
        report("playback_failure_after_start_is_reported",
               failedIdle && heard.failures.size() == 1
                   && heard.failures[0].startsWith("playback 3")
                   && heard.failures[0].contains("did not engage"),
               heard.failures.join(" | ").toStdString());
        route.keyEngages = true;
        heard.failures.clear();

        // PLAY while previewing never reaches the transmitter.
        keyer.setPreviewHandlers([](const QString&, QString&) { return true; }, [] {});
        keyer.previewStart(3);
        route.calls.clear();
        keyer.playbackStart(3);
        report("playback_refused_while_previewing",
               keyer.status() == VoiceKeyer::Preview && route.calls.isEmpty() && !tx.isActive()
                   && heard.failures.size() == 1 && heard.failures[0].startsWith("playback_start 3"),
               heard.failures.join(" | ").toStdString());
        keyer.previewStop(3);
        heard.failures.clear();

        // Stop is not a failure.
        keyer.playbackStart(3);
        keyer.playbackStop(3);
        report("playback_stop_returns_to_idle_quietly",
               keyer.status() == VoiceKeyer::Idle && heard.failures.isEmpty() && !tx.isActive());

        // An empty slot never reaches the transmitter.
        route.calls.clear();
        keyer.playbackStart(7);
        report("playback_of_empty_slot_is_refused",
               heard.failures.size() == 1 && heard.failures[0].contains("no recording")
                   && route.calls.isEmpty(),
               heard.failures.join(" | ").toStdString());
        heard.failures.clear();
        keyer.setTransmitter(nullptr);
    }

    // Labels persist in settings and survive a new keyer on the same folder.
    keyer.setName(3, QStringLiteral("  CQ Contest  "));
    {
        LocalVoiceKeyer again(recDir);
        report("rename_persists_across_instances",
               again.recordings()[2].name == QLatin1String("CQ Contest")
                   && again.recordings()[2].durationMs == 500);
    }

    // Clear drops the audio but keeps the label; Delete drops both.
    keyer.setName(5, QStringLiteral("Exchange"));
    keyer.clear(5);
    const bool clearedKeepsName = keyer.recordings()[4].durationMs == 0
                                  && keyer.recordings()[4].name == QLatin1String("Exchange");
    keyer.setName(3, QStringLiteral("CQ Contest"));
    keyer.remove(3);
    report("clear_keeps_label_delete_drops_it",
           clearedKeepsName && keyer.recordings()[2].durationMs == 0
               && keyer.recordings()[2].name == QLatin1String("Recording 3")
               && !QFileInfo::exists(LocalVoiceKeyerStore(recDir).slotPath(3)));

    // Import converts a 48 kHz mono file into the slot; export copies it out.
    {
        heard.transfers.clear();
        const QString src = dir.path() + QStringLiteral("/cq48k.wav");
        const QString dst = dir.path() + QStringLiteral("/exported.wav");
        const bool wrote = writeMonoWav48k(src, 1000);
        keyer.importWav(8, src);
        const int ms = keyer.recordings()[7].durationMs;
        keyer.exportWav(8, dst);
        QFile a(LocalVoiceKeyerStore(recDir).slotPath(8)), b(dst);
        const bool same = a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly)
                          && a.readAll() == b.readAll();
        report("import_converts_and_export_copies",
               wrote && ms >= 995 && ms <= 1005 && same && heard.transfers.size() == 2
                   && heard.transfers[0].startsWith("ok") && heard.transfers[1].startsWith("ok"),
               QString("ms=%1 transfers=%2").arg(ms).arg(heard.transfers.join(" | ")).toStdString());
    }

    // A file that is not a WAV is refused at import, leaving the slot alone.
    {
        heard.transfers.clear();
        const QString junk = dir.path() + QStringLiteral("/not-audio.wav");
        QFile f(junk);
        const bool wroteJunk = f.open(QIODevice::WriteOnly) && f.write(QByteArray(200, 'x')) == 200;
        f.close();
        keyer.importWav(9, junk);
        report("import_rejects_a_non_wav",
               wroteJunk && heard.transfers.size() == 1 && heard.transfers[0].startsWith("fail")
                   && keyer.recordings()[8].durationMs == 0,
               heard.transfers.join(" | ").toStdString());
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
