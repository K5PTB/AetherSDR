#include "DvkPanel.h"
#include "models/VoiceKeyer.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QShortcut>
#include <QPainter>
#include <QFrame>
#include <QMenu>
#include <QMouseEvent>
#include <QFileDialog>
#include <QDir>
#include <QRegularExpression>
#include <QIcon>
#include <QPainter>
#include <QPen>
#include "core/ThemeManager.h"
#include "core/TxKeyingMarker.h"

#include <algorithm>

namespace AetherSDR {

namespace {

// A transmitting antenna for XMIT: a mast on a splayed base with signal arcs
// radiating from its tip. Drawn rather than an emoji so it takes the button's
// own text colour and looks the same on every platform.
QPixmap antennaPixmap(const QColor& color, int size, qreal dpr)
{
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    p.setPen(QPen(color, std::max<qreal>(1.0, s / 12.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF tip(s / 2, s * 0.36);
    const QPointF hip(s / 2, s * 0.66);
    p.drawLine(tip, QPointF(s / 2, s * 0.94));      // mast
    p.drawLine(hip, QPointF(s * 0.32, s * 0.94));   // legs
    p.drawLine(hip, QPointF(s * 0.68, s * 0.94));
    for (int i = 1; i <= 2; ++i) {
        const qreal r = s * 0.15 * i;
        const QRectF box(tip.x() - r, tip.y() - r, 2 * r, 2 * r);
        p.drawArc(box, 135 * 16, 90 * 16);   // left, centred on 9 o'clock
        p.drawArc(box, -45 * 16, 90 * 16);   // right, centred on 3 o'clock
    }
    p.setBrush(color);
    p.drawEllipse(tip, s / 16, s / 16);
    return pm;
}

} // namespace

static const char* kFKeyStyle =
    "QPushButton { background: #1a2a3a; color: #00b4d8; border: 1px solid #203040; "
    "border-radius: 3px; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
    "QPushButton:hover { background: #253545; }"
    "QPushButton:pressed { background: #00b4d8; color: #000; }";

static const char* kNameStyle =
    "QLabel { color: #c8d8e8; font-size: 10px; }";

static const char* kDurStyle =
    "QLabel { color: #6a8090; font-size: 9px; }";

static const char* kBtnStyle =
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #203040; "
    "border-radius: 3px; padding: 4px 8px; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: #253545; }"
    "QPushButton:checked { background: #00b4d8; color: #000; }";

DvkPanel::DvkPanel(VoiceKeyer* keyer, QWidget* parent)
    : QWidget(parent), m_model(keyer)
{
    theme::setContainer(this, QStringLiteral("panel/dvk"));
    auto* outerVbox = new QVBoxLayout(this);
    outerVbox->setContentsMargins(4, 4, 4, 4);
    outerVbox->setSpacing(4);

    // Title
    m_titleLabel = new QLabel;
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_titleLabel, "QLabel { color: {{color.accent}}; font-weight: bold; font-size: 12px; }");
    outerVbox->addWidget(m_titleLabel);

    // Grid of slots — each row gets equal stretch
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);

    for (int i = 0; i < 12; ++i) {
        int id = i + 1;
        grid->setRowStretch(i, 1);

        // Inset container per row: VBox with content row + progress bar
        auto* rowFrame = new QFrame;
        AetherSDR::ThemeManager::instance().applyStyleSheet(rowFrame, "QFrame { background: #0f1520; border: 1px solid {{color.background.1}}; border-radius: 3px; }");
        rowFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* rowVbox = new QVBoxLayout(rowFrame);
        rowVbox->setContentsMargins(3, 2, 3, 1);
        rowVbox->setSpacing(0);

        auto* rowLayout = new QHBoxLayout;
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        auto* fkeyBtn = new QPushButton(QString("F%1").arg(id));
        fkeyBtn->setStyleSheet(kFKeyStyle);
        fkeyBtn->setFixedWidth(34);
        fkeyBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        fkeyBtn->setToolTip(QString("Transmit recording %1 on the air (F%1)").arg(id));
        // Keys the transmitter: the radio's DVK, or AetherSDR itself for Local.
        markTxKeying(fkeyBtn);
        rowLayout->addWidget(fkeyBtn);

        auto* nameLabel = new QLabel(QString("Recording %1").arg(id));
        nameLabel->setStyleSheet("QLabel { color: #505060; font-size: 10px; }");
        rowLayout->addWidget(nameLabel, 1);

        auto* durLabel = new QLabel("Empty");
        durLabel->setStyleSheet(kDurStyle);
        durLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        durLabel->setFixedWidth(40);
        rowLayout->addWidget(durLabel);

        rowVbox->addLayout(rowLayout, 1);

        auto* progressBar = new QProgressBar;
        progressBar->setFixedHeight(3);
        progressBar->setTextVisible(false);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setStyleSheet(
            "QProgressBar { background: transparent; border: none; }"
            "QProgressBar::chunk { background: #33aa33; border-radius: 1px; }");
        progressBar->hide();
        rowVbox->addWidget(progressBar);

        grid->addWidget(rowFrame, i, 0);

        m_rowFrames.append(rowFrame);
        m_fkeyBtns.append(fkeyBtn);
        m_nameLabels.append(nameLabel);
        m_durLabels.append(durLabel);
        m_progressBars.append(progressBar);

        // Right-click context menu on row
        rowFrame->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(rowFrame, &QFrame::customContextMenuRequested, this, [this, id](const QPoint& pos) {
            selectSlot(id);
            showContextMenu(id, m_rowFrames[id - 1]->mapToGlobal(pos));
        });

        // Left-click row to select, double-click name label to rename
        rowFrame->installEventFilter(this);
        rowFrame->setProperty("slotId", id);
        nameLabel->installEventFilter(this);
        nameLabel->setProperty("slotId", id);

        // F-key button click → playback toggle (only if slot has a recording)
        connect(fkeyBtn, &QPushButton::clicked, this, [this, id]() {
            selectSlot(id);
            if (m_model->status() == VoiceKeyer::Playback && m_model->activeId() == id)
                m_model->playbackStop(id);
            else if (durationForSlot(id) > 0)
                m_model->playbackStart(id);
        });
    }

    outerVbox->addLayout(grid, 1);

    // Control buttons: REC | PLAY | STOP | XMIT. PLAY is heard only on this
    // computer; XMIT is the one that puts the recording on the air.
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(3);

    m_recBtn = new QPushButton(QString::fromUtf8("\u25CF REC"));
    m_recBtn->setCheckable(true);
    m_recBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #cc3333; color: #fff; }");
    btnRow->addWidget(m_recBtn);

    m_previewBtn = new QPushButton(QString::fromUtf8("\u25B6 PLAY"));
    m_previewBtn->setCheckable(true);
    m_previewBtn->setToolTip(QStringLiteral("Play the selected recording on this computer \u2014 not transmitted"));
    m_previewBtn->setAccessibleName(QStringLiteral("Play recording on this computer"));
    m_previewBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #3388cc; color: #fff; }");
    btnRow->addWidget(m_previewBtn);

    m_stopBtn = new QPushButton(QString::fromUtf8("\u25A0 STOP"));
    m_stopBtn->setStyleSheet(kBtnStyle);
    btnRow->addWidget(m_stopBtn);

    m_xmitBtn = new QPushButton(QStringLiteral("XMIT"));
    m_xmitBtn->setCheckable(true);
    m_xmitBtn->setToolTip(QStringLiteral("Transmit the selected recording on the air (F1\u2013F12)"));
    m_xmitBtn->setAccessibleName(QStringLiteral("Transmit recording"));
    m_xmitBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #33aa33; color: #fff; }");
    markTxKeying(m_xmitBtn);
    rebuildXmitIcon();
    btnRow->addWidget(m_xmitBtn);

    outerVbox->addLayout(btnRow);

    // Status label
    m_statusLabel = new QLabel("Status: Idle");
    // Refusals ("set the mic source to PC…") are long and must be read, so the
    // label wraps rather than clipping them.
    m_statusLabel->setWordWrap(true);
    m_statusIsError = true;   // force the first style application
    setStatusError(false);
    outerVbox->addWidget(m_statusLabel);

    // Wire buttons
    connect(m_recBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked) m_model->recStart(m_selectedSlot);
        else         m_model->recStop(m_selectedSlot);
    });

