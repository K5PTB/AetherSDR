#pragma once

#include <QString>
#include <QVersionNumber>

// Is the radio's firmware behind the newest release its vendor publishes, and
// what do we tell the operator about it?
//
// NO FAMILY IS NAMED HERE. The published version arrives from whoever fetched
// it, and the release-notes link is built from a template the connected backend
// declares (RadioCapabilities::FirmwareUpdateSource) — see that record for why
// a consumer asks for it rather than testing a family name (docs/HERMES.md
// §"For coding agents"). A backend that declares nothing, and a check that has
// not answered, both yield Unknown, which draws exactly as the status bar
// always has.
namespace AetherSDR::FirmwareCurrency {

// A RELEASE is major.minor.patch, and that is the whole comparison.
//
// A radio reports four components ("4.2.20.41343"); the fourth is a build
// number. Nothing FlexRadio publishes carries one — not the software page, not
// the installer URL, not the MD5 file, not the release notes — it appears only
// inside a downloaded installer. So there is never a published build number to
// compare against, and keeping the radio's would make every radio look ahead of
// every published version. Truncating both sides is the only comparison that
// can actually be made.
inline QVersionNumber releaseOf(const QString& version)
{
    const QVersionNumber full = QVersionNumber::fromString(version);
    if (full.isNull())
        return {};

    const QList<int> segments = full.segments();
    return QVersionNumber(segments.mid(0, qMin<qsizetype>(3, segments.size())));
}

enum class Status {
    Unknown,   // nothing published yet, or a version neither side can parse
    Current,   // at or newer than the newest published release
    Outdated,  // behind the newest published release
};

// A release NEWER than the published one is Current, not Outdated. A published
// answer can be stale — it is a web page read at startup — and an operator on a
// release the page has not caught up with must not be told to upgrade.
inline Status evaluate(const QString& latestPublished, const QString& reported)
{
    const QVersionNumber theirs = releaseOf(reported);
    const QVersionNumber ours = releaseOf(latestPublished);
    if (theirs.isNull() || ours.isNull())
        return Status::Unknown;

    return QVersionNumber::compare(theirs, ours) < 0 ? Status::Outdated
                                                     : Status::Current;
}

// Which of two versions names the newer release: -1, 0 or 1, on the same
// major.minor.patch basis as evaluate(). Returns 0 when either is unparseable,
// which reads as "no reason to think one is newer".
inline int compareReleases(const QString& lhs, const QString& rhs)
{
    const QVersionNumber left = releaseOf(lhs);
    const QVersionNumber right = releaseOf(rhs);
    if (left.isNull() || right.isNull())
        return 0;

    const int cmp = QVersionNumber::compare(left, right);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

// The release-notes page for `version`, from a template whose "%1" is the
// release with its dots as dashes ("4.2.20" -> "4-2-20"). Empty when there is
// no template or nothing parseable to put in it, and the caller then offers the
// operator nowhere to click.
inline QString releaseNotesUrl(const QString& urlTemplate, const QString& version)
{
    const QVersionNumber release = releaseOf(version);
    if (urlTemplate.isEmpty() || release.isNull())
        return {};

    return urlTemplate.arg(release.toString().replace(QLatin1Char('.'),
                                                      QLatin1Char('-')));
}

// Empty for Unknown, so the caller clears the tooltip rather than leaving a
// previous radio's verdict on the label.
inline QString tooltip(Status status)
{
    switch (status) {
    case Status::Current:
        return QStringLiteral("Firmware up-to-date");
    case Status::Outdated:
        return QStringLiteral("Firmware is out of date, click to see release notes.");
    case Status::Unknown:
        break;
    }
    return {};
}

} // namespace AetherSDR::FirmwareCurrency
