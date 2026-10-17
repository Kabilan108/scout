#include "service.h"

#include "mainwindow.h"

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUrl>

Q_DECLARE_METATYPE(FileFilter)
Q_DECLARE_METATYPE(ChooserChoice)

using StringPair = QPair<QString, QString>;
using Rule = QPair<uint, QString>;

QDBusArgument &operator<<(QDBusArgument &arg, const FileFilter &filter) {
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, FileFilter &filter) {
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const ChooserChoice &choice) {
    arg.beginStructure();
    arg << choice.id << choice.label << choice.options << choice.selected;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, ChooserChoice &choice) {
    arg.beginStructure();
    arg >> choice.id >> choice.label >> choice.options >> choice.selected;
    arg.endStructure();
    return arg;
}

namespace {

constexpr auto kFileManagerName = "org.freedesktop.FileManager1";
constexpr auto kPortalName = "org.freedesktop.impl.portal.desktop.scout";

template <typename T>
T demarshal(const QVariant &value) {
    T out{};
    if (value.canConvert<QDBusArgument>())
        value.value<QDBusArgument>() >> out;
    else if (value.canConvert<T>())
        out = value.value<T>();
    return out;
}

QString pathFromBytes(const QVariant &value) {
    QByteArray bytes = value.canConvert<QDBusArgument>() ? demarshal<QByteArray>(value) : value.toByteArray();
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QFile::decodeName(bytes);
}

QString localPath(const QString &uriOrPath) {
    if (uriOrPath.startsWith(QLatin1String("file:")))
        return QUrl(uriOrPath).toLocalFile();
    return uriOrPath;
}

void showWindow(MainWindow *window, const QString &startupId) {
    // Lets the compositor focus the window the calling app asked for.
    if (!startupId.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", startupId.toUtf8());
    window->show();
    window->raise();
    window->activateWindow();
    qunsetenv("XDG_ACTIVATION_TOKEN");
}

} // namespace

void service::registerTypes() {
    qDBusRegisterMetaType<Rule>();
    qDBusRegisterMetaType<QList<Rule>>();
    qDBusRegisterMetaType<FileFilter>();
    qDBusRegisterMetaType<QList<FileFilter>>();
    qDBusRegisterMetaType<StringPair>();
    qDBusRegisterMetaType<QList<StringPair>>();
    qDBusRegisterMetaType<ChooserChoice>();
    qDBusRegisterMetaType<QList<ChooserChoice>>();
    qDBusRegisterMetaType<QList<QByteArray>>();
}

ChooserRequest service::requestFromOptions(ChooserRequest::Mode mode, const QString &title, const QVariantMap &options) {
    ChooserRequest r;
    r.mode = mode;
    r.title = title;
    r.acceptLabel = options.value(QStringLiteral("accept_label")).toString();
    r.multiple = mode == ChooserRequest::Mode::Open && options.value(QStringLiteral("multiple")).toBool();
    r.directory = mode == ChooserRequest::Mode::Open && options.value(QStringLiteral("directory")).toBool();
    r.filters = demarshal<QList<FileFilter>>(options.value(QStringLiteral("filters")));
    if (options.contains(QStringLiteral("current_filter"))) {
        const FileFilter current = demarshal<FileFilter>(options.value(QStringLiteral("current_filter")));
        for (int i = 0; i < r.filters.size(); ++i)
            if (r.filters[i].name == current.name)
                r.currentFilter = i;
        if (r.currentFilter < 0 && r.filters.isEmpty() && !current.rules.isEmpty()) {
            r.filters << current;
            r.currentFilter = 0;
        }
    }
    r.choices = demarshal<QList<ChooserChoice>>(options.value(QStringLiteral("choices")));
    r.currentName = options.value(QStringLiteral("current_name")).toString();
    if (options.contains(QStringLiteral("current_folder")))
        r.currentFolder = pathFromBytes(options.value(QStringLiteral("current_folder")));
    if (options.contains(QStringLiteral("current_file")))
        r.currentFile = pathFromBytes(options.value(QStringLiteral("current_file")));
    for (const QByteArray &f : demarshal<QList<QByteArray>>(options.value(QStringLiteral("files")))) {
        QByteArray name = f;
        while (name.endsWith('\0'))
            name.chop(1);
        r.files << QFileInfo(QFile::decodeName(name)).fileName();
    }
    if (r.currentFolder.isEmpty() || !QFileInfo(r.currentFolder).isDir())
        r.currentFolder = r.currentFile.isEmpty() ? QDir::homePath() : QFileInfo(r.currentFile).absolutePath();
    return r;
}

QVariantMap service::resultsFromChooser(const ChooserRequest &request, const ChooserResult &result) {
    QVariantMap out;
    if (!result.accepted)
        return out;
    QStringList uris;
    for (const QString &p : result.paths)
        uris << QString::fromUtf8(QUrl::fromLocalFile(p).toEncoded());
    out.insert(QStringLiteral("uris"), uris);
    if (!result.choices.isEmpty())
        out.insert(QStringLiteral("choices"), QVariant::fromValue(result.choices));
    if (result.filter >= 0 && result.filter < request.filters.size())
        out.insert(QStringLiteral("current_filter"), QVariant::fromValue(request.filters[result.filter]));
    if (request.mode == ChooserRequest::Mode::Open)
        out.insert(QStringLiteral("writable"), true);
    return out;
}

QList<MainWindow *> service::showItems(const QStringList &uris, bool folders) {
    // One window per folder, with the items in it revealed and marked.
    QList<QPair<QString, QStringList>> groups;
    for (const QString &uri : uris) {
        const QString path = QDir::cleanPath(localPath(uri));
        if (path.isEmpty())
            continue;
        const QString dir = folders ? path : QFileInfo(path).absolutePath();
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == dir; });
        if (it == groups.end()) {
            groups.append({dir, {}});
            it = groups.end() - 1;
        }
        if (!folders)
            it->second << path;
    }
    QList<MainWindow *> windows;
    for (const auto &[dir, items] : groups) {
        auto *window = new MainWindow(items.isEmpty() ? dir : items.first());
        window->setAttribute(Qt::WA_DeleteOnClose);
        if (items.size() > 1)
            window->marks()->add(items);
        windows << window;
    }
    return windows;
}

