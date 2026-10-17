#pragma once

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>

#include "chooser.h"

class MainWindow;

// `scout --service`: the D-Bus side of scout. It owns
//  - org.freedesktop.FileManager1, so "Show in folder" in browsers and other
//    apps opens a scout window with the file selected, and
//  - org.freedesktop.impl.portal.desktop.scout, a FileChooser backend for
//    xdg-desktop-portal, so open and save dialogs are scout windows.
// D-Bus activation starts it on demand; it exits after a while with no windows.
namespace service {

void registerTypes();
// Builds the chooser request a portal call describes.
ChooserRequest requestFromOptions(ChooserRequest::Mode mode, const QString &title, const QVariantMap &options);
// The portal's results map for a finished chooser.
QVariantMap resultsFromChooser(const ChooserRequest &request, const ChooserResult &result);
// Opens windows for FileManager1 calls; one per folder, revealing the items.
QList<MainWindow *> showItems(const QStringList &uris, bool folders);
// Runs the service; returns the exit code.
int run();

} // namespace service

class FileManager1 : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
    using QObject::QObject;

public slots:
    void ShowFolders(const QStringList &uris, const QString &startupId);
    void ShowItems(const QStringList &uris, const QString &startupId);
    void ShowItemProperties(const QStringList &uris, const QString &startupId);
};

class FileChooserPortal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
public:
    using QObject::QObject;
    int openDialogs() const { return m_open; }

public slots:
    uint OpenFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, QVariantMap &results);
    uint SaveFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, QVariantMap &results);
    uint SaveFiles(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                   const QString &title, const QVariantMap &options, QVariantMap &results);

private:
    void openChooser(ChooserRequest::Mode mode, const QDBusObjectPath &handle, const QString &title,
                     const QVariantMap &options);

    int m_open = 0;
};

// The org.freedesktop.impl.portal.Request object xdg-desktop-portal uses to
// cancel a dialog it asked for.
class PortalRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")
public:
    using QObject::QObject;

public slots:
    void Close();

signals:
    void closeRequested();
};
