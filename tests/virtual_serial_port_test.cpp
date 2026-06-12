// Unit test for VirtualSerialPort — POSIX only.
//
// Tests that the PTY opens and provides a valid slave device, that data written
// to the slave side arrives via the dataReceived signal, that write() sends data
// to the slave side, and that close() tears down the PTY cleanly.
//
// Skipped at runtime on Windows (open() returns false) — the test binary still
// links and is still registered with ctest, but all checks are skipped.

#include "core/VirtualSerialPort.h"

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <cstdio>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using AetherSDR::VirtualSerialPort;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const char* detail = nullptr)
{
    std::printf("%s %s", ok ? "[ OK ]" : "[FAIL]", name);
    if (detail && *detail)
        std::printf("  (%s)", detail);
    std::printf("\n");
    if (!ok)
        ++g_failed;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Lifecycle ──────────────────────────────────────────────────────────────

    {
        VirtualSerialPort pty;
        report("initial: isOpen() == false", !pty.isOpen());
        report("initial: slavePath() is empty", pty.slavePath().isEmpty());

#if defined(Q_OS_UNIX)
        bool opened = pty.open();
        report("open() returns true", opened);

        if (!opened) {
            report("SKIP all PTY I/O tests (open failed)", true);
        } else {
            report("isOpen() == true after open()", pty.isOpen());
            report("slavePath() non-empty after open()", !pty.slavePath().isEmpty());

            const QString slavePath = pty.slavePath();

            // ── Slave-side file descriptor for I/O ────────────────────────────
            int slaveFd = ::open(slavePath.toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            const bool slaveOpened = (slaveFd >= 0);
            report("slave device openable by path", slaveOpened,
                   slaveOpened ? "" : ::strerror(errno));

            if (slaveOpened) {
                // ── RX path: slave→signal ──────────────────────────────────────
                QByteArray received;
                QObject::connect(&pty, &VirtualSerialPort::dataReceived,
                                 [&received](const QByteArray& d) { received.append(d); });

                const QByteArray txToMaster = "HELLO\n";
                const ssize_t written = ::write(slaveFd, txToMaster.constData(),
                                                static_cast<size_t>(txToMaster.size()));
                report("write to slave fd succeeds", written == txToMaster.size());

                // Let the QSocketNotifier fire.
                for (int i = 0; i < 50 && received.isEmpty(); ++i)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

                report("dataReceived signal fires with correct data",
                       received == txToMaster,
                       received.isEmpty() ? "(empty)" : received.constData());

                // ── TX path: write()→slave ─────────────────────────────────────
                const QByteArray txToSlave = "WORLD\n";
                pty.write(txToSlave);

                char buf[64] = {};
                // Poll for up to 1 s to allow the OS to deliver bytes.
                ssize_t n = 0;
                for (int i = 0; i < 50 && n <= 0; ++i) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    n = ::read(slaveFd, buf, sizeof(buf) - 1);
                }
                const QByteArray readBack(buf, static_cast<int>(n > 0 ? n : 0));
                report("write() delivers data to slave fd", readBack == txToSlave,
                       readBack.isEmpty() ? "(empty)" : readBack.constData());

                ::close(slaveFd);
            }

            // ── Close ──────────────────────────────────────────────────────────
            pty.close();
            report("isOpen() == false after close()", !pty.isOpen());
            report("slavePath() empty after close()", pty.slavePath().isEmpty());
        }
#else
        // Windows: open() is a no-op; just verify it doesn't crash.
        report("open() returns false on Windows (no-op)", !pty.open());
        report("isOpen() stays false on Windows", !pty.isOpen());
#endif
    }

    // ── Symlink path ───────────────────────────────────────────────────────────
#if defined(Q_OS_UNIX)
    {
        VirtualSerialPort pty2;
        // Use a tmp path that won't collide with production symlinks.
        const QString symlinkPath = QStringLiteral("/tmp/virtual_serial_port_test_link");
        pty2.setSymlinkPath(symlinkPath);
        report("symlinkPath() matches set value", pty2.symlinkPath() == symlinkPath);

        bool opened2 = pty2.open();
        report("open() with symlink path succeeds", opened2);
        if (opened2) {
            // slavePath() must equal the symlink when one is configured.
            report("slavePath() returns symlink when configured", pty2.slavePath() == symlinkPath,
                   pty2.slavePath().toLocal8Bit().constData());

            // Symlink must exist on disk and resolve to a device.
            const bool symlinkExists = (::access(symlinkPath.toLocal8Bit().constData(), F_OK) == 0);
            report("symlink exists on disk", symlinkExists);

            pty2.close();

            // Symlink must be removed on close.
            const bool symlinkGone = (::access(symlinkPath.toLocal8Bit().constData(), F_OK) != 0);
            report("symlink removed on close()", symlinkGone);
        }
    }
#endif

    // ── Double-open guard ──────────────────────────────────────────────────────
#if defined(Q_OS_UNIX)
    {
        VirtualSerialPort pty3;
        const bool first  = pty3.open();
        const bool second = pty3.open();  // must be idempotent (true, not double-open)
        report("second open() while already open returns true", first && second);
        report("isOpen() still true after double open()", pty3.isOpen());
        pty3.close();
    }
#endif

    std::printf("\n%s — %d failure(s)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
