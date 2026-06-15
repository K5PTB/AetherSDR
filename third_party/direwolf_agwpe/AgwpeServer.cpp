// AgwpeServer.cpp
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later

#include "AgwpeServer.h"

#include <QHostAddress>
#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace AetherSDR {

static Q_LOGGING_CATEGORY(lcAgwpe, "aether.ax25.agwpe")

// ── AGWPE wire protocol ──────────────────────────────────────────────────────
//
// All multi-byte integers are little-endian.
//
// Frame layout (matches struct agwpe_s in Dire Wolf server.c):
//
//   Offset  Size  Field
//   0       1     portx       — radio port (0 = first)
//   1       3     reserved
//   4       1     dataKind    — frame type character ('R', 'G', 'g', 'K', …)
//   5       1     reserved
//   6       1     pid         — AX.25 PID (0xF0 for normal frames)
//   7       1     reserved
//   8       10    callFrom    — source callsign, null-padded
//   18      10    callTo      — destination callsign, null-padded
//   28      4     dataLen     — number of data bytes following (LE int32)
//   32      4     userReserved
//   ──────
//   36      total header size

static constexpr int kHeaderSize = 36;

// Pack a header into a 36-byte QByteArray.
static QByteArray packHeader(char kind, quint8 port, quint8 pid,
                             const char* from, const char* to, qint32 dataLen)
{
    QByteArray h(kHeaderSize, '\0');
    h[0]  = static_cast<char>(port);
    h[4]  = kind;
    h[6]  = static_cast<char>(pid);
    if (from) { const int n = qMin(9, (int)strlen(from)); memcpy(h.data()+8,  from, n); }
    if (to)   { const int n = qMin(9, (int)strlen(to));   memcpy(h.data()+18, to,   n); }
    // dataLen as LE int32
    h[28] = static_cast<char>((dataLen >>  0) & 0xFF);
    h[29] = static_cast<char>((dataLen >>  8) & 0xFF);
    h[30] = static_cast<char>((dataLen >> 16) & 0xFF);
    h[31] = static_cast<char>((dataLen >> 24) & 0xFF);
    return h;
}

// ── CRC-16-CCITT (AX.25 FCS) ────────────────────────────────────────────────
//
// Polynomial 0x8408 (bit-reversed 0x1021), init=0xFFFF, final XOR=0xFFFF.
// Identical to Dire Wolf fcs_calc.c and AetherSDR HdlcCodec.

static constexpr quint16 kCrcTable[256] = {
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78,
};

quint16 AgwpeServer::computeFcs(const QByteArray& data)
{
    quint16 crc = 0xFFFF;
    for (unsigned char b : data)
        crc = static_cast<quint16>((crc >> 8) ^ kCrcTable[(crc ^ b) & 0xFF]);
    return crc ^ 0xFFFF;
}

// Extract a printable AX.25 callsign from the address field starting at
// byteOffset in frame.  Each address byte is right-shifted by 1 to recover
// the ASCII character.  The SSID nibble (bits 4-1 of the 7th byte) is
// appended as "-N" if non-zero.  Returns empty if the frame is too short.
QByteArray AgwpeServer::parseCallsign(const QByteArray& frame, int byteOffset)
{
    if (frame.size() < byteOffset + 7)
        return {};
    QByteArray cs;
    for (int i = 0; i < 6; ++i) {
        const char c = static_cast<char>(
            (static_cast<quint8>(frame[byteOffset + i]) >> 1) & 0x7F);
        if (c != ' ')
            cs.append(c);
    }
    const int ssid = (static_cast<quint8>(frame[byteOffset + 6]) >> 1) & 0x0F;
    if (ssid)
        cs.append('-').append(QByteArray::number(ssid));
    return cs;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

AgwpeServer::AgwpeServer(QObject* parent)
    : QObject(parent)
{}

AgwpeServer::~AgwpeServer()
{
    stop();
}

bool AgwpeServer::start(quint16 port)
{
    stop();

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &AgwpeServer::onNewConnection);

    if (!m_server->listen(QHostAddress::Any, port)) {
        m_lastError = m_server->errorString();
        qCWarning(lcAgwpe).noquote()
            << QStringLiteral("AGWPE failed to listen on port %1: %2").arg(port).arg(m_lastError);
        emit activity(QStringLiteral("AGWPE could not bind port %1: %2").arg(port).arg(m_lastError));
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }

    m_port = m_server->serverPort();   // actual OS-assigned port (0 → ephemeral)
    m_lastError.clear();
    m_framesToClients = 0;
    m_framesFromClients = 0;

    m_sweepTimer = new QTimer(this);
    m_sweepTimer->setInterval(kSweepIntervalMs);
    connect(m_sweepTimer, &QTimer::timeout, this, &AgwpeServer::onSweepTimer);
    m_sweepTimer->start();

    qCInfo(lcAgwpe).noquote()
        << QStringLiteral("AGWPE monitoring server listening on TCP port %1 (all interfaces), maxClients=%2")
               .arg(port).arg(m_maxClients);
    emit activity(QStringLiteral("AGWPE monitoring server listening on TCP port %1.").arg(port));
    emit listeningChanged(true);
    return true;
}