    connect(m_stopBtn, &QPushButton::clicked, this, [this]() {
        int id = m_model->activeId();
        if (id < 0) id = m_selectedSlot;
        if (id < 1) return;
        switch (m_model->status()) {
        case VoiceKeyer::Recording: m_model->recStop(id); break;
        case VoiceKeyer::Playback:  m_model->playbackStop(id); break;
        case VoiceKeyer::Preview:   m_model->previewStop(id); break;
        default: break;
        }
    });

    connect(m_xmitBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked && durationForSlot(m_selectedSlot) > 0)
            m_model->playbackStart(m_selectedSlot);
        else if (checked) { m_xmitBtn->blockSignals(true); m_xmitBtn->setChecked(false); m_xmitBtn->blockSignals(false); }
        else m_model->playbackStop(m_selectedSlot);
    });

    connect(m_previewBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked && durationForSlot(m_selectedSlot) > 0)
            m_model->previewStart(m_selectedSlot);
        else if (checked) { m_previewBtn->blockSignals(true); m_previewBtn->setChecked(false); m_previewBtn->blockSignals(false); }
        else m_model->previewStop(m_selectedSlot);
    });

    connectKeyer();

    // F1-F12 hotkeys (only play if slot has a recording).  Registered as
    // Qt::ApplicationShortcut on window() and created disabled — MainWindow
    // flips enable state based on the active slice's mode (mutually
    // exclusive with CwxPanel's F1-F12 set) so the keys fire regardless of
    // panel visibility while Qt still sees at most one enabled shortcut
    // per key and never emits activatedAmbiguously. (#2464, #2582)
    for (int i = 0; i < 12; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), window());
        sc->setContext(Qt::ApplicationShortcut);
        sc->setEnabled(false);
        m_shortcuts.append(sc);
        connect(sc, &QShortcut::activated, this, [this, i]() {
            int id = i + 1;
            selectSlot(id);
            if (m_model->status() == VoiceKeyer::Playback && m_model->activeId() == id)
                m_model->playbackStop(id);
            else if (durationForSlot(id) > 0)
                m_model->playbackStart(id);
        });
    }

    // Escape: cancel rename if active, otherwise stop DVK operation.
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), window());
    esc->setContext(Qt::ApplicationShortcut);
    esc->setEnabled(false);
    m_shortcuts.append(esc);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (m_renameEdit) {
            cancelRename();
            return;
        }
        int id = m_model->activeId();
        if (id < 0) return;
        switch (m_model->status()) {
        case VoiceKeyer::Recording: m_model->recStop(id); break;
        case VoiceKeyer::Playback:  m_model->playbackStop(id); break;
        case VoiceKeyer::Preview:   m_model->previewStop(id); break;
        default: break;
        }
    });

    // Elapsed timer for recording/playback/preview progress
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(100);
    connect(m_elapsedTimer, &QTimer::timeout, this, &DvkPanel::onElapsedTick);

    m_selectedSlot = 1;
    selectSlot(1);
    refreshFromKeyer();
}

