// agwpe_server_test.cpp — self-contained unit test for AgwpeServer.
//
// Starts an AgwpeServer in-process, connects raw TCP sockets, and
// verifies the monitoring-only protocol frames (R, G, g, k, K broadcast,
// K transmit, X).  No running AetherSDR instance required.

#include "AgwpeServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpSocket>

#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace AetherSDR;

// ── Minimal test harness ─────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void check(const char* label, bool ok, const char* got = "")
{
    if (ok) {
        std::printf("  PASS  %s\n", label);
        ++g_pass;
    } else {
        std::printf("  FAIL  %s  (got: %s)\n", label, got);
        ++g_fail;
    }
}

// ── AGWPE wire helpers ───────────────────────────────────────────────────────

static constexpr int kHdrSize = 36;

// Build a minimal AGWPE header (no data, all callsigns empty).
static QByteArray makeHeader(char kind, quint8 port = 0, quint8 pid = 0,
                             qint32 dataLen = 0)
{
    QByteArray h(kHdrSize, '\0');
    h[0] = static_cast<char>(port);
    h[4] = kind;
    h[6] = static_cast<char>(pid);
    h[28] = static_cast<char>((dataLen >>  0) & 0xFF);
    h[29] = static_cast<char>((dataLen >>  8) & 0xFF);
    h[30] = static_cast<char>((dataLen >> 16) & 0xFF);
    h[31] = static_cast<char>((dataLen >> 24) & 0xFF);
    return h;
}

