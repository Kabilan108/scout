#include "fuzzy.h"

#include <algorithm>

namespace fuzzy {
namespace {

constexpr int kScoreMatch = 16;
constexpr int kBonusPathSeparator = 10;
constexpr int kBonusBoundary = 8;
constexpr int kBonusCamel = 7;
constexpr int kBonusConsecutive = 6;
constexpr int kPenaltyGapStart = 3;
constexpr int kPenaltyGapExtension = 1;
constexpr int kBonusBasename = 24;

bool isSeparator(QChar c) { return c == u'/'; }
bool isDelimiter(QChar c) { return c == u'-' || c == u'_' || c == u'.' || c == u' '; }

int boundaryBonus(QStringView s, int i) {
    if (i == 0)
        return kBonusPathSeparator;
    const QChar prev = s[i - 1];
    const QChar cur = s[i];
    if (isSeparator(prev))
        return kBonusPathSeparator;
    if (isDelimiter(prev))
        return kBonusBoundary;
    if (prev.isLower() && cur.isUpper())
        return kBonusCamel;
    if (!prev.isDigit() && cur.isDigit())
        return kBonusCamel;
    return 0;
}

bool equal(QChar a, QChar b, bool caseSensitive) {
    return caseSensitive ? a == b : a.toCaseFolded() == b.toCaseFolded();
}

// fzf v1: find the first window that contains the query as a subsequence,
// shrink it from the right, then score the greedy positions inside it.
bool matchRange(QStringView query, QStringView text, int offset, bool caseSensitive, Match *out) {
    const int n = int(text.size());
    const int m = int(query.size());
    if (m == 0) {
        if (out)
            *out = {};
        return true;
    }
    int qi = 0;
    int end = -1;
    for (int i = offset; i < n; ++i) {
        if (equal(text[i], query[qi], caseSensitive) && ++qi == m) {
            end = i;
            break;
        }
    }
    if (end < 0)
        return false;

    qi = m - 1;
    int start = end;
    for (int i = end; i >= offset; --i) {
        if (equal(text[i], query[qi], caseSensitive)) {
            if (--qi < 0) {
                start = i;
                break;
            }
        }
    }

    Match result;
    result.positions.reserve(m);
    int score = 0;
    int last = -2;
    int chunkBonus = 0;
    bool inGap = false;
    qi = 0;
    for (int i = start; i <= end && qi < m; ++i) {
        if (!equal(text[i], query[qi], caseSensitive)) {
            if (last >= 0) {
                score -= inGap ? kPenaltyGapExtension : kPenaltyGapStart;
                inGap = true;
            }
            continue;
        }
        int bonus = boundaryBonus(text, i);
        if (last == i - 1) {
            chunkBonus = std::max({chunkBonus, bonus, kBonusConsecutive});
            bonus = chunkBonus;
        } else {
            chunkBonus = bonus;
        }
        if (qi == 0)
            bonus *= 2;
        score += kScoreMatch + bonus;
        result.positions.append(i);
        last = i;
        inGap = false;
        ++qi;
    }
    result.score = score;
    if (out)
        *out = std::move(result);
    return true;
}

} // namespace

bool match(QStringView query, QStringView candidate, Match *out) {
    bool caseSensitive = false;
    for (QChar c : query) {
        if (c.isUpper()) {
            caseSensitive = true;
            break;
        }
    }

    Match full;
    if (!matchRange(query, candidate, 0, caseSensitive, &full))
        return false;

    QStringView trimmed = candidate;
    while (trimmed.endsWith(u'/'))
        trimmed.chop(1);
    const int slash = int(trimmed.lastIndexOf(u'/'));
    if (slash >= 0 && !query.contains(u'/')) {
        Match base;
        if (matchRange(query, trimmed, slash + 1, caseSensitive, &base)) {
            base.score += kBonusBasename;
            if (base.score >= full.score)
                full = std::move(base);
        }
    }
    // Shorter candidates win ties.
    full.score -= int(candidate.size()) / 12;
    if (out)
        *out = std::move(full);
    return true;
}

} // namespace fuzzy
