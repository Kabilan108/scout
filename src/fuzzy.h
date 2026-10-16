#pragma once

#include <QList>
#include <QString>

namespace fuzzy {

struct Match {
    int score = 0;
    QList<int> positions;
};

// Scores `candidate` against `query`, fzf style: every query character must
// appear in order. Matches at word boundaries, consecutive runs and matches in
// the last path component score higher. Smartcase: an uppercase letter in the
// query makes the match case sensitive. Returns false when there is no match.
bool match(QStringView query, QStringView candidate, Match *out = nullptr);

} // namespace fuzzy