void DvkPanel::connectKeyer()
{
    connect(m_model, &VoiceKeyer::statusChanged, this, &DvkPanel::onStatusChanged);
    connect(m_model, &VoiceKeyer::recordingChanged, this, &DvkPanel::onRecordingChanged);

    // Surface radio rejections instead of silently toggling buttons.  Without
    // this the REC button latched "checked" on a rejected rec_start. (#3377)
    connect(m_model, &VoiceKeyer::commandFailed, this,
            [this](const QString& verb, int id, uint /*code*/, const QString& message) {
        // Re-drive the buttons from the current (unchanged) status so the
        // failed momentary press is visually released.  This must run *first*:
        // onStatusChanged() rewrites m_statusLabel ("Status: Idle"), so set the
        // failure text afterwards or it gets clobbered before the event loop
        // returns and the user never sees the rejection. (#3377)
        onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
        setStatusError(true);
        m_statusLabel->setText(QString("Status: %1 (slot %2) failed — %3")
                                   .arg(verb).arg(id).arg(message));
    });

    // WAV import/export progress and outcome, whichever keyer carries it.
    connect(m_model, &VoiceKeyer::transferStatusChanged, this, [this](const QString& msg) {
        setStatusError(false);
        m_statusLabel->setText(msg);
    });
    connect(m_model, &VoiceKeyer::transferFinished,
            this, [this](bool success, const QString& msg) {
        setStatusError(!success);
        m_statusLabel->setText(success ? msg : QString("Transfer failed: %1").arg(msg));
    });
}

void DvkPanel::refreshFromKeyer()
{
    const QString source = m_model->sourceLabel();
    m_titleLabel->setText(source.isEmpty() ? QStringLiteral("Digital Voice Keyer")
                                           : QStringLiteral("Digital Voice Keyer (%1)").arg(source));
    for (int id = 1; id <= 12; ++id)
        onRecordingChanged(id);
    onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
}

