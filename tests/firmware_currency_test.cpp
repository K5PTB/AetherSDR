// Covers the status-bar firmware verdict end to end, minus the network: how the
// published software page is read, which radios the verdict applies to, which
// releases count as behind, and what the operator is told and offered.
//
// The gate is a DECLARED capability, never a family name (docs/HERMES.md
// §"For coding agents"), so "which radios" here means "which backends declared
// that their firmware versions are published".

#include "core/FirmwareCurrency.h"
#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/FirmwareStager.h"
#include "core/backends/flex/FlexBackend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>

#include <cstdio>
#include <memory>

namespace {

using AetherSDR::FirmwareStager;
using AetherSDR::FirmwareCurrency::Status;
using AetherSDR::FirmwareCurrency::compareReleases;
using AetherSDR::FirmwareCurrency::evaluate;
using AetherSDR::FirmwareCurrency::kPublishedVersionMaxAgeSecs;
using AetherSDR::FirmwareCurrency::publishedVersionIsStale;
using AetherSDR::FirmwareCurrency::releaseNotesUrl;
using AetherSDR::FirmwareCurrency::tooltip;
using AetherSDR::FirmwareCurrency::upgradeTargetFor;

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// What flexradio.com/software offered when these tests were written: several
// lines side by side, with THREE v4 releases downloadable at once (4.1.5,
// 4.2.18, 4.2.20). Spelled out rather than fetched — the tests must not need a
// network, and must not silently re-baseline when FlexRadio ships.
const QMap<int, QString> kPublished = {
    {2, QStringLiteral("2.10.1")},
    {3, QStringLiteral("3.10.15")},
    {4, QStringLiteral("4.2.20")},
};

Status verdict(const QString& reported) { return evaluate(kPublished, reported); }

QString newestInLine(const QString& html, int major)
{
    return FirmwareStager::parsePublishedReleases(html).value(major);
}

// THE FINDING THIS FEATURE WAS REBUILT AROUND (PR #6177, Jeremy's B1).
// A radio is judged against the newest release of ITS OWN LINE. Measuring every
// radio against the newest release overall tells a v3 operator they are behind
// 4.2.20 — a line they are not on — and links release notes they cannot use.
void checkTheVerdictIsScopedToTheRadiosOwnLine()
{
    // The two radios this was confirmed on.
    check(verdict(QStringLiteral("4.2.20.41343")) == Status::Current,
          "a radio on the newest v4 release is current");
    check(verdict(QStringLiteral("3.9.18.36988")) == Status::Outdated,
          "a radio on 3.9.18 is behind its own line's 3.10.15");

    // ...and the case that was wrong before: the newest release of an OLDER
    // line is CURRENT, not permanently out of date against v4.
    check(verdict(QStringLiteral("3.10.15")) == Status::Current,
          "a radio on the newest v3 release is current, not behind v4");
    check(verdict(QStringLiteral("2.10.1")) == Status::Current,
          "a radio on the newest v2 release is current, not behind v4");

    // Within a line it still nudges: three v4 releases are downloadable at once.
    check(verdict(QStringLiteral("4.1.5")) == Status::Outdated,
          "an older v4 release is behind the newest v4");
    check(verdict(QStringLiteral("4.2.18.41174")) == Status::Outdated,
          "4.2.18 is behind 4.2.20 — same line");

    // A radio on a line nobody publishes gets no verdict, rather than being
    // measured against someone else's line.
    check(verdict(QStringLiteral("1.12.1")) == Status::Unknown,
          "a radio on an unpublished line yields no verdict");

    // The PROTOCOL version, not a firmware version. A connect-by-IP session
    // briefly puts the `V` line into the model before `software_ver` corrects
    // it, and "1.4.0.0" was confirmed on the wire. Line scoping means it falls
    // in an unpublished line and draws plain, instead of flashing orange with a
    // 404 link (PR #6177 review, aethersdr-agent B3).
    //
    // CONTINGENT: this holds because FlexRadio publishes no v1 release today.
    // If one ever appears, the protocol version becomes judgeable again and the
    // real fix — judging only after software_ver arrives — is needed.
    check(verdict(QStringLiteral("1.4.0.0")) == Status::Unknown,
          "the protocol version falls in an unpublished line and is not judged");
}

// The link must point at the upgrade actually being recommended.
void checkTheUpgradeTargetIsTheOwnLineRelease()
{
    check(upgradeTargetFor(kPublished, QStringLiteral("3.9.18.36988"))
              == QStringLiteral("3.10.15"),
          "a v3 radio is pointed at 3.10.15, not 4.2.20");
    check(upgradeTargetFor(kPublished, QStringLiteral("4.1.5"))
              == QStringLiteral("4.2.20"),
          "a v4 radio is pointed at 4.2.20");
    check(upgradeTargetFor(kPublished, QStringLiteral("1.12.1")).isEmpty(),
          "an unpublished line offers no upgrade target");
    check(upgradeTargetFor({}, QStringLiteral("4.1.5")).isEmpty(),
          "nothing published means no upgrade target");
}

void checkTheBuildNumberIsIgnored()
{
    // No published source carries a build number, so two radios on the same
    // release must reach the same verdict whatever their build.
    check(verdict(QStringLiteral("4.2.20.99999")) == Status::Current,
          "a high build of the published release is current");
    check(verdict(QStringLiteral("4.2.20.1")) == Status::Current,
          "a low build of the published release is equally current");
    check(verdict(QStringLiteral("4.2.20")) == Status::Current,
          "the release with no build number at all is current");
}

void checkComparisonIsNumericNotLexicographic()
{
    // As text, "4.2.9" sorts AFTER "4.2.20" and "4.2.100" before it.
    check(verdict(QStringLiteral("4.2.9.99999")) == Status::Outdated,
          "4.2.9 is behind 4.2.20 despite sorting after it as text");
    check(verdict(QStringLiteral("4.2.100.1")) == Status::Current,
          "4.2.100 is ahead of 4.2.20 despite sorting before it as text");
    check(compareReleases(QStringLiteral("3.10.15"), QStringLiteral("3.9.19")) > 0,
          "compareReleases() orders 3.10.15 above 3.9.19");
    check(compareReleases(QStringLiteral("4.2.20.41343"), QStringLiteral("4.2.20")) == 0,
          "a build number does not make a release newer than itself");
}

void checkNewerThanPublishedIsNotOutdated()
{
    // The published answer is a web page read at connect and can lag a release.
    check(verdict(QStringLiteral("4.2.21.0")) == Status::Current,
          "a release newer than the published one is not flagged out of date");
}

void checkNothingPublishedMeansNoVerdict()
{
    check(evaluate({}, QStringLiteral("3.9.18")) == Status::Unknown,
          "no published releases yields no verdict, however old the radio is");
    check(evaluate({{4, QStringLiteral("not a version")}},
                   QStringLiteral("4.1.5")) == Status::Unknown,
          "an unparseable published release yields no verdict");
    // Guards the guard: the same radio IS judged once its line is published.
    check(verdict(QStringLiteral("3.9.18")) == Status::Outdated,
          "the same radio IS judged once its line is published");
}

void checkUnparseableRadioVersionsAreUnknown()
{
    check(verdict(QString()) == Status::Unknown,
          "a disconnected radio's cleared label yields no verdict");
    check(verdict(QStringLiteral("Gateware 75")) == Status::Unknown,
          "a label word in front of the number is not silently parsed");
    check(verdict(QStringLiteral("unknown")) == Status::Unknown,
          "a non-numeric version yields no verdict");
}

void checkTooltipsMatchTheState()
{
    check(tooltip(Status::Current) == QStringLiteral("Firmware up-to-date"),
          "the current-firmware tooltip reads as specified");
    check(tooltip(Status::Outdated)
              == QStringLiteral("Firmware is out of date, click to see release notes."),
          "the out-of-date tooltip reads as specified, including the invitation to click");
    check(tooltip(Status::Unknown).isEmpty(),
          "an unjudged radio clears the tooltip rather than keeping a stale one");
}

void checkWhenTheCachedAnswerIsLookedUpAgain()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString cached = QStringLiteral("cached");

