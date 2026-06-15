// AgwpeServer.h
//
// AGWPE (AGW Packet Engine) monitoring server — AetherSDR
//
// Derived from Dire Wolf by John Langner WB2OSZ
// Copyright (C) 2011-2020 John Langner WB2OSZ
// Dire Wolf: GPL-2.0-or-later — https://github.com/wb2osz/direwolf
// AetherSDR: GPL-3.0-or-later — compatible via GPL-2.0-or-later upgrade path
//
// Protocol reference: src/server.c in Dire Wolf 1.7
//
// Monitoring-only subset: R, G, g, k, K, m, X, x, P.
// Connected mode (C/D/d/Y/y) is not implemented — requires AX.25 L2.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace AetherSDR {

// A monitoring-only AGWPE TCP server.  Applications such as Xastir and YAAC
// connect on TCP (default port 8000) and receive decoded AX.25 frames in the
// AGW raw-frame ('K') format.  Frames a client transmits via 'K' are forwarded
// to the air via ax25FrameFromClient.
//
// Multiple simultaneous clients are supported.  Each client independently
// enables raw-frame reception with the 'k' toggle.
class AgwpeServer : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort       = 8000;
    static constexpr int     kMaxClients        = 8;
    static constexpr int     kIdleTimeoutMs     = 5 * 60 * 1000;
    static constexpr int     kSweepIntervalMs   = 30 * 1000;
    static constexpr int     kMaxWriteBacklog   = 256 * 1024;

    explicit AgwpeServer(QObject* parent = nullptr);
    ~AgwpeServer() override;

    bool     start(quint16 port);
    void     stop();
    bool     isListening()  const;
    quint16  port()         const { return m_port; }
    int      clientCount()  const { return m_clients.size(); }
    QString  lastError()    const { return m_lastError; }
    void     setMaxClients(int n) { m_maxClients = n; }

    quint64  framesToClients()   const { return m_framesToClients; }
    quint64  framesFromClients() const { return m_framesFromClients; }

public slots:
    // RX path: broadcast a decoded AX.25 frame (no FCS) to all clients that
    // have enabled raw reception via 'k'.  FCS is recomputed before sending.
    void broadcastAx25Frame(const QByteArray& ax25NoFcs);

signals:
    // TX path: a client sent a 'K' frame; payload is the AX.25 frame (no FCS).
    void ax25FrameFromClient(const QByteArray& ax25NoFcs);

    void listeningChanged(bool listening);
    void clientCountChanged(int count);
    void activity(const QString& message);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onSweepTimer();

private:
    struct Client {
        QString       peer;
        QElapsedTimer lastActivity;
        QByteArray    buffer;
        bool          rawEnabled{false};
        bool          monitorEnabled{false};
    };

    void processMessage(QTcpSocket* s, Client& c,
                        char kind, quint8 port, quint8 pid,
                        const char* from, const char* to,
                        const QByteArray& data);
    void sendRaw(QTcpSocket* s, char kind, quint8 port, quint8 pid,
                 const char* from, const char* to, const QByteArray& data);
    void closeClient(QTcpSocket* s, const QString& reason);
    void emitClientCount();

    // AX.25 address extraction (for 'K' header fields).
    static QByteArray parseCallsign(const QByteArray& frame, int byteOffset);
    // CRC-16-CCITT for AX.25 FCS (same as HdlcCodec).
    static quint16    computeFcs(const QByteArray& data);

    QTcpServer*                m_server{nullptr};
    QHash<QTcpSocket*, Client> m_clients;
    QTimer*                    m_sweepTimer{nullptr};
    quint16                    m_port{0};
    int                        m_maxClients{kMaxClients};
    QString                    m_lastError;
    quint64                    m_framesToClients{0};
    quint64                    m_framesFromClients{0};
};

} // namespace AetherSDR