void DvkPanel::setKeyer(VoiceKeyer* keyer)
{
    if (!keyer || keyer == m_model)
        return;
    cancelRename();
    disconnect(m_model, nullptr, this, nullptr);
    disconnect(m_model, nullptr, m_statusLabel, nullptr);
    m_model = keyer;
    connectKeyer();
    refreshFromKeyer();
}

void DvkPanel::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts) sc->setEnabled(enabled);
}

void DvkPanel::selectSlot(int id)
{
    m_selectedSlot = id;
    for (int i = 0; i < m_rowFrames.size(); ++i) {
        bool selected = (i + 1 == id);
        m_rowFrames[i]->setStyleSheet(selected
            ? "QFrame { background: #1a2a4a; border: 1px solid #00b4d8; border-radius: 3px; }"
            : "QFrame { background: #0f1520; border: 1px solid #203040; border-radius: 3px; }");
    }
}

int DvkPanel::selectedSlot() const
{
    return m_selectedSlot;
}

void DvkPanel::rebuildXmitIcon()
{
    // Off: the button's own text colour (from its stylesheet). On: the checked
    // state's white text. Drawn at 2x at least so it stays sharp on HiDPI.
    constexpr int kIconPx = 14;
    const qreal dpr = std::max<qreal>(2.0, devicePixelRatioF());
    m_xmitBtn->ensurePolished();
    QIcon icon;
    icon.addPixmap(antennaPixmap(m_xmitBtn->palette().color(QPalette::ButtonText), kIconPx, dpr),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(antennaPixmap(QColor(Qt::white), kIconPx, dpr), QIcon::Normal, QIcon::On);
    m_xmitBtn->setIcon(icon);
    m_xmitBtn->setIconSize(QSize(kIconPx, kIconPx));
}

void DvkPanel::setStatusError(bool error)
{
    // A failure the operator must act on is shown bold and in the theme's
    // danger colour; the next ordinary status puts the quiet style back.
    if (error == m_statusIsError)
        return;
    m_statusIsError = error;
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_statusLabel, error
        ? "QLabel { color: {{color.accent.danger}}; font-weight: bold; font-size: 12px; }"
        : "QLabel { color: {{color.text.label}}; font-size: 10px; }");
}

void DvkPanel::onStatusChanged(int status, int id)
{
    auto s = static_cast<VoiceKeyer::Status>(status);
    setStatusError(false);

    m_recBtn->blockSignals(true);
    m_xmitBtn->blockSignals(true);
    m_previewBtn->blockSignals(true);

    m_recBtn->setChecked(s == VoiceKeyer::Recording);
    m_xmitBtn->setChecked(s == VoiceKeyer::Playback);
    m_previewBtn->setChecked(s == VoiceKeyer::Preview);

    m_recBtn->blockSignals(false);
    m_xmitBtn->blockSignals(false);
    m_previewBtn->blockSignals(false);

    // Highlight active slot's F-key button
    for (int i = 0; i < m_fkeyBtns.size(); ++i) {
        bool active = (i + 1 == id) && (s == VoiceKeyer::Playback || s == VoiceKeyer::Recording || s == VoiceKeyer::Preview);
        m_fkeyBtns[i]->setStyleSheet(active
            ? "QPushButton { background: #00b4d8; color: #000; border: 1px solid #00b4d8; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
            : kFKeyStyle);
    }

    bool isActive = (s == VoiceKeyer::Recording || s == VoiceKeyer::Playback || s == VoiceKeyer::Preview);

    if (isActive) {
        // Start or restart elapsed timer
        if (m_timerSlotId != id || m_timerStatus != status) {
            m_elapsedMs = 0;
            m_timerSlotId = id;
            m_timerStatus = status;

            // Hide any previous progress bar
            for (auto* bar : m_progressBars) bar->hide();

            // Show and configure progress bar on active slot
            if (id >= 1 && id <= 12) {
                auto* bar = m_progressBars[id - 1];
                int totalMs = durationForSlot(id);

                // Color: red=recording, green=playback, blue=preview
                const char* color = (s == VoiceKeyer::Recording) ? "#cc3333"
                                  : (s == VoiceKeyer::Playback)  ? "#33aa33"
                                  :                               "#3388cc";
                bar->setStyleSheet(QString(
                    "QProgressBar { background: transparent; border: none; }"
                    "QProgressBar::chunk { background: %1; border-radius: 1px; }").arg(color));

                if (totalMs > 0 && s != VoiceKeyer::Recording) {
                    bar->setRange(0, totalMs);
                    bar->setValue(0);
                    bar->show();
                } else {
                    // Recording: indeterminate — show as full bar that stays visible
                    bar->setRange(0, 0);
                    bar->show();
                }
            }

            if (!m_elapsedTimer->isActive())
                m_elapsedTimer->start();
        }

        // Update status label with initial text (tick will update with elapsed)
        onElapsedTick();
    } else {
        // Stop timer and hide progress bars
        m_elapsedTimer->stop();
        m_timerSlotId = -1;
        m_timerStatus = 0;
        m_elapsedMs = 0;
        for (auto* bar : m_progressBars) bar->hide();

        switch (s) {
        case VoiceKeyer::Idle:     m_statusLabel->setText("Status: Idle"); break;
        case VoiceKeyer::Disabled: m_statusLabel->setText("Status: Disabled (SmartSDR+ required)"); break;
        default:                   m_statusLabel->setText("Status: Idle"); break;
        }
    }
}

