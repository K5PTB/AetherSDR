#include "CatPort.h"
#include "LogManager.h"
#include "RigctlProtocol.h"
#include "SmartCatProtocol.h"
#include "SmartCatSession.h"
#include "VirtualSerialPort.h"
#include "models/RadioModel.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

namespace AetherSDR {

// Per-user symlink path — mirrors RigctlPty::defaultSymlinkPath() exactly
// (GHSA-qxhr-cwrc-pvrm: no /tmp cross-user collisions, no TOCTOU window).
QString CatPort::defaultSymlinkPath(int portIndex)
{
    QString leaf;
    if (portIndex >= 0 && portIndex < 8) {
        const char letter = static_cast<char>('A' + portIndex);
        leaf = QStringLiteral("cat-%1").arg(QChar(letter));
    } else {
        leaf = QStringLiteral("cat-%1").arg(portIndex);
    }

    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache/aethersdr");

    if (!base.contains(QStringLiteral("aethersdr"), Qt::CaseInsensitive))
        base += QStringLiteral("/aethersdr");

    return base + QChar('/') + leaf;
}

CatPort::CatPort(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_pty(std::make_unique<VirtualSerialPort>(this))
{
    connect(m_pty.get(), &VirtualSerialPort::dataReceived,
            this, &CatPort::onPtyData);
    connect(m_pty.get(), &VirtualSerialPort::pathChanged,
            this, &CatPort::ptyPathChanged);
}

void CatPort::setSymlinkPath(const QString& path) { m_pty->setSymlinkPath(path); }
QString CatPort::symlinkPath() const               { return m_pty->symlinkPath(); }

CatPort::~CatPort()
{
    stop();
}

bool CatPort::start(quint16 port)
{
    if (isRunning()) return true;

    m_tcpServer = new QTcpServer(this);
    if (!m_tcpServer->listen(QHostAddress::Any, port)) {
        qCWarning(lcCat) << "CatPort: failed to bind port" << port
                         << m_tcpServer->errorString();
        delete m_tcpServer;
        m_tcpServer = nullptr;
        return false;
    }

    connect(m_tcpServer, &QTcpServer::newConnection,
            this, &CatPort::onNewConnection);

    startPty();

    qCInfo(lcCat) << "CatPort: listening on port" << port
                  << "dialect" << static_cast<int>(m_dialect);
    return true;
}

void CatPort::stop()
{
    stopPty();

    // Swap out the client list before iterating: abort() emits disconnected
    // synchronously, and onRigctlDisconnected calls removeAt() on m_rigctlClients,
    // which would invalidate the range-for with 2+ clients. Disconnecting signals
    // first prevents that re-entrant mutation and the double-free it causes.
    // Also fixes the original UAF: Qt parents sockets to m_tcpServer, so
    // delete m_tcpServer frees them — abort() after that is a use-after-free.
    auto clients = std::exchange(m_rigctlClients, {});
    for (auto& c : clients) {
        QObject::disconnect(c.socket, nullptr, this, nullptr);
        c.socket->abort();
        delete c.protocol;
    }

    if (m_tcpServer) {
        m_tcpServer->close();
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }

    // Close all SmartCAT sessions (sessions own their sockets)
    for (auto* s : m_catSessions)
        s->deleteLater();
    m_catSessions.clear();

    emit clientCountChanged(0);
}

bool CatPort::isRunning() const
{
    return m_tcpServer && m_tcpServer->isListening();
}

quint16 CatPort::port() const
{
    return m_tcpServer ? m_tcpServer->serverPort() : 0;
}

int CatPort::clientCount() const
{
    return m_rigctlClients.size() + m_catSessions.size();
}

QString CatPort::ptyPath() const
{
    return m_pty->slavePath();
}

// ── New TCP connection ───────────────────────────────────────────────────────

void CatPort::onNewConnection()
{
    QTcpSocket* socket = m_tcpServer->nextPendingConnection();
    if (!socket) return;

    if (m_dialect == CatDialect::Rigctld) {
        RigctlClient c;
        c.socket   = socket;
        c.protocol = new RigctlProtocol(m_model);
        c.protocol->setSliceIndex(m_vfoA);
        m_rigctlClients.append(c);

        connect(socket, &QTcpSocket::readyRead,
                this, &CatPort::onRigctlData);
        connect(socket, &QTcpSocket::disconnected,
                this, &CatPort::onRigctlDisconnected);

        qCInfo(lcCat) << "CatPort: rigctld client connected on port" << port();
    } else {
        bool flex = (m_dialect == CatDialect::FlexCAT);
        auto* session = new SmartCatSession(socket, m_model,
                                            m_vfoA, m_vfoB, flex, this);
        m_catSessions.append(session);

        connect(session, &SmartCatSession::sessionEnded,
                this, &CatPort::onCatSessionEnded);

        qCInfo(lcCat) << "CatPort: SmartCAT client connected on port" << port()
                      << "(flex=" << flex << ")";
    }

    emit clientCountChanged(clientCount());
}

// ── Rigctld TCP I/O ──────────────────────────────────────────────────────────

void CatPort::onRigctlData()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    for (auto& c : m_rigctlClients) {
        if (c.socket != socket) continue;
        c.buffer.append(socket->readAll());

        while (true) {
            int nlPos = c.buffer.indexOf('\n');
            if (nlPos < 0) nlPos = c.buffer.indexOf('\r');
            if (nlPos < 0) break;

            QString line = QString::fromUtf8(c.buffer.left(nlPos)).trimmed();
            c.buffer.remove(0, nlPos + 1);
            if (line.isEmpty()) continue;

            QString resp = c.protocol->handleLine(line);
            if (!resp.isEmpty())
                socket->write(resp.toUtf8());
        }
        break;
    }
}

void CatPort::onRigctlDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    for (int i = 0; i < m_rigctlClients.size(); ++i) {
        if (m_rigctlClients[i].socket == socket) {
            delete m_rigctlClients[i].protocol;
            m_rigctlClients.removeAt(i);
            socket->deleteLater();
            qCInfo(lcCat) << "CatPort: rigctld client disconnected";
            break;
        }
    }
    emit clientCountChanged(clientCount());
}

