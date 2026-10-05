// Covers the status-bar firmware verdict end to end, minus the network: how the
// published software page is read, which radios the verdict applies to, which
// versions count as behind, and what the operator is told and offered.
//
// The gate is a DECLARED capability, never a family name (docs/HERMES.md
// §"For coding agents"), so "which radios" here means "which backends declared
// that their firmware versions are published" — the tests exercise it that way.

#include "core/FirmwareCurrency.h"
#include "core/FirmwareStager.h"
#include "core/backends/flex/FlexBackend.h"

#include <QCoreApplication>
#include <QDateTime>

#include <cstdio>
#include <memory>

namespace {

using AetherSDR::FirmwareStager;
using AetherSDR::FirmwareCurrency::Status;
using AetherSDR::FirmwareCurrency::compareReleases;
using AetherSDR::FirmwareCurrency::kPublishedVersionMaxAgeSecs;
using AetherSDR::FirmwareCurrency::publishedVersionIsStale;
using AetherSDR::FirmwareCurrency::evaluate;
using AetherSDR::FirmwareCurrency::releaseNotesUrl;
using AetherSDR::FirmwareCurrency::tooltip;

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// The newest release published on FlexRadio's software page at the time these
// tests were written. Spelled out rather than fetched: the tests must not need
// a network, and must not silently re-baseline when FlexRadio ships.
const QString kPublished = QStringLiteral("4.2.20");

Status verdict(const QString& reported)
{
    return evaluate(kPublished, reported);
}

void checkTheRadioInFrontOfUs()
{
    // A FLEX-6500 on the current firmware. Note the build number: the radio
    // reports four components and the page publishes three, so this only reads
    // as current if the comparison truncates.
    check(verdict(QStringLiteral("4.2.20.41343")) == Status::Current,
          "a radio on 4.2.20.41343 is current against a published 4.2.20");
    // The firmware that was running while the CWX mute was live.
    check(verdict(QStringLiteral("4.2.18.41174")) == Status::Outdated,
          "a radio on 4.2.18.41174 is behind a published 4.2.20");
}

void checkTheBuildNumberIsIgnored()
{
    // No published source carries a build number, so two radios on the same
    // release must reach the same verdict whatever their build. Without the
    // truncation the first of these compares as NEWER than the published
    // version and the second as older, from the same release.
    check(verdict(QStringLiteral("4.2.20.99999")) == Status::Current,
          "a high build of the published release is current");
    check(verdict(QStringLiteral("4.2.20.1")) == Status::Current,
          "a low build of the published release is equally current");
    check(verdict(QStringLiteral("4.2.20")) == Status::Current,
          "the release with no build number at all is current");
}

void checkOlderAndNewerReleases()
{
    check(verdict(QStringLiteral("4.1.5.0")) == Status::Outdated,
          "an older minor is behind");
    check(verdict(QStringLiteral("3.3.28.0")) == Status::Outdated,
          "an older major is behind");
    // The published answer is a web page read once at startup and can lag a
    // release. An operator who is ahead of it must not be told to upgrade.
    check(verdict(QStringLiteral("4.2.21.0")) == Status::Current,
          "a release newer than the published one is not flagged out of date");
    check(verdict(QStringLiteral("5.0.0.0")) == Status::Current,
          "a newer major is not flagged out of date either");
}

void checkComparisonIsNumericNotLexicographic()
{
    // As text, "4.2.9" sorts AFTER "4.2.20" and "4.2.100" before it.
    check(verdict(QStringLiteral("4.2.9.99999")) == Status::Outdated,
          "4.2.9 is behind 4.2.20 despite sorting after it as text");
    check(verdict(QStringLiteral("4.2.100.1")) == Status::Current,
          "4.2.100 is ahead of 4.2.20 despite sorting before it as text");
    check(compareReleases(QStringLiteral("4.2.20"), QStringLiteral("4.2.9")) > 0,
          "compareReleases() orders 4.2.20 above 4.2.9");
    check(compareReleases(QStringLiteral("3.10.15"), QStringLiteral("3.9.19")) > 0,
          "compareReleases() orders 3.10.15 above 3.9.19");
    check(compareReleases(QStringLiteral("4.2.20.41343"), QStringLiteral("4.2.20")) == 0,
          "a build number does not make a release newer than itself");
}

void checkNoPublishedAnswerMeansNoVerdict()
{
    // The whole offline story: the fetch failed, or has not answered yet.
    check(evaluate(QString(), QStringLiteral("4.2.18.41174")) == Status::Unknown,
          "no published version yields no verdict, however old the radio is");
    check(evaluate(QStringLiteral("not a version"),
                   QStringLiteral("4.2.18.41174")) == Status::Unknown,
          "an unparseable published version yields no verdict");
    // Guards the guard: without it, the two above would compare against an
    // empty version and come back Outdated.
    check(evaluate(kPublished, QStringLiteral("4.2.18.41174")) == Status::Outdated,
          "the same radio IS judged once a published version exists");
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
    // Not cosmetic: the label keeps whatever tooltip it was last given, so an
    // empty string here is what clears a previous radio's verdict.
    check(tooltip(Status::Unknown).isEmpty(),
          "an unjudged radio clears the tooltip rather than keeping a stale one");
}

void checkTheReleaseNotesLinkIsBuiltFromTheTemplate()
{
    // Nothing here touches the network, and that is now structural rather than
    // incidental: the lookup is driven by the wire's `connected` signal, and no
    // radio is connected in this file. A backend that is merely built — which
    // several teardown tests do, and which happens at app launch — contacts
    // nobody (PR #6177 review, M2).
    auto backend = std::make_unique<AetherSDR::FlexBackend>();
    const auto source = backend->capabilities().firmwareUpdateSource;
    check(source.has_value(), "FlexBackend declares where its firmware is published");
    if (!source.has_value())
        return;

    check(source->latestPublishedVersion.isEmpty(),
          "constructing a backend performs no lookup and publishes no version");
    // The assertion above cannot fail on its own: this file runs no event loop,
    // so an empty version proves nothing about whether the constructor asked.
    // This one does — the stager is parented to the backend, so its absence is
    // direct evidence that nothing has been built to ask with. It fails if the
    // lookup is ever moved back into the constructor (PR #6177 review, N1).
    check(backend->findChild<AetherSDR::FirmwareStager*>() == nullptr,
          "constructing a backend builds no FirmwareStager, so nothing has asked");
    // ...and with none, no radio is judged. This is the offline station, and
    // the first moments of every launch.
    check(evaluate(source->latestPublishedVersion,
                   QStringLiteral("4.2.18.41174")) == Status::Unknown,
          "a backend that has not looked up a version judges nothing");

    // Every one of these URLs was confirmed to return 200 when the template was
    // chosen, across releases from 3.8.23 to 4.2.20.
    check(releaseNotesUrl(source->releaseNotesUrlTemplate, kPublished)
              == QStringLiteral(
                     "https://www.flexradio.com/documentation/"
                     "smartsdr-v4-2-20-release-notes/"),
          "the published release's notes URL is built from the declared template");
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          QStringLiteral("3.8.23"))
              == QStringLiteral(
                     "https://www.flexradio.com/documentation/"
                     "smartsdr-v3-8-23-release-notes/"),
          "an older release maps to its own notes page");
    // A build number must not reach the URL — there is no such page.
    check(releaseNotesUrl(source->releaseNotesUrlTemplate,
                          QStringLiteral("4.2.20.41343"))
              == QStringLiteral(
                     "https://www.flexradio.com/documentation/"
                     "smartsdr-v4-2-20-release-notes/"),
          "a reported build number is dropped before the URL is built");
    // The click handler refuses an empty URL, so these are the cases where the
    // label stays unclickable rather than opening something wrong.
    check(releaseNotesUrl(source->releaseNotesUrlTemplate, QString()).isEmpty(),
          "no version means no link");
    check(releaseNotesUrl(QString(), kPublished).isEmpty(),
          "no declared template means no link");
}

