#include "CwNeuralApplet.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace AetherSDR {

CwNeuralApplet::CwNeuralApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(6);

    auto* title = new QLabel("DeepCW (neural)");
    title->setStyleSheet("QLabel { color:#5fc8ff; font-size:10px; font-weight:bold; background:transparent; }");
    bar->addWidget(title);

    m_pitchLabel = new QLabel;
    m_pitchLabel->setStyleSheet("QLabel { color:#8090a0; font-size:9px; background:transparent; }");
    bar->addWidget(m_pitchLabel);

    bar->addStretch();

    m_statusLabel = new QLabel("no model");
    m_statusLabel->setStyleSheet("QLabel { color:#8090a0; font-size:9px; background:transparent; }");
    bar->addWidget(m_statusLabel);

    auto* clr = new QPushButton("CLR");
    clr->setFixedHeight(16);
    clr->setStyleSheet("QPushButton { background:#1a2a3a; color:#c8d8e8; border:1px solid #203040;"
                       " border-radius:2px; font-size:9px; font-weight:bold; padding:1px 6px; }"
                       "QPushButton:hover { color:#ffffff; background:#2a3a4a; }");
    connect(clr, &QPushButton::clicked, this, &CwNeuralApplet::clearText);
    bar->addWidget(clr);

    v->addLayout(bar);

    m_text = new QTextEdit;
    m_text->setReadOnly(true);
    m_text->setMinimumHeight(70);
    m_text->setStyleSheet("QTextEdit { background:#0a0f14; color:#c8d8e8; border:1px solid #203040;"
                          " border-radius:2px; font-family:monospace; font-size:13px; }");
    v->addWidget(m_text, 1);
}

void CwNeuralApplet::appendText(const QString& text, float cost)
{
    QString clean = text;
    clean.replace('\n', ' ');
    if (clean.isEmpty()) return;

    // Same confidence palette as the CW panel: lower cost = higher confidence.
    QString color;
    if (cost < 0.15f)      color = "#00ff88";
    else if (cost < 0.35f) color = "#e0e040";
    else if (cost < 0.60f) color = "#ff9020";
    else                   color = "#ff4040";

    m_text->moveCursor(QTextCursor::End);
    m_text->insertHtml(QString("<span style=\"color:%1\">%2</span>")
                           .arg(color, clean.toHtmlEscaped()));
    m_text->moveCursor(QTextCursor::End);
}

void CwNeuralApplet::setStats(float pitchHz, float /*speedWpm*/)
{
    if (pitchHz > 0)
        m_pitchLabel->setText(QString("%1 Hz").arg(pitchHz, 0, 'f', 0));
}

void CwNeuralApplet::setStatus(const QString& status)
{
    if (m_statusLabel) m_statusLabel->setText(status);
}

void CwNeuralApplet::clearText()
{
    m_text->clear();
}

} // namespace AetherSDR