// ── SmartCAT session ended ───────────────────────────────────────────────────

void CatPort::onCatSessionEnded(SmartCatSession* session)
{
    m_catSessions.removeOne(session);
    session->deleteLater();
    qCInfo(lcCat) << "CatPort: SmartCAT session ended";
    emit clientCountChanged(clientCount());
}

// ── PTY ─────────────────────────────────────────────────────────────────────

void CatPort::startPty()
{
    if (!m_pty->open()) return;

    if (m_dialect == CatDialect::Rigctld) {
        m_ptyRigctlProtocol = new RigctlProtocol(m_model);
        m_ptyRigctlProtocol->setSliceIndex(m_vfoA);
    } else {
        bool flex = (m_dialect == CatDialect::FlexCAT);
        m_ptyCatProtocol = new SmartCatProtocol(m_model, m_vfoA, m_vfoB, flex);
    }

    qCInfo(lcCat) << "CatPort: PTY started on" << m_pty->slavePath()
                  << "symlink:" << m_pty->symlinkPath();
    // ptyPathChanged emitted by VirtualSerialPort::open() via the pathChanged signal
}

void CatPort::stopPty()
{
    if (!m_pty->isOpen()) return;

    m_pty->close();

    delete m_ptyRigctlProtocol;
    m_ptyRigctlProtocol = nullptr;
    delete m_ptyCatProtocol;
    m_ptyCatProtocol = nullptr;

    m_ptyBuffer.clear();
    qCInfo(lcCat) << "CatPort: PTY stopped";
    // ptyPathChanged emitted by VirtualSerialPort::close() via the pathChanged signal
}

void CatPort::onPtyData(const QByteArray& data)
{
    m_ptyBuffer.append(data);

    // Rigctld framing: newline-delimited
    if (m_dialect == CatDialect::Rigctld) {
        while (true) {
            int nlPos = m_ptyBuffer.indexOf('\n');
            if (nlPos < 0) nlPos = m_ptyBuffer.indexOf('\r');
            if (nlPos < 0) break;

            QString line = QString::fromUtf8(m_ptyBuffer.left(nlPos)).trimmed();
            m_ptyBuffer.remove(0, nlPos + 1);
            if (line.isEmpty()) continue;

            if (m_ptyRigctlProtocol) {
                QString resp = m_ptyRigctlProtocol->handleLine(line);
                if (!resp.isEmpty())
                    m_pty->write(resp.toUtf8());
            }
        }
    } else {
        // SmartCAT framing: semicolon-delimited
        int pos;
        while ((pos = m_ptyBuffer.indexOf(';')) >= 0) {
            QString cmd = QString::fromUtf8(m_ptyBuffer.left(pos)).trimmed();
            m_ptyBuffer.remove(0, pos + 1);
            if (cmd.isEmpty()) continue;

            if (m_ptyCatProtocol) {
                QString resp = m_ptyCatProtocol->processCommand(cmd);
                if (!resp.isEmpty())
                    m_pty->write(resp.toUtf8());
            }
        }
    }
}

} // namespace AetherSDR