// The software page is untrusted input (Principle VII) and the part of this
// feature most likely to change under us, so its parse is pinned without a
// network. The fixture is the shape the real page uses, which was confirmed by
// fetching it: many releases named on one page, oldest to newest.
void checkThePublishedVersionIsReadFromThePage()
{
    const QString page = QStringLiteral(
        "<h3>SmartSDR v3.8.23</h3><a href=\"/software/smartsdr-v4-1-5/\">"
        "SmartSDR v4.1.5</a><h3>SmartSDR v4.2.18</h3>"
        "<img alt=\"SmartSDR v4.2.20\">smartsdr-v4-2-20");
    check(FirmwareStager::parseLatestVersion(page) == QStringLiteral("4.2.20"),
          "the highest release on the page is the published one");

    // The bug this replaced: a string maximum picks 4.2.5 over 4.2.20, and
    // 3.9.19 over 3.10.15. Both pairs are plausible on a page listing history.
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR v4.2.20 SmartSDR v4.2.5"))
              == QStringLiteral("4.2.20"),
          "4.2.20 beats 4.2.5 although it sorts lower as text");
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR v3.9.19 SmartSDR v3.10.15"))
              == QStringLiteral("3.10.15"),
          "3.10.15 beats 3.9.19 although it sorts lower as text");

    // Both spellings the page actually uses.
    check(FirmwareStager::parseLatestVersion(QStringLiteral("smartsdr-v4-2-20"))
              == QStringLiteral("4.2.20"),
          "the hyphenated link spelling is read");
    check(FirmwareStager::parseLatestVersion(QStringLiteral("SmartSDR v4.2.20"))
              == QStringLiteral("4.2.20"),
          "the spaced heading spelling is read");

    // A page that says nothing useful must not produce a version. Each of these
    // reaches the operator as Unknown, not as a wrong verdict.
    check(FirmwareStager::parseLatestVersion(QString()).isEmpty(),
          "an empty page names no version");
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("<html>we have moved</html>")).isEmpty(),
          "a page with no SmartSDR version names none");
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR v4.2")).isEmpty(),
          "a two-component version is not accepted as a release");

    // The page is not ours and its digits are unbounded. A component past
    // INT_MAX parses to nothing (measured), so it must be skipped rather than
    // returned as a version the rest of the app cannot use.
    // A DATE-SHAPED SLUG IS NOT A RELEASE. The hyphenated spelling made `v`
    // optional, so an asset named SmartSDR-2025-03-12.png parsed as release
    // 2025.3.12 — which marks every radio Outdated and links a 404, the exact
    // wrong-verdict direction this feature claims to avoid. The `v` is now
    // required on the hyphenated form (PR #6177 review, N2).
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("<img src=\"/uploads/SmartSDR-2025-03-12.png\">"
                             " SmartSDR v4.2.20"))
              == QStringLiteral("4.2.20"),
          "a date-shaped asset name does not out-rank a real release");
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR-2025-03-12.png")).isEmpty(),
          "a date-shaped asset name alone yields no version");
    // The spelling that IS a release link must still be read.
    check(FirmwareStager::parseLatestVersion(QStringLiteral("smartsdr-v4-2-20"))
              == QStringLiteral("4.2.20"),
          "the hyphenated release link still carries its v");

    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR v2147483648.0.0")).isEmpty(),
          "a version component that overflows int yields no version at all");
    check(FirmwareStager::parseLatestVersion(
              QStringLiteral("SmartSDR v99999999999.1.1 SmartSDR v4.2.20"))
              == QStringLiteral("4.2.20"),
          "an overflowing version does not crowd out a real one on the same page");
}

