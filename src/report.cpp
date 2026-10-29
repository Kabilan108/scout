#include "report.h"

#include "config.h"
#include "logging.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QUrl>

#include <optional>

namespace {

constexpr int kTailLines = 50;

QString count(qsizetype n, const char *word) {
    return QStringLiteral("%1 %2%3").arg(n).arg(QLatin1String(word)).arg(n == 1 ? "" : "s");
}

bool isPathChar(QChar c) { return c.isLetterOrNumber() || QStringView(u"._-+@~/").contains(c); }

bool endsPath(QStringView text, qsizetype i) {
    const QChar c = text[i];
    const QChar next = i + 1 < text.size() ? text[i + 1] : QChar();
    if (QStringView(u"\"'()[],\n\t").contains(c))
        return true;
    return (c == u':' && (next.isNull() || next == u' ')) || (c == u' ' && (next == u'(' || next == u'['));
}

std::optional<QStringList> readLines(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    QStringList lines = QString::fromUtf8(f.readAll()).split(u'\n');
    if (lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

bool isStamped(const QString &line) {
    static const QRegularExpression stamp(QStringLiteral(R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[\d+\] )"));
    return stamp.match(line).hasMatch();
}

// The last crash line and the backtrace under it, whose lines carry no timestamp.
QStringList lastCrash(const QStringList &lines, qsizetype *start) {
    for (qsizetype i = lines.size() - 1; i >= 0; --i) {
        if (!lines[i].contains(QLatin1String("] F crash: signal ")) || !isStamped(lines[i]))
            continue;
        qsizetype end = i + 1;
        while (end < lines.size() && !isStamped(lines[end]))
            ++end;
        *start = i;
        return lines.mid(i, end - i);
    }
    return {};
}

QString lastStartPlatform(const QStringList &lines) {
    static const QRegularExpression start(QStringLiteral(R"(\] I scout .* starting: mode \w+, platform ([^,]+),)"));
    for (qsizetype i = lines.size() - 1; i >= 0; --i)
        if (const QRegularExpressionMatch m = start.match(lines[i]); m.hasMatch() && isStamped(lines[i]))
            return m.captured(1);
    return {};
}

QString configSection(const QString &path, const CommandRegistry &commands, const KeyMap &defaults) {
    QFile f(path);
    if (!f.exists())
        return QStringLiteral("config: none at %1, so scout uses its defaults\n").arg(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("config: %1, can't read it: %2\n").arg(path, f.errorString());
    const ConfigReport r = config::analyze(QString::fromUtf8(f.readAll()), commands, defaults);
    QString out = QStringLiteral("config: %1\n%2, %3\n").arg(path, count(r.errors.size(), "error"), count(r.notes.size(), "override"));
    for (const QString &e : r.errors)
        out += QStringLiteral("  %1\n").arg(e);
    return out;
}

QString orUnset(const QString &value) { return value.isEmpty() ? QStringLiteral("(unset)") : value; }

} // namespace

report::Environment report::current(const QString &platform) {
    return {platform,
            qEnvironmentVariable("XDG_CURRENT_DESKTOP"),
            qEnvironmentVariable("XDG_SESSION_TYPE"),
            QDir::homePath(),
            config::defaultPath(),
            logging::isOpen() ? logging::path() : logging::defaultPath()};
}

QString report::build(const Environment &env, const CommandRegistry &commands, const KeyMap &defaults) {
    const std::optional<QStringList> log = readLines(env.logPath);
    const QString rotatedPath = env.logPath + QStringLiteral(".1");
    const QStringList rotated = readLines(rotatedPath).value_or(QStringList());

    QString body = QStringLiteral("scout %1\n").arg(logging::versionString());
    body += QStringLiteral("Qt %1").arg(QLatin1String(qVersion()));
    if (qstrcmp(qVersion(), QT_VERSION_STR) != 0)
        body += QStringLiteral(" (built with %1)").arg(QLatin1String(QT_VERSION_STR));
    body += QStringLiteral("\nplatform: %1").arg(env.platform);
    QString lastPlatform = lastStartPlatform(log.value_or(QStringList()));
    if (lastPlatform.isEmpty())
        lastPlatform = lastStartPlatform(rotated);
    if (!lastPlatform.isEmpty() && lastPlatform != env.platform)
        body += QStringLiteral(", %1 at the last start in scout.log").arg(lastPlatform);
    body += QStringLiteral("\nXDG_CURRENT_DESKTOP: %1\nXDG_SESSION_TYPE: %2\n\n").arg(orUnset(env.desktop), orUnset(env.sessionType));

    body += configSection(env.configPath, commands, defaults);
    body += u'\n';

    qsizetype tailStart = 0;
    if (!log) {
        body += QFile::exists(env.logPath) ? QStringLiteral("log: can't read %1\n").arg(env.logPath)
                                           : QStringLiteral("log: none at %1\n").arg(env.logPath);
    } else {
        tailStart = std::max<qsizetype>(0, log->size() - kTailLines);
        body += QStringLiteral("log: %1, last %2\n").arg(env.logPath, count(log->size() - tailStart, "line"));
        for (qsizetype i = tailStart; i < log->size(); ++i)
            body += log->at(i) + u'\n';
    }

    qsizetype crashStart = 0;
    QStringList crash = lastCrash(log.value_or(QStringList()), &crashStart);
    const bool crashInTail = !crash.isEmpty() && crashStart >= tailStart;
    QString crashSource = env.logPath;
    if (crash.isEmpty()) {
        crash = lastCrash(rotated, &crashStart);
        crashSource = rotatedPath;
    }
    if (!crash.isEmpty() && !crashInTail)
        body += QStringLiteral("\nmost recent crash, from %1 line %2:\n%3\n").arg(crashSource).arg(crashStart + 1).arg(crash.join(u'\n'));

    body = redactHome(body, env.home, {env.configPath, env.logPath, rotatedPath});
    qsizetype longest = 0, run = 0;
    for (const QChar c : std::as_const(body)) {
        run = c == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const QString fence(std::max<qsizetype>(3, longest + 1), u'`');
    return fence + QStringLiteral("text\n") + body + fence + u'\n';
}

QString report::redactHome(const QString &text, const QString &home, const QStringList &keep) {
    const QString root = QDir::cleanPath(home);
    const bool matchHome = !home.isEmpty() && root != QStringLiteral("/");
    // As a file URL spells it, e.g. "/home/you%20space".
    const QString encodedRoot = QString::fromLatin1(QUrl::toPercentEncoding(root, "/"));
    // Kept paths are matched whole first: they are known to end where a space and more words follow.
    QStringList keptTails;
    for (const QString &k : keep)
        if (const QString clean = QDir::cleanPath(k); clean.startsWith(root + u'/'))
            keptTails << clean.sliced(root.size());
    QString out;
    out.reserve(text.size());
    qsizetype i = 0;
    while (i < text.size()) {
        const bool atBoundary = i == 0 || !isPathChar(text[i - 1]) || QStringView(text).first(i).endsWith(u"file://");
        qsizetype prefix = 0;
        if (atBoundary && matchHome && QStringView(text).sliced(i).startsWith(root))
            prefix = root.size();
        else if (atBoundary && matchHome && encodedRoot != root && QStringView(text).sliced(i).startsWith(encodedRoot, Qt::CaseInsensitive))
            prefix = encodedRoot.size();
        else if (atBoundary && text[i] == u'~')
            prefix = 1;
        const qsizetype afterPrefix = i + prefix;
        if (prefix == 0 || (afterPrefix < text.size() && text[afterPrefix] != u'/' && isPathChar(text[afterPrefix]))) {
            out += text[i++];
            continue;
        }
        const qsizetype newline = text.indexOf(u'\n', afterPrefix);
        const QStringView after = QStringView(text).sliced(afterPrefix, (newline < 0 ? text.size() : newline) - afterPrefix);
        // A slash later on the line could mean the match was only the start of a longer path.
        qsizetype keptLength = -1;
        for (const QString &tail : std::as_const(keptTails))
            if (tail.size() > keptLength && after.startsWith(tail) && !after.sliced(tail.size()).contains(u'/') &&
                (after.size() == tail.size() || !isPathChar(after[tail.size()])))
                keptLength = tail.size();
        qsizetype end = afterPrefix;
        if (keptLength >= 0) {
            end += keptLength;
        } else {
            // A delimiter can sit in a folder name too, as in "~/Clients (2026)/Acme", so the
            // path runs at least to the line's last slash; only a file name's tail can show.
            if (end < text.size() && text[end] == u'/')
                end = std::max(end, text.lastIndexOf(u'/', newline < 0 ? text.size() - 1 : newline));
            while (end < text.size() && !endsPath(text, end))
                ++end;
        }
        const QStringView rest = QStringView(text).sliced(afterPrefix, end - afterPrefix);
        if (keptLength >= 0 || rest.isEmpty() || rest == u"/")
            out += QStringLiteral("~") + rest;
        else
            out += QStringLiteral("~/…");
        i = end;
    }
    return out;
}