void DvkPanel::onRecordingChanged(int id)
{
    if (id < 1 || id > 12) return;
    int idx = id - 1;
    // A slot the keyer holds nothing for (deleted, or a keyer that never had
    // it) shows as an empty row rather than keeping whatever was there before.
    QString name = QString("Recording %1").arg(id);
    int durationMs = 0;
    for (const auto& r : m_model->recordings()) {
        if (r.id == id) {
            name = r.name;
            durationMs = r.durationMs;
            break;
        }
    }
    m_nameLabels[idx]->setText(name);
    m_durLabels[idx]->setText(durationMs > 0 ? formatDuration(durationMs) : "Empty");
    m_nameLabels[idx]->setStyleSheet(durationMs > 0
        ? kNameStyle
        : "QLabel { color: #505060; font-size: 10px; }");
}

void DvkPanel::onElapsedTick()
{
    m_elapsedMs += 100;

    auto s = static_cast<VoiceKeyer::Status>(m_timerStatus);
    QString elapsed = formatDuration(m_elapsedMs);
    int totalMs = durationForSlot(m_timerSlotId);

    switch (s) {
    case VoiceKeyer::Recording:
        m_statusLabel->setText(QString("Status: Recording %1 / %2").arg(m_timerSlotId).arg(elapsed));
        break;
    case VoiceKeyer::Playback:
    case VoiceKeyer::Preview: {
        // Named after the buttons: XMIT transmits, PLAY plays locally.
        QString label = (s == VoiceKeyer::Playback) ? "Transmitting" : "Playing";
        if (totalMs > 0)
            m_statusLabel->setText(QString("Status: %1 %2 / %3")
                .arg(label).arg(m_timerSlotId).arg(elapsed));
        else
            m_statusLabel->setText(QString("Status: %1 %2 / %3")
                .arg(label).arg(m_timerSlotId).arg(elapsed));
        break;
    }
    default: break;
    }

    // Update progress bar
    if (m_timerSlotId >= 1 && m_timerSlotId <= 12 && totalMs > 0 && s != VoiceKeyer::Recording) {
        m_progressBars[m_timerSlotId - 1]->setValue(qMin(m_elapsedMs, totalMs));
    }
}

int DvkPanel::durationForSlot(int id) const
{
    for (const auto& r : m_model->recordings())
        if (r.id == id) return r.durationMs;
    return 0;
}

QString DvkPanel::formatDuration(int ms)
{
    int secs = ms / 1000;
    int frac = (ms % 1000) / 100;
    return QString("%1.%2s").arg(secs).arg(frac);
}

// ── Event filter (double-click name label → rename) ────────────────────────

