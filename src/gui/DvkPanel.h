#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QTimer>
#include <QVector>

class QFrame;
class QShortcut;

namespace AetherSDR {
class VoiceKeyer;

class DvkPanel : public QWidget {
    Q_OBJECT
public:
    // The panel drives whichever keyer it is given — the radio DVK or the
    // client-side keyer — through the VoiceKeyer interface only.
    explicit DvkPanel(VoiceKeyer* keyer, QWidget* parent = nullptr);
    int selectedSlot() const;

    // Switch the panel to another keyer (radio DVK <-> client-side). Rows,
    // title and buttons are re-read from the new keyer; the old one's signals
    // no longer reach the panel.
    void setKeyer(VoiceKeyer* keyer);
    VoiceKeyer* keyer() const { return m_model; }

    // Enable/disable the F1-F12 and Esc ApplicationShortcuts. Driven by
    // the active slice's mode in MainWindow so the keys fire regardless
    // of panel visibility, while staying mutually exclusive with CwxPanel
    // to avoid Qt shortcut ambiguity. (#2582)
    void setShortcutsEnabled(bool enabled);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onStatusChanged(int status, int id);
    void onRecordingChanged(int id);
    void onElapsedTick();

private:
    VoiceKeyer* m_model;
    QLabel* m_titleLabel{nullptr};
    QVector<QFrame*> m_rowFrames;
    QVector<QPushButton*> m_fkeyBtns;
    QVector<QLabel*> m_nameLabels;
    QVector<QLabel*> m_durLabels;
    QVector<QProgressBar*> m_progressBars;
    QPushButton* m_recBtn;
    QPushButton* m_stopBtn;
    QPushButton* m_playBtn;
    QPushButton* m_prevBtn;
    QLabel* m_statusLabel;
    bool m_statusIsError{false};
    int m_selectedSlot{1};
    QLineEdit* m_renameEdit{nullptr};
    int m_renameSlot{-1};

    // Elapsed timer state
    QTimer* m_elapsedTimer{nullptr};
    int m_elapsedMs{0};
    int m_timerSlotId{-1};
    int m_timerStatus{0};  // VoiceKeyer::Status cast to int

    // F1-F12 + ESC shortcuts — ApplicationShortcut on window(), enabled
    // by MainWindow based on the active slice's mode (mutually exclusive
    // with CwxPanel's set) so they fire regardless of panel visibility
    // (#2464, #2582).
    QVector<QShortcut*> m_shortcuts;

    void connectKeyer();
    void refreshFromKeyer();
    void setStatusError(bool error);
    void selectSlot(int id);
    void showContextMenu(int id, const QPoint& globalPos);
    void startRename(int id);
    void commitRename();
    void cancelRename();
    QString formatDuration(int ms);
    int durationForSlot(int id) const;
};

} // namespace AetherSDR
