#pragma once

#include <QObject>
#include <QString>

class QSocketNotifier;

namespace AetherSDR {

// Platform-portable virtual serial port backed by a PTY on POSIX systems.
//
// Call setSymlinkPath() before open() to create a stable per-user symlink
// (atomic replace, TOCTOU-safe) pointing at the underlying /dev/pts/N device.
// slavePath() returns the symlink path if one is set, otherwise /dev/pts/N.
//
// On Windows, open() is a no-op and slavePath() returns empty — callers
// should check isOpen() and hide or grey out any path-display UI accordingly.
class VirtualSerialPort : public QObject {
    Q_OBJECT

public:
    explicit VirtualSerialPort(QObject* parent = nullptr);
    ~VirtualSerialPort() override;

    void    setSymlinkPath(const QString& path);
    QString symlinkPath() const;

    bool    open();
    void    close();
    bool    isOpen() const;
    QString slavePath() const;

    void write(const QByteArray& data);

signals:
    void dataReceived(const QByteArray& data);
    void pathChanged(const QString& path);

private slots:
    void onReadReady();

private:
    QString m_symlinkPath;

#ifndef Q_OS_WIN
    int              m_masterFd{-1};
    int              m_slaveFd{-1};
    QString          m_slavePath;
    QSocketNotifier* m_notifier{nullptr};
#endif
};

} // namespace AetherSDR
