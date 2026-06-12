#include "VirtualSerialPort.h"
#include "LogManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSocketNotifier>

#ifndef Q_OS_WIN
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef Q_OS_MAC
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace AetherSDR {

VirtualSerialPort::VirtualSerialPort(QObject* parent)
    : QObject(parent)
{}

VirtualSerialPort::~VirtualSerialPort()
{
    close();
}

void VirtualSerialPort::setSymlinkPath(const QString& path)
{
    m_symlinkPath = path;
}

QString VirtualSerialPort::symlinkPath() const
{
    return m_symlinkPath;
}

bool VirtualSerialPort::isOpen() const
{
#ifndef Q_OS_WIN
    return m_masterFd >= 0;
#else
    return false;
#endif
}

QString VirtualSerialPort::slavePath() const
{
#ifndef Q_OS_WIN
    if (!m_symlinkPath.isEmpty() && isOpen())
        return m_symlinkPath;
    return m_slavePath;
#else
    return {};
#endif
}

bool VirtualSerialPort::open()
{
#ifndef Q_OS_WIN
    if (isOpen()) return true;

    char slaveName[256] = {};
    if (::openpty(&m_masterFd, &m_slaveFd, slaveName, nullptr, nullptr) != 0) {
        qCWarning(lcCat) << "VirtualSerialPort: openpty() failed";
        return false;
    }

    m_slavePath = QString::fromLocal8Bit(slaveName);

    int flags = ::fcntl(m_masterFd, F_GETFL);
    ::fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    struct termios tio;
    if (::tcgetattr(m_slaveFd, &tio) == 0) {
        ::cfmakeraw(&tio);
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
        ::tcsetattr(m_slaveFd, TCSANOW, &tio);
    }

    if (!m_symlinkPath.isEmpty()) {
        // Atomic replace: symlink to a tmp name, then rename().
        // Avoids the TOCTOU window of unlink+symlink (GHSA-qxhr-cwrc-pvrm).
        const QFileInfo info(m_symlinkPath);
        const QString parentDir = info.absolutePath();
        if (!QDir().mkpath(parentDir))
            qCWarning(lcCat) << "VirtualSerialPort: mkpath failed:" << parentDir;
        if (::chmod(parentDir.toLocal8Bit().constData(), 0700) != 0)
            qCWarning(lcCat) << "VirtualSerialPort: chmod 0700 failed:" << parentDir;

        const QString tmpPath = m_symlinkPath + QStringLiteral(".tmp");
        ::unlink(tmpPath.toLocal8Bit().constData());
        if (::symlink(slaveName, tmpPath.toLocal8Bit().constData()) == 0) {
            if (::rename(tmpPath.toLocal8Bit().constData(),
                         m_symlinkPath.toLocal8Bit().constData()) != 0) {
                ::unlink(tmpPath.toLocal8Bit().constData());
                qCWarning(lcCat) << "VirtualSerialPort: symlink rename failed:" << m_symlinkPath;
            }
        } else {
            qCWarning(lcCat) << "VirtualSerialPort: symlink failed:" << tmpPath;
        }
    }

    // Assert RTS+DTR on the master so the slave sees CTS+DSR active.
    // Without this, clients using hardware flow control (e.g. go-serial with
    // CRTSCTS) will block writes indefinitely waiting for CTS.
    int mctl = TIOCM_RTS | TIOCM_DTR;
    ::ioctl(m_masterFd, TIOCMSET, &mctl);

    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &VirtualSerialPort::onReadReady);

    emit pathChanged(slavePath());
    return true;
#else
    return false;
#endif
}

void VirtualSerialPort::close()
{
#ifndef Q_OS_WIN
    if (!isOpen()) return;

    delete m_notifier;
    m_notifier = nullptr;

    ::close(m_masterFd);
    ::close(m_slaveFd);
    m_masterFd = -1;
    m_slaveFd  = -1;

    if (!m_symlinkPath.isEmpty())
        ::unlink(m_symlinkPath.toLocal8Bit().constData());

    m_slavePath.clear();
    emit pathChanged({});
#endif
}

void VirtualSerialPort::write(const QByteArray& data)
{
#ifndef Q_OS_WIN
    if (!isOpen() || data.isEmpty()) return;
    if (::write(m_masterFd, data.constData(), static_cast<size_t>(data.size())) < 0)
        qCWarning(lcCat) << "VirtualSerialPort: write failed";
#else
    Q_UNUSED(data)
#endif
}

void VirtualSerialPort::onReadReady()
{
#ifndef Q_OS_WIN
    char buf[4096];
    ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
    if (n > 0)
        emit dataReceived(QByteArray(buf, static_cast<int>(n)));
#endif
}

} // namespace AetherSDR