void FileManager1::ShowFolders(const QStringList &uris, const QString &startupId) {
    for (MainWindow *w : service::showItems(uris, true))
        showWindow(w, startupId);
}

void FileManager1::ShowItems(const QStringList &uris, const QString &startupId) {
    for (MainWindow *w : service::showItems(uris, false))
        showWindow(w, startupId);
}

void FileManager1::ShowItemProperties(const QStringList &uris, const QString &startupId) {
    for (MainWindow *w : service::showItems(uris, false)) {
        showWindow(w, startupId);
        QPointer<MainWindow> window(w);
        // Properties need the listing, which loads asynchronously.
        auto *poll = new QTimer(w);
        poll->setInterval(30);
        connect(poll, &QTimer::timeout, w, [window, poll] {
            if (window && window->isIdle()) {
                poll->stop();
                poll->deleteLater();
                window->runCommandLine(QStringLiteral("properties"));
            }
        });
        poll->start();
    }
}

uint FileChooserPortal::OpenFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                                 const QVariantMap &options, QVariantMap &) {
    openChooser(ChooserRequest::Mode::Open, handle, title, options);
    return 2;
}

uint FileChooserPortal::SaveFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                                 const QVariantMap &options, QVariantMap &) {
    openChooser(ChooserRequest::Mode::Save, handle, title, options);
    return 2;
}

uint FileChooserPortal::SaveFiles(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                                  const QVariantMap &options, QVariantMap &) {
    openChooser(ChooserRequest::Mode::SaveFiles, handle, title, options);
    return 2;
}

void FileChooserPortal::openChooser(ChooserRequest::Mode mode, const QDBusObjectPath &handle, const QString &title,
                                    const QVariantMap &options) {
    setDelayedReply(true);
    const QDBusMessage call = message();
    QDBusConnection bus = connection();
    const ChooserRequest request = service::requestFromOptions(mode, title, options);
    qInfo("scout portal: %s \"%s\" in %s (%d filters%s)",
          mode == ChooserRequest::Mode::Open ? (request.directory ? "select folder" : "open") : "save", qPrintable(title),
          qPrintable(request.currentFolder), int(request.filters.size()), request.multiple ? ", multiple" : "");

    auto *window = new MainWindow(request.currentFolder);
    window->setAttribute(Qt::WA_DeleteOnClose);
    auto *closer = new PortalRequest(this);
    const QString handlePath = handle.path();
    bus.registerObject(handlePath, closer, QDBusConnection::ExportAllSlots);
    ++m_open;

    QPointer<MainWindow> guard(window);
    connect(closer, &PortalRequest::closeRequested, this, [guard] {
        if (guard)
            guard->finishChooser(false);
    });
    window->setChooser(request, [this, call, bus, request, closer, handlePath](const ChooserResult &result) mutable {
        const uint response = result.accepted ? 0 : 1;
        const QVariantMap results = service::resultsFromChooser(request, result);
        bus.send(call.createReply({QVariant::fromValue(response), QVariant::fromValue(results)}));
        bus.unregisterObject(handlePath);
        closer->deleteLater();
        --m_open;
    });
    showWindow(window, {});
}

void PortalRequest::Close() { emit closeRequested(); }

int service::run() {
    registerTypes();
    QApplication::setQuitOnLastWindowClosed(false);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning("scout --service: no session bus");
        return 1;
    }
    auto *fileManager = new FileManager1(qApp);
    auto *portal = new FileChooserPortal(qApp);
    auto own = [&](const char *name, const char *path, QObject *object) {
        if (!bus.registerObject(QString::fromLatin1(path), object, QDBusConnection::ExportAllSlots))
            return false;
        const auto reply = bus.interface()->registerService(QString::fromLatin1(name), QDBusConnectionInterface::DontQueueService,
                                                            QDBusConnectionInterface::AllowReplacement);
        if (reply.value() != QDBusConnectionInterface::ServiceRegistered) {
            qWarning("scout --service: %s is owned by another program", name);
            bus.unregisterObject(QString::fromLatin1(path));
            return false;
        }
        return true;
    };
    const bool ownsFileManager = own(kFileManagerName, "/org/freedesktop/FileManager1", fileManager);
    const bool ownsPortal = own(kPortalName, "/org/freedesktop/portal/desktop", portal);
    if (!ownsFileManager && !ownsPortal)
        return 1;

    // D-Bus activation brings the service back when it is needed again.
    auto *idle = new QTimer(qApp);
    idle->setInterval(60 * 1000);
    QObject::connect(idle, &QTimer::timeout, qApp, [portal, idleMinutes = 0]() mutable {
        const QWidgetList windows = QApplication::topLevelWidgets();
        const bool busy = portal->openDialogs() > 0 ||
                          std::any_of(windows.begin(), windows.end(), [](QWidget *w) { return w->isVisible(); });
        idleMinutes = busy ? 0 : idleMinutes + 1;
        if (idleMinutes >= 10)
            QCoreApplication::quit();
    });
    idle->start();
    return QApplication::exec();
}
