// Unit tests for the CWX break-in delay floor (#5945).
//
// FLEX firmware before v4.2.20 leaves receive audio digitally silent for ~70 s
// after a CWX send when the radio's break-in delay is small — reliably at 0,
// intermittently at 1-6, not reproducible at 7 or above. AetherSDR never sent
// `cwx delay` at all unless the operator touched the Delay box, so a radio
// sitting at 0 stayed there. The floor raises it on the operator's own send.
//
// Two invariants carry the design and are what these tests exist to break:
//  1. The status-driven raise is BOUNDED: it fires only when the reported
//     value changes into a below-floor one, so a radio that refuses the
//     command cannot make it repeat.
//  2. The floor is ABSOLUTE by maintainer decision (K5PTB, #5945): the UI
//     cannot request a value below it, and a radio reporting one is corrected
//     as soon as it says so — a deliberate, documented deviation from
//     Principle II, because a radio left deaf for ~70 s per send is worse.

#include "models/CwxModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Every command the model emits, in order, by either route.
class Recorder {
public:
    explicit Recorder(CwxModel& model)
    {
        QObject::connect(&model, &CwxModel::commandReady,
                         [this](const QString& c) { m_commands << c; });
        QObject::connect(&model, &CwxModel::replyCommandReady,
                         [this](const QString& c, int, int) { m_commands << c; });
    }

    // Only the two verbs these tests reason about, so unrelated traffic
    // (macro saves, wpm restores) cannot mask an ordering mistake.
    QStringList relevant() const
    {
        QStringList out;
        for (const QString& c : m_commands) {
            if (c.startsWith(QStringLiteral("cwx delay"))
                || c.startsWith(QStringLiteral("cwx send"))) {
                out << c.left(c.indexOf(QChar('"')) > 0 ? c.indexOf(QChar('"')) - 1
                                                        : c.size());
            }
        }
        return out;
    }

    int count(const QString& prefix) const
    {
        int n = 0;
        for (const QString& c : m_commands) {
            if (c.startsWith(prefix)) ++n;
        }
        return n;
    }

    void clear() { m_commands.clear(); }

private:
    QStringList m_commands;
};

// Radio truth: what the radio says its break-in delay is.
void radioReportsDelay(CwxModel& model, int ms)
{
    QMap<QString, QString> kvs;
    kvs.insert(QStringLiteral("break_in_delay"), QString::number(ms));
    model.applyStatus(kvs);
}