    check(!publishedVersionIsStale(cached, now.addSecs(-60), now),
          "a minutes-old answer is reused");
    check(kPublishedVersionMaxAgeSecs == 24 * 60 * 60,
          "the lookup window is 24 hours");
    check(!publishedVersionIsStale(cached, now.addSecs(-23 * 60 * 60), now),
          "23 hours old is still reused");
    check(publishedVersionIsStale(cached, now.addSecs(-25 * 60 * 60), now),
          "25 hours old is looked up again");
    check(publishedVersionIsStale(QString(), now.addSecs(-60), now),
          "an empty cache means look it up, however recent the stamp");
    check(publishedVersionIsStale(cached, QDateTime(), now),
          "an unreadable stamp means look it up");
    // A stamp in the FUTURE would otherwise never expire.
    check(publishedVersionIsStale(cached, now.addSecs(60), now),
          "a stamp in the future is looked up again rather than trusted forever");
}

void checkTheBackendDeclaresTheRecordAndAsksNobodyAtConstruction()
{
    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    const auto source = backend->capabilities().firmwareUpdateSource;
    check(source.has_value(), "FlexBackend declares where its firmware is published");
    if (!source.has_value())
        return;

    check(source->publishedReleases.isEmpty(),
          "constructing a backend performs no lookup and publishes no releases");
    // Direct evidence, not an absence that proves nothing: the lookup is driven
    // by the wire's `connected` signal, and no radio is connected here.
    check(backend->findChild<AetherSDR::FirmwareStager*>() == nullptr,
          "constructing a backend builds no FirmwareStager, so nothing has asked");

    check(evaluate(source->publishedReleases, QStringLiteral("3.9.18")) == Status::Unknown,
          "a backend that has not looked anything up judges nothing");

    // The link template, applied to the radio's OWN line.
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          upgradeTargetFor(kPublished, QStringLiteral("3.9.18")))
              == QStringLiteral("https://www.flexradio.com/documentation/"
                                "smartsdr-v3-10-15-release-notes/"),
          "a v3 radio's link opens 3.10.15's notes");
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          upgradeTargetFor(kPublished, QStringLiteral("4.1.5")))
              == QStringLiteral("https://www.flexradio.com/documentation/"
                                "smartsdr-v4-2-20-release-notes/"),
          "a v4 radio's link opens 4.2.20's notes");
    check(releaseNotesUrl(source->releaseNotesUrlTemplate, QString()).isEmpty(),
          "no upgrade target means no link");
}