void AgwpeServer::stop()
{
    if (m_sweepTimer) {
        m_sweepTimer->stop();
        m_sweepTimer->deleteLater();
        m_sweepTimer = nullptr;
    }

    const bool wasListening = m_server != nullptr;
    const int  hadClients   = m_clients.size();

    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        QTcpSocket* s = it.key();
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    m_clients.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (wasListening) {
        qCInfo(lcAgwpe).noquote()
            << QStringLiteral("AGWPE monitoring server stopped (closed %1 client(s)).").arg(hadClients);
        emit activity(QStringLiteral("AGWPE monitoring server stopped."));
        emit listeningChanged(false);
        emitClientCount();
    }
}

bool AgwpeServer::isListening() const
{
    return m_server && m_server->isListening();
}

// ── New connection ────────────────────────────────────────────────────────────

void AgwpeServer::onNewConnection()
{
    if (!m_server)
        return;

    while (QTcpSocket* s = m_server->nextPendingConnection()) {
        if (m_clients.size() >= m_maxClients) {
            const QString peer = QStringLiteral("%1:%2")
                .arg(s->peerAddress().toString()).arg(s->peerPort());
            qCWarning(lcAgwpe).noquote()
                << QStringLiteral("AGWPE refused %1: client limit (%2) reached")
                       .arg(peer).arg(m_maxClients);
            s->abort();
            s->deleteLater();
            continue;
        }

        s->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        Client client;
        client.peer = QStringLiteral("%1:%2")
            .arg(s->peerAddress().toString()).arg(s->peerPort());
        client.lastActivity.start();
        m_clients.insert(s, client);

        connect(s, &QTcpSocket::readyRead,    this, &AgwpeServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, &AgwpeServer::onDisconnected);

        qCInfo(lcAgwpe).noquote()
            << QStringLiteral("AGWPE client connected: %1 (now %2 client(s))")
                   .arg(client.peer).arg(m_clients.size());
        emit activity(QStringLiteral("AGWPE client connected: %1.").arg(client.peer));
        emitClientCount();
    }
}

// ── Incoming data ────────────────────────────────────────────────────────────

void AgwpeServer::onReadyRead()
{
    auto* s = qobject_cast<QTcpSocket*>(sender());
    if (!s)
        return;
    auto it = m_clients.find(s);
    if (it == m_clients.end())
        return;

    Client& c = it.value();
    c.lastActivity.restart();
    c.buffer.append(s->readAll());

    while (true) {
        // Need at least the 36-byte header.
        if (c.buffer.size() < kHeaderSize)
            break;

        // Parse header fields from the raw buffer.
        const char*  buf      = c.buffer.constData();
        const char   kind     = buf[4];
        const quint8 port     = static_cast<quint8>(buf[0]);
        const quint8 pid      = static_cast<quint8>(buf[6]);
        // callFrom at [8..17], callTo at [18..27]
        char from[11]{};  memcpy(from, buf + 8,  10);
        char to[11]{};    memcpy(to,   buf + 18, 10);
        // dataLen as LE int32 at [28]
        const qint32 dataLen =
            (static_cast<quint8>(buf[28])       ) |
            (static_cast<quint8>(buf[29]) <<  8 ) |
            (static_cast<quint8>(buf[30]) << 16 ) |
            (static_cast<quint8>(buf[31]) << 24 );

        if (dataLen < 0 || dataLen > 65536) {
            qCWarning(lcAgwpe).noquote()
                << QStringLiteral("AGWPE client %1: invalid dataLen %2 — closing")
                       .arg(c.peer).arg(dataLen);
            closeClient(s, QStringLiteral("invalid dataLen"));
            return;
        }

        // Wait until the full message (header + data) has arrived.
        if (c.buffer.size() < kHeaderSize + dataLen)
            break;

        const QByteArray data = c.buffer.mid(kHeaderSize, dataLen);
        c.buffer.remove(0, kHeaderSize + dataLen);

        processMessage(s, c, kind, port, pid, from, to, data);
    }
}

