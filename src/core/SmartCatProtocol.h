#pragma once

#include <QString>
#include <QList>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Pure request/response handler for the SmartSDR CAT (Kenwood) protocol.
// No I/O — call processCommand() with one semicolon-delimited command (the
// semicolon itself is NOT included). Returns the response to send, or an
// empty string for set commands that produce no reply.
//
// Used by both SmartCatSession (TCP) and CatPort's PTY handler.
// AI mode (async pushes) is NOT handled here; that is a session-level concern.
class SmartCatProtocol {
public:
    explicit SmartCatProtocol(RadioModel* model,
                              int vfoA = 0, int vfoB = -1,
                              bool flexExtensions = true);

    // Closes a split TX slice we created so a client disconnect (session/PTY
    // teardown) never leaves an orphan slice on the radio.
    ~SmartCatProtocol();

    QString processCommand(const QString& cmd);

    void setVfoA(int idx)            { m_vfoA = idx; }
    void setVfoB(int idx)            { m_vfoB = idx; }
    int  vfoA() const                { return m_vfoA; }
    int  vfoB() const                { return m_vfoB; }

    void setFlexExtensions(bool on)  { m_flexExtensions = on; }
    bool flexExtensions() const      { return m_flexExtensions; }

    // AI state: set by the session when AI enable/disable commands arrive.
    // processCommand() reads this for AI query responses (AI; / ZZAI;).
    bool aiEnabled() const           { return m_aiEnabled; }
    void setAiEnabled(bool on)       { m_aiEnabled = on; }

    // PTT safety: release transmit if this protocol instance asserted it.
    // Call from the session's onDisconnected() / dtor so an abrupt client
    // drop does not leave the radio keyed.  No-op if we never asserted PTT.
    void releasePtt();

    // Public helpers used by SmartCatSession's AI push
    static QString freqField(double mhz);
    static QString modeToKenwood(const QString& ssdrMode);
    static QString modeToZZ(const QString& ssdrMode);

private:
    // ── Frequency / Mode ────────────────────────────────────────────────────
    QString cmdFA(const QString& arg);
    QString cmdFB(const QString& arg);
    QString cmdMD(const QString& arg);
    QString cmdZZMD(const QString& arg);
    QString cmdZZME(const QString& arg);
    QString cmdIF();
    QString cmdZZIF();
    QString cmdFT(const QString& arg);
    QString cmdZZSW(const QString& arg);
    QString cmdFR(const QString& arg);
    QString cmdTX(const QString& arg);
    QString cmdRX();
    QString cmdID();
    QString cmdPS();
    QString cmdSM(const QString& arg);
    QString cmdZZSM(const QString& arg);

    // ── Audio gain / pan / mute ─────────────────────────────────────────────
    QString cmdAG(const QString& arg);
    QString cmdZZAG(const QString& arg);
    QString cmdZZLE(const QString& arg);
    QString cmdZZLB(const QString& arg);
    QString cmdZZLF(const QString& arg);
    QString cmdZZMA(const QString& arg);
    QString cmdZZMB(const QString& arg);

    // ── AGC ─────────────────────────────────────────────────────────────────
    QString cmdGT(const QString& arg);
    QString cmdZZGT(const QString& arg);
    QString cmdZZAR(const QString& arg);
    QString cmdZZAS(const QString& arg);

    // ── RF Power / Mic Gain ─────────────────────────────────────────────────
    QString cmdPC(const QString& arg);
    QString cmdZZPC(const QString& arg);
    QString cmdZZMG(const QString& arg);

    // ── RIT ─────────────────────────────────────────────────────────────────
    QString cmdRG(const QString& arg);
    QString cmdZZRG(const QString& arg);
    QString cmdRC();
    QString cmdZZRC();
    QString cmdRD(const QString& arg);
    QString cmdZZRD(const QString& arg);
    QString cmdRU(const QString& arg);
    QString cmdZZRU(const QString& arg);
    QString cmdRT(const QString& arg);
    QString cmdZZRT(const QString& arg);
    QString cmdZZRW(const QString& arg);
    QString cmdZZRY(const QString& arg);

    // ── XIT ─────────────────────────────────────────────────────────────────
    QString cmdXT(const QString& arg);
    QString cmdZZXG(const QString& arg);
    QString cmdZZXC();
    QString cmdZZXS(const QString& arg);

    // ── CW ──────────────────────────────────────────────────────────────────
    QString cmdKS(const QString& arg);
    QString cmdPT(const QString& arg);
    QString cmdKY(const QString& arg);