std::string joined(const QStringList& list)
{
    return ("[" + list.join(QStringLiteral(" | ")) + "]").toStdString();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("CWX break-in delay floor tests (#5945)\n\n");

    const QString kRaise = QStringLiteral("cwx delay 10");

    // ── The reported bug: a radio left at 0 is raised as soon as it says so ──
    {
        CwxModel model;
        Recorder rec(model);
        radioReportsDelay(model, 0);
        const QStringList got = rec.relevant();
        report("delay 0 -> floor command is emitted",
               got == QStringList{kRaise}, joined(got));
        // And it lands BEFORE any send can go out at the bad value.
        rec.clear();
        model.send(QStringLiteral("K5PTB"));
        report("the send that follows carries no further delay command",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }

    // ── Never act on the client's own default ───────────────────────────────
    {
        // No status has arrived, so m_delay is just this client's default.
        // Raising on it could LOWER a radio another client set to 41.
        CwxModel model;
        Recorder rec(model);
        model.send(QStringLiteral("K5PTB"));
        report("no radio status yet -> no floor command",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }

    // ── The raise is a request, not a local write ────────────────────────────
    {
        // Principle II: m_delay stays radio truth. If the client adopted its own
        // 10 here, a radio that refused or clamped the value would leave the
        // model (and the Delay box) showing something the radio never holds.
        CwxModel model;
        radioReportsDelay(model, 0);
        model.send(QStringLiteral("K5PTB"));
        report("model still reports the radio's value after raising",
               model.delay() == 0, std::to_string(model.delay()));
        radioReportsDelay(model, 10);            // the echo is what moves it
        report("the radio's echo is what updates the model",
               model.delay() == 10, std::to_string(model.delay()));
    }

    // ── Values at or above the floor are left alone ──────────────────────────
    {
        CwxModel model;
        radioReportsDelay(model, 41);        // e.g. SmartSDR's value
        Recorder rec(model);
        model.send(QStringLiteral("K5PTB"));
        report("delay 41 -> no floor command (another client's value survives)",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }
    {
        CwxModel model;
        radioReportsDelay(model, CwxModel::kMinBreakInDelayMs);
        Recorder rec(model);
        model.send(QStringLiteral("K5PTB"));
        report("delay exactly at the floor -> no command (boundary)",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }

    // ── Status above the floor still emits nothing ──────────────────────────
    {
        CwxModel model;
        Recorder rec(model);
        radioReportsDelay(model, 41);
        report("healthy radio status emits no command at all",
               rec.count(QStringLiteral("cwx")) == 0, joined(rec.relevant()));
    }

    // ── The floor is absolute: the UI cannot request below it ───────────────
    {
        CwxModel model;
        Recorder rec(model);
        model.setDelay(0);                   // operator types 0
        report("operator input below the floor is clamped to it",
               rec.count(QStringLiteral("cwx delay 10")) == 1
                   && model.delay() == CwxModel::kMinBreakInDelayMs,
               joined(rec.relevant()) + " delay=" + std::to_string(model.delay()));
    }
    {
        // The case the operator actually hit: the model already holds the floor,
        // so the clamp changes nothing — but the widget that asked is showing 4
        // and must be told to snap back.
        CwxModel model;
        model.setDelay(CwxModel::kMinBreakInDelayMs);
        int announced = -1;
        QObject::connect(&model, &CwxModel::delayChanged,
                         [&announced](int ms) { announced = ms; });
        Recorder rec(model);
        model.setDelay(4);
        report("a clamp that changes nothing still announces the floor",
               announced == CwxModel::kMinBreakInDelayMs,
               std::to_string(announced));
        report("and sends no duplicate command",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }
    {
        CwxModel model;
        Recorder rec(model);
        model.setDelay(41);                  // a legitimate larger value passes
        report("operator input above the floor is untouched",
               model.delay() == 41, std::to_string(model.delay()));
    }

    // ── The radio reporting a low value is corrected there and then ─────────
    {
        // What the operator actually hits: connect, radio says 0, and the fix
        // must not wait for the first send to happen.
        CwxModel model;
        Recorder rec(model);
        radioReportsDelay(model, 0);
        report("radio reporting 0 is raised immediately, without a send",
               rec.relevant() == QStringList{kRaise}, joined(rec.relevant()));
    }
    {
        // A radio that REFUSES the raise keeps reporting the same value. That
        // is not a change, so nothing re-fires: this is what bounds the
        // status->command path.
        CwxModel model;
        radioReportsDelay(model, 0);
        Recorder rec(model);
        radioReportsDelay(model, 0);
        radioReportsDelay(model, 0);
        report("a refused raise does not repeat on unchanged status",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }
    {
        CwxModel model;
        radioReportsDelay(model, 0);
        Recorder rec(model);
        radioReportsDelay(model, CwxModel::kMinBreakInDelayMs);   // radio adopted it
        report("the radio adopting the floor ends it",
               rec.count(QStringLiteral("cwx delay")) == 0, joined(rec.relevant()));
    }

    // ── A fresh low value gets a fresh ask ───────────────────────────────────
    {
        // The radio adopted the floor, then something (another client, a
        // profile load) put it back down. That is a new value, so it earns one
        // new ask rather than being ignored as already-handled.
        CwxModel model;
        radioReportsDelay(model, 0);
        radioReportsDelay(model, CwxModel::kMinBreakInDelayMs);
        Recorder rec(model);
        radioReportsDelay(model, 3);
        report("a new below-floor value is asked about again",
               rec.count(QStringLiteral("cwx delay")) == 1, joined(rec.relevant()));
    }

    // ── A radio that refuses must not draw a command per keystroke ──────────
    {
        // Live mode sends one `cwx send` per character. If the radio refuses
        // the raise, m_delay stays low, and an unguarded backstop would append
        // a `cwx delay` to every keystroke.
        CwxModel model;
        radioReportsDelay(model, 0);          // asks once, radio refuses
        Recorder rec(model);
        for (const QString& ch : {QStringLiteral("K"), QStringLiteral("5"),
                                  QStringLiteral("P"), QStringLiteral("T"),
                                  QStringLiteral("B")}) {
            model.sendChar(ch);
        }
        report("live typing does not re-ask the radio per character",
               rec.count(QStringLiteral("cwx delay")) <= 1,
               joined(rec.relevant()));
    }

    // ── The floor is the value two reporters found safe, with margin ─────────
    report("floor is 10 ms (above the highest value seen to fault: 6)",
           CwxModel::kMinBreakInDelayMs == 10,
           std::to_string(CwxModel::kMinBreakInDelayMs));

    std::printf("\n%s\n", g_failed == 0 ? "all passed" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