// ── Frame dispatcher ─────────────────────────────────────────────────────────

void AgwpeServer::processMessage(QTcpSocket* s, Client& c,
                                 char kind, quint8 port, quint8 pid,
                                 const char* from, const char* to,
                                 const QByteArray& data)
{
    Q_UNUSED(to)
    Q_UNUSED(pid)

    switch (kind) {

    case 'R': {                         // Version number request
        // Reply: header + major (LE int32) + minor (LE int32)
        // Direwolf uses 2005 / 127; wl2k-go reads each as LE uint16 pair.
        QByteArray payload(8, '\0');
        payload[0] = static_cast<char>(0xD5); payload[1] = 0x07;  // 2005 as LE int32
        payload[4] = 0x7F;                      // 127  LE int32 low word
        sendRaw(s, 'R', 0, 0, nullptr, nullptr, payload);
        break;
    }

    case 'G': {                         // Ask about radio ports
        // "count;Port1 description;Port2 description;…"
        const QByteArray info("1;AetherSDR VHF 1200;\0", 22);
        sendRaw(s, 'G', 0, 0, nullptr, nullptr, info);
        break;
    }

    case 'g': {                         // Port capabilities
        // 8 bytes (uint8 fields) + 4 bytes (LE int32 HowManyBytes) = 12 bytes.
        // Matches portCapabilities struct in wl2k-go agwpe/port.go.
        //   [0] on_air_baud_rate  0 = 1200
        //   [1] traffic_level     1
        //   [2] tx_delay          0x19
        //   [3] tx_tail           4
        //   [4] persist           0xC8
        //   [5] slot_time         4
        //   [6] maxframe          7   ← wl2k-go uses this for window sizing
        //   [7] active_conns      0
        //   [8-11] how_many_bytes 1 (LE int32)
        QByteArray caps(12, '\0');
        caps[0] = 0x00;   // 1200 baud
        caps[1] = 0x01;
        caps[2] = 0x19;
        caps[3] = 0x04;
        caps[4] = static_cast<char>(0xC8);
        caps[5] = 0x04;
        caps[6] = 0x07;   // MaxFrame
        caps[7] = 0x00;
        caps[8] = 0x01;   // how_many_bytes LE int32 = 1
        sendRaw(s, 'g', port, 0, nullptr, nullptr, caps);
        break;
    }

    case 'k':                           // Toggle raw-frame reception
        c.rawEnabled = !c.rawEnabled;
        qCDebug(lcAgwpe).noquote()
            << QStringLiteral("AGWPE %1: raw frames %2")
                   .arg(c.peer, c.rawEnabled ? "ENABLED" : "disabled");
        break;

    case 'm':                           // Toggle monitor frames (flag only)
        c.monitorEnabled = !c.monitorEnabled;
        break;

    case 'K': {                         // Transmit raw AX.25 frame from client
        // data[0] = port nibble (ignore); data[1..N-3] = AX.25; data[N-2..N-1] = FCS
        if (data.size() < 4)            // port byte + min 1 addr byte + 2 FCS bytes
            break;
        ++m_framesFromClients;
        const QByteArray frame = data.mid(1, data.size() - 3); // strip port byte + FCS
        qCDebug(lcAgwpe).noquote()
            << QStringLiteral("AGWPE TX from %1: %2 AX.25 bytes (frame #%3)")
                   .arg(c.peer).arg(frame.size()).arg(m_framesFromClients);
        emit ax25FrameFromClient(frame);
        break;
    }

    case 'X': {                         // Register callsign → success
        QByteArray ack(1, '\x01');
        sendRaw(s, 'X', port, 0, from, nullptr, ack);
        break;
    }

    case 'x':                           // Unregister callsign → no response
    case 'P':                           // Application login → ignore
        break;

    default:
        qCDebug(lcAgwpe).noquote()
            << QStringLiteral("AGWPE %1: unhandled frame kind '%2' (0x%3)")
                   .arg(c.peer).arg(kind).arg(static_cast<quint8>(kind), 2, 16, QLatin1Char('0'));
        break;
    }
}