// The software page is untrusted input (Principle VII) and the part most likely
// to change under us, so its parse is pinned without a network.
void checkThePageIsReadIntoLines()
{
    // The real page's shape: several lines at once, three of them v4.
    const QString page = QStringLiteral(
        "<h3>SmartSDR v4.2.20</h3><h3>SmartSDR v4.2.18</h3><h3>SmartSDR v4.1.5</h3>"
        "<h3>SmartSDR v3.10.15</h3><h3>SmartSDR v2.10.1</h3>"
        "<a href=\"/software/smartsdr-v3-10-10/\">older</a>");
    const auto rel = FirmwareStager::parsePublishedReleases(page);
    check(rel.value(4) == QStringLiteral("4.2.20"), "v4's newest is 4.2.20");
    check(rel.value(3) == QStringLiteral("3.10.15"), "v3's newest is 3.10.15");
    check(rel.value(2) == QStringLiteral("2.10.1"), "v2's newest is 2.10.1");
    check(rel.size() == 3, "exactly the three published lines are reported");

    // Both spellings the page uses.
    check(newestInLine(QStringLiteral("smartsdr-v4-2-20"), 4) == QStringLiteral("4.2.20"),
          "the hyphenated release link is read");
    check(newestInLine(QStringLiteral("SmartSDR v4.2.20"), 4) == QStringLiteral("4.2.20"),
          "the spaced heading spelling is read");

    // Lexicographic traps, per line.
    check(newestInLine(QStringLiteral("SmartSDR v4.2.20 SmartSDR v4.2.5"), 4)
              == QStringLiteral("4.2.20"),
          "4.2.20 beats 4.2.5 although it sorts lower as text");
    check(newestInLine(QStringLiteral("SmartSDR v3.9.19 SmartSDR v3.10.15"), 3)
              == QStringLiteral("3.10.15"),
          "3.10.15 beats 3.9.19 although it sorts lower as text");

    // A date-shaped asset name is not a release (PR #6177 review, N2).
    check(newestInLine(QStringLiteral("<img src=\"/uploads/SmartSDR-2025-03-12.png\">"
                                      " SmartSDR v4.2.20"), 4)
              == QStringLiteral("4.2.20"),
          "a date-shaped asset name does not out-rank a real release");
    check(FirmwareStager::parsePublishedReleases(
              QStringLiteral("SmartSDR-2025-03-12.png")).isEmpty(),
          "a date-shaped asset name alone yields nothing");

    // INT OVERFLOW IN ANY COMPONENT (PR #6177 review, rfoust B1 / Jeremy B2).
    // fromString() does NOT return null for a later overflowing component: it
    // stops and returns the PREFIX, so "99.2147483648.0" becomes
    // QVersionNumber(99) and would out-rank every real release.
    check(newestInLine(QStringLiteral("SmartSDR v99.2147483648.0 SmartSDR v4.2.20"), 4)
              == QStringLiteral("4.2.20"),
          "a second-component overflow does not out-rank a real release");
    check(FirmwareStager::parsePublishedReleases(
              QStringLiteral("SmartSDR v99.2147483648.0")).isEmpty(),
          "a second-component overflow yields no release at all");
    check(newestInLine(QStringLiteral("SmartSDR v4.2.2147483648 SmartSDR v4.2.20"), 4)
              == QStringLiteral("4.2.20"),
          "a third-component overflow does not out-rank a real release");
    check(FirmwareStager::parsePublishedReleases(
              QStringLiteral("SmartSDR v2147483648.0.0")).isEmpty(),
          "a first-component overflow yields no release at all");

    // A page that says nothing useful must produce nothing.
    check(FirmwareStager::parsePublishedReleases(QString()).isEmpty(),
          "an empty page names no release");
    check(FirmwareStager::parsePublishedReleases(
              QStringLiteral("<html>we have moved</html>")).isEmpty(),
          "a page with no SmartSDR release names none");
    check(FirmwareStager::parsePublishedReleases(
              QStringLiteral("SmartSDR v4.2")).isEmpty(),
          "a two-component version is not accepted as a release");
}

