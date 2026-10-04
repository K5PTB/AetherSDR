#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace AetherSDR {

// Downloads SmartSDR installer, verifies integrity, extracts .ssdr firmware
// files, and stages them for upload.
//
// Workflow:
//   1. checkForUpdate()  — compare radio version vs latest available
//   2. downloadAndStage() — download installer, verify MD5, extract .ssdr
//   3. stagedFilePath()   — path to extracted .ssdr ready for upload

class FirmwareStager : public QObject {
    Q_OBJECT
public:
    explicit FirmwareStager(QObject* parent = nullptr);

    // Fetch the latest SmartSDR version FlexRadio publishes, and judge nothing.
    // Answers `latestVersionKnown` or `latestVersionUnavailable` exactly once.
    //
    // Separate from checkForUpdate() because the two have different questions:
    // this one asks "what is published?", which needs no radio and is safe to
    // ask before one is connected. checkForUpdate() asks "is THIS radio behind?"
    // and is built on top of it.
    void fetchLatestVersion();

    // Check FlexRadio website for latest version
    void checkForUpdate(const QString& currentVersion);

    // The highest SmartSDR version named on a FlexRadio software page, or empty
    // when the page names none. Exposed for tests: the page is the untrusted
    // input this class exists to read (Principle VII), and its parse is the part
    // worth pinning without a network.
    static QString parseLatestVersion(const QString& html);

    // Download installer, verify, extract .ssdr for the given model family
    // modelFamily: "6x00" or "9600"
    void downloadAndStage(const QString& version, const QString& modelFamily);

    // Stage firmware from an installer file the user has already downloaded.
    // Accepts .msi (v4.2+), .exe (v4.1.x and earlier), or .ssdr (no extraction
    // needed; passed straight through to staging). Version is parsed from the
    // filename when present, otherwise stays empty until the radio confirms.
    void stageFromLocalFile(const QString& installerPath, const QString& modelFamily);

    // Cancel in-progress download
    void cancel();

    // Path to the staged .ssdr file (empty if not staged)
    QString stagedFilePath() const { return m_stagedPath; }
    QString stagedVersion()  const { return m_stagedVersion; }
    bool    isStaged()       const { return !m_stagedPath.isEmpty(); }

    // Map radio model string to firmware model family
    static QString modelToFamily(const QString& model);

    // Staging directory
    static QString stagingDir();

signals:
    // Step 0: what does FlexRadio publish? (fetchLatestVersion)
    void latestVersionKnown(const QString& latestVersion);
    void latestVersionUnavailable(const QString& reason);

    // Step 1: version check
    void updateCheckComplete(const QString& latestVersion, bool updateAvailable);
    void updateCheckFailed(const QString& error);

    // Steps 2-4: download, verify, extract
    void stageProgress(int percent, const QString& status);
    void stageComplete(const QString& ssdrPath, const QString& version);
    void stageFailed(const QString& error);

private:
    void onInstallerDownloadProgress(qint64 received, qint64 total);
    void onInstallerDownloadFinished();
    void verifyAndExtract();

    // Format-specific extractors. Both produce a .ssdr file at outPath and
    // emit progress/failed signals on the way.
    bool extractFromInnoSetup(const QByteArray& data, const QString& outPath);
    bool extractFromMsi(const QString& msiPath, const QString& outPath);

    // Returns true if the version uses the WiX MSI installer (v4.2+) instead
    // of the older InnoSetup .exe.
    static bool versionUsesMsi(const QString& version);

    // One GET of the software page, one parse, one answer. Both public entry
    // points wrap this so the page is read and validated in exactly one place.
    // `done` receives the parsed version, or an empty string plus a reason.
    void requestLatestVersion(std::function<void(const QString& version,
                                                 const QString& error)> done);

    // Refuse a software page larger than this rather than buffering whatever
    // the far end decides to send. The real page is ~290 KB.
    static constexpr qint64 kMaxSoftwarePageBytes = 8 * 1024 * 1024;

    QNetworkAccessManager m_nam;
    QNetworkReply*  m_downloadReply{nullptr};
    QString         m_installerPath;
    QString         m_expectedMd5;
    QString         m_modelFamily;
    QString         m_targetVersion;
    QString         m_stagedPath;
    QString         m_stagedVersion;
    bool            m_cancelled{false};

    // v4.1.x and earlier: InnoSetup self-extracting .exe.
    // v4.2+: WiX 6 MSI (OLE Compound File with embedded LZX-compressed CABs).
    static constexpr const char* INSTALLER_URL_FMT_EXE =
        "https://smartsdr.flexradio.com/SmartSDR_v%1_Installer.exe";
    static constexpr const char* INSTALLER_URL_FMT_MSI =
        "https://smartsdr.flexradio.com/SmartSDR_v%1_x64.msi";
    static constexpr const char* MD5_URL_FMT =
        "https://edge.flexradio.com/www/offload/20251215133656/"
        "SmartSDR-v%1-Installer-MD5-Hash-File.txt";
    static constexpr const char* SOFTWARE_PAGE =
        "https://www.flexradio.com/software/";
};

} // namespace AetherSDR
