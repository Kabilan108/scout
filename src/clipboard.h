#pragma once

#include <QStringList>

class QMimeData;

struct ClipboardFiles {
    QStringList paths;
    bool cut = false;
};

// Encodes files the way GNOME and KDE file managers do, so copy/paste and
// drag-and-drop interoperate with Nautilus, Dolphin, browsers and chat apps.
QMimeData *makeFileMimeData(const QStringList &paths, bool cut);
ClipboardFiles parseFileMimeData(const QMimeData *data);

void setClipboardFiles(const QStringList &paths, bool cut);
ClipboardFiles clipboardFiles();
void setClipboardText(const QString &text);