// ── Broadcast received frame to all raw-enabled clients ──────────────────────

void AgwpeServer::broadcastAx25Frame(const QByteArray& ax25NoFcs)
{
    if (ax25NoFcs.isEmpty())
        return;

    // Extract source and destination callsigns from AX.25 address field.
    // AX.25: destination at byte 0, source at byte 7.
    const QByteArray dst = parseCallsign(ax25NoFcs, 0);
    const QByteArray src = parseCallsign(ax25NoFcs, 7);

    // Compute FCS and build the frame payload: [port_nibble] + frame + FCS.
    const quint16 fcs = computeFcs(ax25NoFcs);
    QByteArray payload;
    payload.reserve(1 + ax25NoFcs.size() + 2);
    payload.append('\x00');          // port nibble byte (port 0)
    payload.append(ax25NoFcs);
    payload.append(static_cast<char>(fcs & 0xFF));
    payload.append(static_cast<char>((fcs >> 8) & 0xFF));

    int delivered = 0;
    QVector<QTcpSocket*> slowConsumers;

    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        QTcpSocket* s = it.key();
        if (!it->rawEnabled || s->state() != QAbstractSocket::ConnectedState)
            continue;
        if (s->bytesToWrite() > kMaxWriteBacklog) {
            slowConsumers.append(s);
            continue;
        }
        sendRaw(s, 'K', 0, 0xF0,
                src.isEmpty()  ? nullptr : src.constData(),
                dst.isEmpty()  ? nullptr : dst.constData(),
                payload);
        ++delivered;
    }

    if (delivered > 0)
        ++m_framesToClients;

    qCDebug(lcAgwpe).noquote()
        << QStringLiteral("AGWPE RX broadcast: %1 AX.25 bytes to %2 raw client(s) (frame #%3)")
               .arg(ax25NoFcs.size()).arg(delivered).arg(m_framesToClients);

    for (QTcpSocket* s : slowConsumers)
        closeClient(s, QStringLiteral("write backlog exceeded"));
}

// ── Wire I/O ─────────────────────────────────────────────────────────────────

void AgwpeServer::sendRaw(QTcpSocket* s, char kind, quint8 port, quint8 pid,
                          const char* from, const char* to, const QByteArray& data)
{
    const QByteArray hdr = packHeader(kind, port, pid, from, to,
                                      static_cast<qint32>(data.size()));
    s->write(hdr);
    if (!data.isEmpty())
        s->write(data);
    s->flush();
}

// ── Sweep / teardown ─────────────────────────────────────────────────────────

void AgwpeServer::onSweepTimer()
{
    QVector<QTcpSocket*> stale;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->lastActivity.isValid() && it->lastActivity.elapsed() > kIdleTimeoutMs)
            stale.append(it.key());
    }
    for (QTcpSocket* s : stale)
        closeClient(s, QStringLiteral("idle timeout"));
}

void AgwpeServer::onDisconnected()
{
    auto* s = qobject_cast<QTcpSocket*>(sender());
    if (s)
        closeClient(s, QStringLiteral("disconnected"));
}

void AgwpeServer::closeClient(QTcpSocket* s, const QString& reason)
{
    auto it = m_clients.find(s);
    if (it == m_clients.end()) {
        s->deleteLater();
        return;
    }
    const QString peer = it->peer;
    m_clients.erase(it);

    s->disconnect(this);
    s->abort();
    s->deleteLater();

    qCInfo(lcAgwpe).noquote()
        << QStringLiteral("AGWPE client %1 closed: %2 (now %3 client(s))")
               .arg(peer, reason).arg(m_clients.size());
    emit activity(QStringLiteral("AGWPE client %1 closed: %2.").arg(peer, reason));
    emitClientCount();
}

void AgwpeServer::emitClientCount()
{
    emit clientCountChanged(m_clients.size());
}

} // namespace AetherSDR