// When may the client contact flexradio.com again?
//
// PR #6177 review, M2: the lookup must happen only on connecting to a Flex, and
// the answer must be cached so that a reconnect, a switch of radios, and a later
// launch all reuse it. This is the rule that decides "again".
void checkWhenTheCachedVersionIsLookedUpAgain()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString cached = QStringLiteral("4.2.20");

    // The quiet path: a cache written minutes ago is reused, so connecting a
    // second time — or relaunching — contacts nobody.
    check(!publishedVersionIsStale(cached, now.addSecs(-60), now),
          "a minutes-old answer is reused");
    check(!publishedVersionIsStale(cached, now.addSecs(-kPublishedVersionMaxAgeSecs + 60), now),
          "an answer just inside the window is reused");

    check(publishedVersionIsStale(cached, now.addSecs(-kPublishedVersionMaxAgeSecs), now),
          "an answer exactly at the window is looked up again");

    // The window is ONE DAY, pinned in absolute terms rather than against the
    // constant. The checks above would all still pass if someone widened it, and
    // the length is the maintainer's stated policy (PR #6177 review, M2), not an
    // implementation detail to drift.
    check(kPublishedVersionMaxAgeSecs == 24 * 60 * 60,
          "the lookup window is 24 hours");
    check(!publishedVersionIsStale(cached, now.addSecs(-23 * 60 * 60), now),
          "23 hours old is still reused");
    check(publishedVersionIsStale(cached, now.addSecs(-25 * 60 * 60), now),
          "25 hours old is looked up again");
    check(publishedVersionIsStale(cached, now.addSecs(-7 * 24 * 60 * 60), now),
          "a week-old answer is looked up again");

    // Nothing cached at all — the first connect on a fresh install.
    check(publishedVersionIsStale(QString(), now.addSecs(-60), now),
          "no cached version means look it up, however recent the stamp");
    // A stamp that cannot be read must not be treated as fresh.
    check(publishedVersionIsStale(cached, QDateTime(), now),
          "an unreadable stamp means look it up");
    check(publishedVersionIsStale(cached, QDateTime::fromString(
              QStringLiteral("not a date"), Qt::ISODate), now),
          "a malformed stamp means look it up");

    // A stamp in the FUTURE — written while the clock was wrong, or on a
    // machine since corrected backwards. Without this it would never expire,
    // and the station would silently stop checking forever.
    check(publishedVersionIsStale(cached, now.addSecs(60), now),
          "a stamp in the future is looked up again rather than trusted forever");
    check(publishedVersionIsStale(cached, now.addSecs(10 * 365 * 24 * 60 * 60LL), now),
          "a wildly future stamp does not freeze the cache");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    checkTheRadioInFrontOfUs();
    checkTheBuildNumberIsIgnored();
    checkOlderAndNewerReleases();
    checkComparisonIsNumericNotLexicographic();
    checkNoPublishedAnswerMeansNoVerdict();
    checkUnparseableRadioVersionsAreUnknown();
    checkTooltipsMatchTheState();
    checkTheReleaseNotesLinkIsBuiltFromTheTemplate();
    checkThePublishedVersionIsReadFromThePage();
    checkWhenTheCachedVersionIsLookedUpAgain();
    return g_failures == 0 ? 0 : 1;
}
