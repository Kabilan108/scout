#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// $XDG_STATE_HOME/scout/state.json: what scout remembers between runs. Kept
// apart from scout.conf so a config managed by Nix never fights it.
namespace state {

QString path();

// The text size zoom left behind; none after zoom-reset.
std::optional<double> fontSize();
void setFontSize(std::optional<double> points);

struct Session {
    QStringList tabs; // each tab's folder
    int active = 0;
};
// The tabs of the window closed last.
std::optional<Session> session();
void setSession(const Session &session);

} // namespace state
