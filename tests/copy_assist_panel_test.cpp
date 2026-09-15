// Offline UI test for CopyAssistPanel (RFC #4333, Phase 5). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies the confidence color-coding (the
// ggmorse-style feature), tier selection, the enable signal, and clear.

#include "gui/CopyAssistPanel.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextEdit>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString transcriptHtml(CopyAssistPanel& panel)
{
    // The transcript is the only QTextEdit in the panel.
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toHtml() : QString();
}

QString transcriptText(CopyAssistPanel& panel)
{
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toPlainText() : QString();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    CopyAssistPanel panel;

    // ---- Confidence color-coding (green=high … red=low) -------------------
    panel.appendText(QStringLiteral("high"), 0.95f);
    panel.appendText(QStringLiteral("medium"), 0.72f);
    panel.appendText(QStringLiteral("low"), 0.50f);
    panel.appendText(QStringLiteral("verylow"), 0.10f);

    const QString html = transcriptHtml(panel).toLower();
    expect(html.contains("high") && html.contains("#00ff88"), "high confidence -> green");
    expect(html.contains("medium") && html.contains("#e0e040"), "medium confidence -> yellow");
    expect(html.contains("low") && html.contains("#ff9020"), "low confidence -> orange");
    expect(html.contains("verylow") && html.contains("#ff4040"), "very low confidence -> red");

    // ---- Empty/whitespace text is ignored ---------------------------------
    const QString before = transcriptText(panel);
    panel.appendText(QStringLiteral("   "), 0.9f);
    expect(transcriptText(panel) == before, "blank utterance is not appended");

    // ---- Clear -------------------------------------------------------------
    panel.clearText();
    expect(transcriptText(panel).trimmed().isEmpty(), "clearText empties the transcript");

    // ---- Settings (⚙) button emits its request ----------------------------
    // (Model/GPU selection now lives in CopyAssistSettingsDialog — see
    // copy_assist_settings_dialog_test.)
    QSignalSpy settingsSpy(&panel, &CopyAssistPanel::settingsRequested);
    panel.settingsButton()->click();
    expect(!settingsSpy.isEmpty(), "settingsRequested emitted on ⚙ click");

    // ---- Enable toggle emits + reflects state -----------------------------
    QSignalSpy enableSpy(&panel, &CopyAssistPanel::enableToggled);
    panel.setAsrEnabled(true);
    expect(panel.isAsrEnabled(), "setAsrEnabled reflects state");
    expect(!enableSpy.isEmpty() && enableSpy.last().at(0).toBool(),
           "enableToggled(true) emitted");

    // ---- Tuning sliders: set + emit --------------------------------------
    {
        QSignalSpy bufSpy(&panel, &CopyAssistPanel::bufferMsChanged);
        panel.setBufferMs(5000);
        expect(panel.bufferMs() == 5000, "setBufferMs sets 5 s");
        expect(!bufSpy.isEmpty() && bufSpy.last().at(0).toInt() == 5000,
               "bufferMsChanged emits milliseconds");

        QSignalSpy sensSpy(&panel, &CopyAssistPanel::sensitivityChanged);
        panel.setSensitivity(50);
        expect(panel.sensitivity() == 50, "setSensitivity sets percent");
        expect(!sensSpy.isEmpty() && sensSpy.last().at(0).toInt() == 50,
               "sensitivityChanged emits percent");

        QSignalSpy silSpy(&panel, &CopyAssistPanel::silenceMsChanged);
        panel.setSilenceMs(500);
        expect(panel.silenceMs() == 500, "setSilenceMs sets milliseconds");
        expect(!silSpy.isEmpty() && silSpy.last().at(0).toInt() == 500,
               "silenceMsChanged emits milliseconds");

        // Clamping into range.
        panel.setBufferMs(999999);
        expect(panel.bufferMs() == 20000, "buffer clamps to 20 s max");

        panel.setFontPx(20);
        expect(panel.fontPx() == 20, "setFontPx sets transcript font size");
        panel.setFontPx(99);
        expect(panel.fontPx() == 32, "font size clamps to 32 px max");
        panel.setFontPx(1);
        expect(panel.fontPx() == 8, "font size clamps to 8 px min");
    }

    // ---- NR tap-point toggle ----------------------------------------------
    // Operator spec: a header button labelled "NR", immediately LEFT of Context,
    // ON by default (transcribe after NR, the historical behaviour). Position is
    // checked by layout index rather than geometry so it holds offscreen at any
    // width.
    {
        // A fresh panel, so the default is observed before anything touches it.
        CopyAssistPanel fresh;
        expect(fresh.isNrChecked(), "NR is on by default (post-DSP, as before)");

        QPushButton* nr = panel.nrButton();
        QPushButton* context = panel.contextCarryButton();
        expect(nr != nullptr && nr->isCheckable(), "NR is a checkable button");
        expect(nr != nullptr && nr->text() == QStringLiteral("NR"), "the button is labelled \"NR\"");

        bool adjacent = false;
        for (QHBoxLayout* row : panel.findChildren<QHBoxLayout*>()) {
            const int p = row->indexOf(nr);
            if (p >= 0) {
                adjacent = row->indexOf(context) == p + 1;
            }
        }
        expect(adjacent, "NR sits immediately left of Context in the header row");

        // Seeding from the store must not echo back as an operator toggle.
        QSignalSpy nrSpy(&panel, &CopyAssistPanel::nrToggled);
        panel.setNrChecked(false);
        expect(!panel.isNrChecked(), "setNrChecked reflects state");
        expect(nrSpy.isEmpty(), "a programmatic NR set does not emit");

        nr->click(); // a real operator toggle
        expect(panel.isNrChecked(), "clicking NR toggles it");
        expect(!nrSpy.isEmpty() && nrSpy.last().at(0).toBool(),
               "nrToggled carries the operator's new state");

        // Context greys out on backends without context hooks; the tap point
        // is backend-independent and must not go with it.
        panel.setContextCarryAvailable(false);
        expect(nr->isEnabled(), "NR stays enabled when Context is unavailable");
        panel.setContextCarryAvailable(true);
    }

    std::printf(g_failures == 0 ? "\nCopy Assist panel: ALL PASS\n"
                                : "\nCopy Assist panel: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