// Read exactly n bytes from socket (waits up to 2 s).
// Uses processEvents(AllEvents, 50) instead of waitForReadyRead() so that
// the global Qt event dispatcher drives ALL sockets (including the server-side
// socket that needs to receive the request and send the response) rather than
// only polling one fd via select().
static QByteArray readExact(QTcpSocket& sock, int n)
{
    QByteArray buf;
    QElapsedTimer t;
    t.start();
    while (buf.size() < n && t.elapsed() < 2000) {
        if (sock.bytesAvailable() > 0)
            buf.append(sock.read(n - buf.size()));
        else
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return buf;
}

// Parse the dataLen field from a 36-byte header buffer.
static qint32 parseDataLen(const QByteArray& hdr)
{
    if (hdr.size() < kHdrSize) return -1;
    return  (static_cast<quint8>(hdr[28])      ) |
            (static_cast<quint8>(hdr[29]) <<  8) |
            (static_cast<quint8>(hdr[30]) << 16) |
            (static_cast<quint8>(hdr[31]) << 24);
}

// Read one complete AGWPE frame (header + data) from socket.
static QPair<QByteArray, QByteArray> readFrame(QTcpSocket& sock)
{
    const QByteArray hdr = readExact(sock, kHdrSize);
    if (hdr.size() < kHdrSize)
        return {};
    const qint32 dl = parseDataLen(hdr);
    if (dl < 0 || dl > 65536)
        return {};
    const QByteArray data = dl > 0 ? readExact(sock, dl) : QByteArray{};
    return {hdr, data};
}

// ── CRC-16-CCITT (AX.25 FCS) for golden-value tests ─────────────────────────

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

static quint16 fcs(const QByteArray& data)
{
    quint16 crc = 0xFFFF;
    for (unsigned char b : data)
        crc = static_cast<quint16>((crc >> 8) ^ kCrcTable[(crc ^ b) & 0xFF]);
    return crc ^ 0xFFFF;
}

// ── Helper: build a minimal AX.25 UI frame (no info field) ───────────────────
//
// AX.25 address bytes are left-shifted ASCII with SSID in byte 6.
// Last address byte has bit 0 set (H-bit / end-of-address).

static QByteArray buildAx25Addr(const char* call, int ssid, bool last)
{
    QByteArray a(7, '\0');
    for (int i = 0; i < 6; ++i)
        a[i] = (call[i] ? call[i] : ' ') << 1;
    a[6] = static_cast<char>(((ssid & 0x0F) << 1) | (last ? 0x01 : 0x00) | 0x60);
    return a;
}

// Build a complete (but empty) AX.25 UI frame: dst + src + control(0x03) + PID(0xF0).
static QByteArray buildAx25Frame(const char* dst, const char* src)
{
    QByteArray f;
    f.append(buildAx25Addr(dst, 0, false));  // destination
    f.append(buildAx25Addr(src, 1, true));   // source, H-bit set, SSID=1
    f.append(static_cast<char>(0x03));       // unnumbered information
    f.append(static_cast<char>(0xF0));       // PID: no layer 3
    return f;
}

// ── Tests ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    AgwpeServer srv;
    // Pick an ephemeral port; 0 means OS assigns one.
    const bool started = srv.start(0);
    check("server starts on ephemeral port", started);
    if (!started) {
        std::printf("FATAL: server failed to start: %s\n",
                    srv.lastError().toUtf8().constData());
        return 1;
    }
    const quint16 port = srv.port();
    std::printf("  INFO  server listening on port %u\n", (unsigned)port);

    // ── T1: version handshake ('R') ──────────────────────────────────────────
    {
        QTcpSocket c1;
        c1.connectToHost(QStringLiteral("127.0.0.1"), port);
        check("T1 client connects", c1.waitForConnected(1000));
        QCoreApplication::processEvents(); // deliver server's newConnection before writing

        c1.write(makeHeader('R'));
        c1.flush();
        QCoreApplication::processEvents();

        auto [hdr, data] = readFrame(c1);
        check("T1 'R' response arrives",              hdr.size() == kHdrSize);
        check("T1 'R' kind byte is 'R'",              hdr.size() >= 5 && hdr[4] == 'R');
        check("T1 'R' dataLen is 8",                  parseDataLen(hdr) == 8);
        check("T1 'R' major version is 2005",
              data.size() >= 2 &&
              (quint16)((quint8)data[0] | ((quint8)data[1] << 8)) == 2005);
        check("T1 'R' minor version is 127",
              data.size() >= 6 &&
              (quint16)((quint8)data[4] | ((quint8)data[5] << 8)) == 127);

        c1.disconnectFromHost();
    }

    // ── T2: port info ('G') ──────────────────────────────────────────────────
    {
        QTcpSocket c2;
        c2.connectToHost(QStringLiteral("127.0.0.1"), port);
        c2.waitForConnected(1000);
        QCoreApplication::processEvents();

        c2.write(makeHeader('G'));
        c2.flush();
        QCoreApplication::processEvents();

        auto [hdr, data] = readFrame(c2);
        check("T2 'G' response arrives",  hdr.size() == kHdrSize);
        check("T2 'G' kind byte is 'G'",  hdr.size() >= 5 && hdr[4] == 'G');
        check("T2 'G' starts with '1;'",
              data.startsWith("1;"));
        check("T2 'G' contains 'AetherSDR'",
              data.contains("AetherSDR"));

        c2.disconnectFromHost();
    }

    // ── T3: port capabilities ('g') ─────────────────────────────────────────
    {
        QTcpSocket c3;
        c3.connectToHost(QStringLiteral("127.0.0.1"), port);
        c3.waitForConnected(1000);
        QCoreApplication::processEvents();

        c3.write(makeHeader('g'));
        c3.flush();
        QCoreApplication::processEvents();

        auto [hdr, data] = readFrame(c3);
        check("T3 'g' response arrives",   hdr.size() == kHdrSize);
        check("T3 'g' kind byte is 'g'",   hdr.size() >= 5 && hdr[4] == 'g');
        check("T3 'g' dataLen is 12",      parseDataLen(hdr) == 12);
        // MaxFrame is at byte offset 6 of capabilities data.
        check("T3 'g' MaxFrame == 7",      data.size() >= 7 && (quint8)data[6] == 7);

        c3.disconnectFromHost();
    }

    // ── T4: register callsign ('X') ─────────────────────────────────────────
    {
        QTcpSocket c4;
        c4.connectToHost(QStringLiteral("127.0.0.1"), port);
        c4.waitForConnected(1000);
        QCoreApplication::processEvents();

        QByteArray xhdr = makeHeader('X', 0, 0, 0);
        memcpy(xhdr.data() + 8, "K5PTB    ", 9);   // callFrom
        c4.write(xhdr);
        c4.flush();
        QCoreApplication::processEvents();

        auto [hdr, data] = readFrame(c4);
        check("T4 'X' response arrives",    hdr.size() == kHdrSize);
        check("T4 'X' kind byte is 'X'",    hdr.size() >= 5 && hdr[4] == 'X');
        check("T4 'X' dataLen is 1",        parseDataLen(hdr) == 1);
        check("T4 'X' success byte is 0x01", data.size() == 1 && (quint8)data[0] == 0x01);

        c4.disconnectFromHost();
    }

    // ── T5: raw-frame broadcast ('k' then 'K') ───────────────────────────────
    //
    // One client enables raw frames ('k'), then another client's decoded frame
    // is broadcast via broadcastAx25Frame().  Verify the 'K' frame arrives at
    // the raw-enabled client with the correct FCS bytes.
    {
        QTcpSocket receiver, sender_sock;
        receiver.connectToHost(QStringLiteral("127.0.0.1"), port);
        receiver.waitForConnected(1000);
        sender_sock.connectToHost(QStringLiteral("127.0.0.1"), port);
        sender_sock.waitForConnected(1000);

        // Enable raw reception on the receiver.
        receiver.write(makeHeader('k'));
        receiver.flush();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        receiver.waitForReadyRead(100);  // 'k' has no response; just drain any pending data

        // Build a minimal AX.25 UI frame: APRS>K5PTB-1 (no info payload).
        const QByteArray frame = buildAx25Frame("APRS  ", "K5PTB ");
        const quint16 expectedFcs = fcs(frame);

        // Trigger broadcast from the server side.
        srv.broadcastAx25Frame(frame);
        QCoreApplication::processEvents();

        auto [hdr, data] = readFrame(receiver);
        check("T5 broadcast 'K' frame arrives at raw-enabled client",
              hdr.size() == kHdrSize);
        check("T5 'K' kind byte is 'K'",
              hdr.size() >= 5 && hdr[4] == 'K');
        // dataLen = 1 (port nibble) + frame.size() + 2 (FCS)
        const int expectedLen = 1 + frame.size() + 2;
        check("T5 'K' dataLen = 1 + frameLen + 2",
              parseDataLen(hdr) == expectedLen);
        // data[0] = port nibble (0x00), data[1..frame.size()] = frame, last 2 = FCS
        check("T5 'K' port nibble byte is 0x00",
              data.size() >= 1 && (quint8)data[0] == 0x00);
        check("T5 'K' frame bytes match original",
              data.size() >= 1 + frame.size() &&
              data.mid(1, frame.size()) == frame);
        const bool fcsOk = data.size() >= expectedLen &&
            (quint8)data[1 + frame.size()]     == (expectedFcs & 0xFF) &&
            (quint8)data[1 + frame.size() + 1] == ((expectedFcs >> 8) & 0xFF);
        check("T5 'K' FCS bytes match CRC-16-CCITT of frame", fcsOk);

        // Confirm sender_sock (no 'k') receives nothing.
        sender_sock.waitForReadyRead(100);
        check("T5 non-raw client receives no 'K' frame",
              sender_sock.bytesAvailable() == 0);

        receiver.disconnectFromHost();
        sender_sock.disconnectFromHost();
    }

    // ── T6: transmit raw frame from client ('K') ─────────────────────────────
    {
        const QByteArray frame = buildAx25Frame("NOCALL", "K5PTB ");
        const quint16 txFcs = fcs(frame);

        // Build payload: [port_nibble] + frame + FCS
        QByteArray payload;
        payload.append('\x00');
        payload.append(frame);
        payload.append(static_cast<char>(txFcs & 0xFF));
        payload.append(static_cast<char>((txFcs >> 8) & 0xFF));

        QTcpSocket c6;
        c6.connectToHost(QStringLiteral("127.0.0.1"), port);
        c6.waitForConnected(1000);

        QByteArray received;
        QObject::connect(&srv, &AgwpeServer::ax25FrameFromClient,
                         [&received](const QByteArray& f) { received = f; });

        c6.write(makeHeader('K', 0, 0xF0, static_cast<qint32>(payload.size())));
        c6.write(payload);
        c6.flush();
        QCoreApplication::processEvents();
        c6.waitForBytesWritten(500);
        QCoreApplication::processEvents();

        check("T6 'K' TX fires ax25FrameFromClient",    !received.isEmpty());
        check("T6 'K' TX frame matches original (no FCS)", received == frame);

        c6.disconnectFromHost();
    }

    // ── T7: server clientCount signal ────────────────────────────────────────
    {
        int cnt = -1;
        QObject::connect(&srv, &AgwpeServer::clientCountChanged,
                         [&cnt](int n) { cnt = n; });

        QTcpSocket c7;
        c7.connectToHost(QStringLiteral("127.0.0.1"), port);
        c7.waitForConnected(1000);
        QCoreApplication::processEvents();
        check("T7 clientCountChanged emitted on connect", cnt >= 0);

        c7.disconnectFromHost();
        c7.waitForDisconnected(500);
        QCoreApplication::processEvents();
        check("T7 server stops on stop()", (srv.stop(), !srv.isListening()));
    }

    // ── Results ──────────────────────────────────────────────────────────────

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