// THE SMARTLINK FIX (PR #6177 review nit). The lookup used to hang off the Flex
// backend's own RadioConnection, which a SmartLink session never dials — so a
// WAN operator got no verdict at all. It now hangs off the seam's
// transport-neutral session verb.
//
// Driven through `onRadioSessionEstablished()` with a FRESH cache already in the
// store, so the verb's wiring is proved without any network: a stale cache would
// build a FirmwareStager and reach for flexradio.com, a fresh one must not.
void checkTheSessionVerbAdoptsTheCacheWithoutAsking()
{
    QJsonObject releases;
    releases.insert(QStringLiteral("3"), QStringLiteral("3.10.15"));
    releases.insert(QStringLiteral("4"), QStringLiteral("4.2.20"));
    QJsonObject doc;
    doc.insert(QLatin1String("releases"), releases);
    doc.insert(QLatin1String("checkedAt"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    AetherSDR::AppSettings::instance().setRadioFeature(
        QStringLiteral("flex"), QString(), QStringLiteral("publishedFirmware"), 2, doc);

    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    backend->onRadioSessionEstablished();

    const auto source = backend->capabilities().firmwareUpdateSource;
    check(source.has_value(), "the record is still declared");
    if (!source.has_value())
        return;

    check(source->publishedReleases.value(3) == QStringLiteral("3.10.15")
              && source->publishedReleases.value(4) == QStringLiteral("4.2.20"),
          "a session adopts the cached releases, whatever transport carried it");
    check(backend->findChild<AetherSDR::FirmwareStager*>() == nullptr,
          "a fresh cache means the session asks flexradio.com nothing");
    // And the verdict that follows is the one the SmartLink radio needed.
    check(evaluate(source->publishedReleases, QStringLiteral("3.9.18.36988"))
              == Status::Outdated,
          "a WAN-connected v3 radio now gets its verdict from the cache");
}

} // namespace

int main(int argc, char** argv)
{
    // An isolated, writable settings store: the cache test below writes a radio
    // feature document, and must not touch the operator's real settings.
    // Constructed before QCoreApplication, as the fixture requires.
    TestSettingsProfile profile(QStringLiteral("aether-firmware-currency"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    checkTheVerdictIsScopedToTheRadiosOwnLine();
    checkTheUpgradeTargetIsTheOwnLineRelease();
    checkTheBuildNumberIsIgnored();
    checkComparisonIsNumericNotLexicographic();
    checkNewerThanPublishedIsNotOutdated();
    checkNothingPublishedMeansNoVerdict();
    checkUnparseableRadioVersionsAreUnknown();
    checkTooltipsMatchTheState();
    checkWhenTheCachedAnswerIsLookedUpAgain();
    checkTheBackendDeclaresTheRecordAndAsksNobodyAtConstruction();
    checkThePageIsReadIntoLines();
    checkTheSessionVerbAdoptsTheCacheWithoutAsking();
    return g_failures == 0 ? 0 : 1;
}
