#pragma once

#include <QWidget>

class QLabel;
class QTextEdit;

namespace AetherSDR {

// A lightweight comparison applet for the AppletPanel that shows the DeepCW
// (neural) decoder's output, so it can run side-by-side with the DSP (ggmorse)
// decoder in the panadapter's CW panel (RFC #4333 CW follow-up). Fed by a
// dedicated DeepCW-pinned CwDecoder owned by MainWindow; decoded text is
// confidence-colored the same way the CW panel colors ggmorse output.
//
// Intended as a temporary A/B evaluation surface: pop it out, line it up under
// the panadapter's DSP CW panel, and compare the two engines on live signals.
class CwNeuralApplet : public QWidget {
    Q_OBJECT

public:
    explicit CwNeuralApplet(QWidget* parent = nullptr);

public slots:
    // Append decoded text; cost = 1 - confidence, colored like the CW panel.
    void appendText(const QString& text, float cost);
    // Pitch readout (dominant tone); speed is ignored — a CTC model has none.
    void setStats(float pitchHz, float speedWpm);
    // Short status string shown at the right of the header (e.g. "downloading…",
    // "running", "no model").
    void setStatus(const QString& status);
    void clearText();

private:
    QLabel*    m_pitchLabel{nullptr};
    QLabel*    m_statusLabel{nullptr};
    QTextEdit* m_text{nullptr};
};

} // namespace AetherSDR