    // ── Noise Blanker / Noise Reduction ─────────────────────────────────────
    QString cmdNB(const QString& arg);
    QString cmdNL(const QString& arg);
    QString cmdNR(const QString& arg);
    QString cmdNT(const QString& arg);
    QString cmdRL(const QString& arg);
    QString cmdZZNL(const QString& arg);
    QString cmdZZNR(const QString& arg);

    // ── DSP Filter (SL / SH / FW / ZZFI / ZZFJ) ────────────────────────────
    QString cmdSL(const QString& arg);
    QString cmdSH(const QString& arg);
    QString cmdFW(const QString& arg);
    QString cmdZZFI(const QString& arg);
    QString cmdZZFJ(const QString& arg);

    // ── Squelch ──────────────────────────────────────────────────────────────
    QString cmdSQ(const QString& arg);

    // ── RF / Mic / Attenuator / Preamp ───────────────────────────────────────
    QString cmdMG(const QString& arg);
    QString cmdRA(const QString& arg);
    QString cmdPA(const QString& arg);

    // ── Meter / Status stubs ─────────────────────────────────────────────────
    QString cmdRM(const QString& arg);
    QString cmdLK(const QString& arg);
    QString cmdTY(const QString& arg);
    QString cmdBY(const QString& arg);

    // ── VFO step ─────────────────────────────────────────────────────────────
    QString cmdUP(const QString& arg);
    QString cmdDN(const QString& arg);

    // ── Opposite IF (VFO B status) ───────────────────────────────────────────
    QString cmdOI();

    // ── Misc ─────────────────────────────────────────────────────────────────
    QString cmdZZBI(const QString& arg);
    QString cmdZZDE(const QString& arg);
    QString cmdZZFR(const QString& arg);

    QString processCommandImpl(const QString& cmd);

    SliceModel* sliceA() const;
    SliceModel* sliceB() const;

    // ── Split (two-mechanism, mirrors SmartSDR-for-Windows) ──────────────────
    // Enable: use the operator-configured VFO B slice if present, else create a
    // dedicated TX slice when there is room, else fall back to an XIT offset on
    // slice A. Disable: tear the mechanism down (close a slice we created,
    // restore TX to slice A, or clear the XIT offset).
    QString enableSplit();
    QString disableSplit();
    // Tear the split mechanism down: close a slice we created (restoring TX to
    // slice A) or clear the XIT offset, then reset all split state. Shared by
    // disableSplit() (ZZSW0/FT0) and the destructor (client disconnect).
    void    teardownSplit();
    // Promote a freshly-created split slice (addSlice is async) to TX once it
    // appears, applying any TX freq/mode stashed while we waited.
    void    tryPromoteSplitSlice();
    // The effective VFO-B / split TX slice: the promoted split slice if any,
    // else the operator-configured VFO B. Promotes a pending slice first.
    SliceModel* vfoBSlice();
    SliceModel* sliceById(int id) const;

    static QString kenwoodToSSDR(QChar c);
    static QString zzToSSDR(const QString& two);

    RadioModel* m_model;
    int         m_vfoA{0};
    int         m_vfoB{-1};
    bool        m_flexExtensions{true};
    bool        m_aiEnabled{false};
    bool        m_splitEnabled{false};
    bool        m_rxVfoB{false};   // false = VFO A is the RX VFO (TS-2000 FR selector; no A/B swap)
    bool        m_pttAssertedByMe{false};

    // ── Split TX state ───────────────────────────────────────────────────────
    bool        m_pendingSplitSlice{false};   // addSlice() issued, slice not yet visible
    int         m_splitTxSliceId{-1};         // id of the dedicated split TX slice (-1 = none)
    bool        m_weCreatedSplitSlice{false}; // we created it → close it at split-disable
    bool        m_removeCreatedSliceWhenItAppears{false}; // split was disabled while our
                                              // addSlice() was still in flight. Don't abandon the
                                              // create: remove the slice as soon as it materializes
                                              // (next split command, or the destructor), else it
                                              // surfaces unowned ~hundreds of ms later as an orphan.
    bool        m_xitSplit{false};            // XIT-offset fallback active (no room for a slice)
    double      m_pendingSplitFreqMhz{0.0};   // TX freq stashed until the split slice appears
    QString     m_pendingSplitMode;           // TX mode stashed until the split slice appears
    QList<const SliceModel*> m_preSplitSlices; // slice OBJECTS present when addSlice() was issued.
                                              // Promotion adopts the slice whose POINTER is not here.
                                              // Pointers (not ids) because the radio reuses a freed
                                              // slice id: on a rapid disable→enable the new slice can
                                              // take the just-removed id, and an id snapshot would skip
                                              // it as "pre-existing" → never promoted/removed → orphan.
};

} // namespace AetherSDR
