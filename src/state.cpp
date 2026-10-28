#include "state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <functional>

namespace {

QJsonObject load() {
    QFile f(state::path());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// QSaveFile writes a temporary file and renames it over the old one.
void update(const std::function<void(QJsonObject &)> &change) {
    QJsonObject root = load();
    change(root);
    QDir().mkpath(QFileInfo(state::path()).absolutePath());
    QSaveFile f(state::path());
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("could not write %s: %s", qPrintable(state::path()), qPrintable(f.errorString()));
        return;
    }
    f.write(QJsonDocument(root).toJson());
    if (!f.commit())
        qWarning("could not write %s: %s", qPrintable(state::path()), qPrintable(f.errorString()));
}

} // namespace

QString state::path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/scout/state.json");
}

std::optional<double> state::fontSize() {
    const QJsonValue v = load().value(QLatin1String("font-size"));
    return v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt;
}

void state::setFontSize(std::optional<double> points) {
    update([points](QJsonObject &root) {
        if (points)
            root.insert(QLatin1String("font-size"), *points);
        else
            root.remove(QLatin1String("font-size"));
    });
}

std::optional<state::Session> state::session() {
    const QJsonValue v = load().value(QLatin1String("session"));
    if (!v.isObject())
        return std::nullopt;
    Session s;
    for (const QJsonValue &tab : v.toObject().value(QLatin1String("tabs")).toArray())
        if (tab.isString())
            s.tabs << tab.toString();
    s.active = v.toObject().value(QLatin1String("active")).toInt();
    return s;
}

void state::setSession(const Session &session) {
    update([&session](QJsonObject &root) {
        root.insert(QLatin1String("session"),
                    QJsonObject{{QLatin1String("tabs"), QJsonArray::fromStringList(session.tabs)},
                                {QLatin1String("active"), session.active}});
    });
}