bool DvkPanel::eventFilter(QObject* obj, QEvent* event)
{
    int id = obj->property("slotId").toInt();
    if (id < 1 || id > 12)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            selectSlot(id);
            return false;  // don't consume — let double-click still work
        }
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        // Only name labels trigger rename (not the row frame itself)
        if (qobject_cast<QLabel*>(obj)) {
            selectSlot(id);
            startRename(id);
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

// ── Context menu ───────────────────────────────────────────────────────────

void DvkPanel::showContextMenu(int id, const QPoint& globalPos)
{
    QMenu menu;

    auto* renameAct = menu.addAction("Rename…");
    menu.addSeparator();
    auto* clearAct = menu.addAction("Clear");
    auto* deleteAct = menu.addAction("Delete");
    menu.addSeparator();
    auto* importAct = menu.addAction("Import WAV…");
    auto* exportAct = menu.addAction("Export WAV…");

    int dur = durationForSlot(id);
    bool hasRecording = dur > 0;
    bool notBusy = m_model->canTransferWav() && !m_model->isTransferring();
    clearAct->setEnabled(hasRecording);
    deleteAct->setEnabled(hasRecording);
    importAct->setEnabled(notBusy);
    exportAct->setEnabled(notBusy && hasRecording);

    connect(renameAct, &QAction::triggered, this, [this, id]() { startRename(id); });
    connect(clearAct, &QAction::triggered, this, [this, id]() { m_model->clear(id); });
    connect(deleteAct, &QAction::triggered, this, [this, id]() { m_model->remove(id); });

    connect(importAct, &QAction::triggered, this, [this, id]() {
        QString path = QFileDialog::getOpenFileName(this,
            "Import WAV to DVK Slot",
            QDir::homePath(),
            "WAV Files (*.wav)");
        if (path.isEmpty()) return;

        m_model->importWav(id, path);
    });

    connect(exportAct, &QAction::triggered, this, [this, id]() {
        QString name;
        for (const auto& r : m_model->recordings()) {
            if (r.id == id) { name = r.name; break; }
        }
        if (name.isEmpty()) name = QString("Recording_%1").arg(id);
        name.replace(QRegularExpression("[^\\w\\s-]"), "_");

        QString path = QFileDialog::getSaveFileName(this,
            "Export DVK Recording",
            QDir::homePath() + "/" + name + ".wav",
            "WAV Files (*.wav)");
        if (path.isEmpty()) return;

        m_model->exportWav(id, path);
    });

    AetherSDR::ThemeManager::instance().applyStyleSheet(&menu, "QMenu { background: {{color.background.1}}; color: {{color.text.primary}}; border: 1px solid {{color.background.1}}; }"
        "QMenu::item:selected { background: {{color.accent}}; color: {{color.background.spectrum}}; }"
        "QMenu::item:disabled { color: #505060; }"
        "QMenu::separator { height: 1px; background: {{color.background.1}}; margin: 2px 6px; }");

    menu.exec(globalPos);
}

// ── Inline rename ──────────────────────────────────────────────────────────

void DvkPanel::startRename(int id)
{
    if (m_renameEdit) cancelRename();

    int idx = id - 1;
    auto* label = m_nameLabels[idx];
    auto* rowLayout = qobject_cast<QHBoxLayout*>(
        m_rowFrames[idx]->layout()->itemAt(0)->layout());
    if (!rowLayout) return;

    m_renameSlot = id;
    m_renameEdit = new QLineEdit;
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_renameEdit, "QLineEdit { background: {{color.background.1}}; color: {{color.text.primary}}; border: 1px solid {{color.accent}}; "
        "border-radius: 2px; font-size: 10px; padding: 0px 2px; }");
    m_renameEdit->setText(label->text());
    m_renameEdit->selectAll();
    m_renameEdit->setMaxLength(40);

    // Swap label out, edit in (same layout position)
    int labelIdx = rowLayout->indexOf(label);
    label->hide();
    rowLayout->insertWidget(labelIdx, m_renameEdit, 1);
    m_renameEdit->setFocus();

    connect(m_renameEdit, &QLineEdit::returnPressed, this, &DvkPanel::commitRename);
    connect(m_renameEdit, &QLineEdit::editingFinished, this, &DvkPanel::commitRename);
}

void DvkPanel::commitRename()
{
    if (!m_renameEdit || m_renameSlot < 1) return;

    int idx = m_renameSlot - 1;
    QString name = m_renameEdit->text().trimmed();

    // Strip forbidden chars (quotes break protocol parsing)
    name.remove('\'');
    name.remove('"');

    if (!name.isEmpty())
        m_model->setName(m_renameSlot, name);

    m_nameLabels[idx]->show();
    m_renameEdit->deleteLater();
    m_renameEdit = nullptr;
    m_renameSlot = -1;
}

void DvkPanel::cancelRename()
{
    if (!m_renameEdit || m_renameSlot < 1) return;

    int idx = m_renameSlot - 1;
    m_nameLabels[idx]->show();
    m_renameEdit->deleteLater();
    m_renameEdit = nullptr;
    m_renameSlot = -1;
}

} // namespace AetherSDR
