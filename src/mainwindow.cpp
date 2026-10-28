#include "mainwindow.h"

#include "desktopentry.h"
#include "fuzzy.h"
#include "launcher.h"
#include "logging.h"
#include "state.h"
#include "theme.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cerrno>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace {

QString parentDir(const QString &path) {
    if (path.isEmpty() || path == QLatin1String("/"))
        return {};
    const int slash = int(path.lastIndexOf(u'/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

QString baseName(const QString &path) { return path.mid(path.lastIndexOf(u'/') + 1); }

QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + u'/'))
        return u'~' + path.mid(home.size());
    return path;
}

QString plural(int n, const char *word) {
    return QStringLiteral("%1 %2%3").arg(n).arg(QLatin1String(word)).arg(n == 1 ? "" : "s");
}

bool parseBool(const QString &value, bool current) {
    const QString v = value.toLower();
    if (v == QLatin1String("toggle") || v.isEmpty())
        return !current;
    return v == QLatin1String("true") || v == QLatin1String("on") || v == QLatin1String("yes") ||
           v == QLatin1String("1");
}

// Renames without ever replacing an existing entry, dangling symlinks included.
QString renamePath(const QString &from, const QString &to) {
    struct stat st {};
    if (lstat(QFile::encodeName(to).constData(), &st) == 0)
        return QStringLiteral("%1 already exists").arg(baseName(to));
    const int err = renameNoReplace(from, to);
    if (err == EEXIST)
        return QStringLiteral("%1 already exists").arg(baseName(to));
    return err == 0 ? QString() : QStringLiteral("rename failed: %1").arg(QString::fromLocal8Bit(strerror(err)));
}

qint64 treeSize(const QString &path) {
    struct stat st {};
    const QByteArray enc = QFile::encodeName(path);
    if (lstat(enc.constData(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode) ? st.st_size : 0;
    qint64 total = 0;
    QString err;
    for (const FileEntry &e : listDirectory(path, &err))
        total += e.isSymlink ? 0 : treeSize(e.path);
    return total;
}

} // namespace

FileStamp fileStamp(const QString &path) {
    struct statx sx {};
    if (statx(AT_FDCWD, QFile::encodeName(path).constData(), AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_BTIME | STATX_CTIME, &sx) != 0)
        return {};
    // Filesystems without birth times fall back to the change time.
    const statx_timestamp t = (sx.stx_mask & STATX_BTIME) ? sx.stx_btime : sx.stx_ctime;
    return {makedev(sx.stx_dev_major, sx.stx_dev_minor), sx.stx_ino, qint64(t.tv_sec) * 1000000000 + t.tv_nsec};
}

MainWindow::MainWindow(const QString &startPath, bool loadUserConfig, QWidget *parent)
    : QWidget(parent), m_loadUserConfig(loadUserConfig) {
    setWindowTitle(QStringLiteral("scout"));
    setFocusPolicy(Qt::StrongFocus);
    resize(1200, 760);

    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(12);
    connect(&m_previewTimer, &QTimer::timeout, this, &MainWindow::updatePreview);
    m_whichKeyTimer.setSingleShot(true);
    m_whichKeyTimer.setInterval(350);
    connect(&m_whichKeyTimer, &QTimer::timeout, this, &MainWindow::showWhichKey);
    m_ambiguityTimer.setSingleShot(true);
    m_ambiguityTimer.setInterval(600);
    connect(&m_ambiguityTimer, &QTimer::timeout, this, [this] {
        const KeyMap::Result r = m_keys.flush();
        hideWhichKey();
        if (r.kind == KeyMap::Result::Command)
            runFromKeys(r);
        updateStatus();
    });
    m_finderTimer.setSingleShot(true);
    m_finderTimer.setInterval(80);
    connect(&m_finderTimer, &QTimer::timeout, this, &MainWindow::updateFinderList);
    m_renamePoll.setInterval(250);
    m_tabs.append(Tab{});

    buildUi();
    registerCommands();
    bindDefaults();
    if (m_loadUserConfig)
        loadConfig();
    applyTheme();
    connect(&Theme::instance(), &Theme::changed, this, &MainWindow::applyTheme);
    // Clicks land on child widgets, so watch them all to drop half-typed key sequences.
    QCoreApplication::instance()->installEventFilter(this);

    QString start = startPath.isEmpty() ? QDir::currentPath() : startPath;
    if (start.startsWith(QLatin1String("file://")))
        start = QUrl(start).toLocalFile();
    start = expandHome(start);
    if (!cd(start, {}, false)) {
        cd(QDir::homePath(), {}, false);
        error(QStringLiteral("could not open %1").arg(startPath));
    }
}

MainWindow::~MainWindow() {
    QCoreApplication::instance()->removeEventFilter(this);
    m_walker.cancel();
    if (auto done = std::exchange(m_chooserDone, {}))
        done(ChooserResult{});
}

void MainWindow::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_header = new HeaderBar(this);
    m_parentModel = new DirModel(&m_marks, this);
    m_currentModel = new DirModel(&m_marks, this);
    m_parentView = new ColumnView(m_parentModel, ColumnView::Role::Parent, this);
    m_currentView = new ColumnView(m_currentModel, ColumnView::Role::Current, this);
    m_preview = new Preview(&m_marks, this);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(1);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_parentView);
    m_splitter->addWidget(m_currentView);
    m_splitter->addWidget(m_preview);
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 5);
    m_splitter->setStretchFactor(2, 7);
    m_splitter->setSizes({160, 420, 620});
    m_splitter->setFocusPolicy(Qt::NoFocus);

    m_bottom = new QStackedWidget(this);
    m_status = new StatusBar(m_bottom);
    m_prompt = new Prompt(m_bottom);
    m_bottom->addWidget(m_status);
    m_bottom->addWidget(m_prompt);

    layout->addWidget(m_header);
    layout->addWidget(m_splitter, 1);
    layout->addWidget(m_bottom);

    m_picker = new PickerList(this);
    m_whichKey = new WhichKey(this);
    m_tasksView = new TasksView(&m_tasks, this);
    m_properties = new PropertiesView(this);

    connect(m_header, &HeaderBar::tabClicked, this, [this](int index) {
        saveTab();
        loadTab(index);
    });
    connect(m_currentView->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onCursorMoved);
    connect(m_currentModel, &DirModel::refreshed, this, &MainWindow::onCurrentRefreshed);
    connect(m_parentModel, &DirModel::refreshed, this, &MainWindow::onParentRefreshed);

    connect(m_parentView, &ColumnView::rowClicked, this, [this](int row) {
        if (const FileEntry *e = m_parentModel->entry(row)) {
            const FileEntry entry = *e;
            if (entry.isDir)
                cd(entry.path);
            else
                cd(parentDir(entry.path), entry.name);
        }
    });
    connect(m_currentView, &ColumnView::rowActivated, this, [this](int row) {
        setCursor(row);
        endVisual();
        enter();
    });
    auto previewEntry = [this](int row) -> std::optional<FileEntry> {
        const FileEntry *e = m_preview->dirModel()->entry(row);
        return e ? std::optional<FileEntry>(*e) : std::nullopt;
    };
    connect(m_preview->dirView(), &ColumnView::rowClicked, this, [this, previewEntry](int row) {
        if (auto e = previewEntry(row))
            cd(parentDir(e->path), e->name);
    });
    connect(m_preview->dirView(), &ColumnView::rowActivated, this, [this, previewEntry](int row) {
        if (auto e = previewEntry(row)) {
            if (e->isDir) {
                cd(e->path);
            } else {
                cd(parentDir(e->path), e->name);
                launcher::openUrl(QUrl::fromLocalFile(e->path));
            }
        }
    });
    for (ColumnView *view : {m_parentView, m_currentView, m_preview->dirView()}) {
        connect(view, &ColumnView::dropRequested, this,
                [this](const QStringList &paths, const QString &target, Qt::DropAction action) {
                    const Task::Kind kind = action == Qt::MoveAction   ? Task::Move
                                            : action == Qt::LinkAction ? Task::Link
                                                                       : Task::Copy;
                    startTask(kind, paths, target);
                });
    }
    connect(m_currentView, &ColumnView::markToggleRequested, this, [this](int row) {
        endVisual();
        if (const FileEntry *e = m_currentModel->entry(row))
            m_marks.toggle(e->path);
    });
    connect(m_currentView, &ColumnView::markRangeRequested, this, [this](int row) {
        endVisual();
        const int from = std::max(0, cursorRow());
        QStringList paths;
        for (int r = std::min(from, row); r <= std::max(from, row); ++r)
            if (const FileEntry *e = m_currentModel->entry(r))
                paths << e->path;
        m_marks.add(paths);
        setCursor(row);
    });
    connect(m_currentView, &ColumnView::contextMenuRequested, this, &MainWindow::showContextMenu);

    connect(m_prompt, &Prompt::closed, this, [this] {
        m_bottom->setCurrentWidget(m_status);
        if (m_finderActive) {
            m_finderActive = false;
            m_walker.cancel();
        }
        setFocus(Qt::OtherFocusReason);
    });
    connect(m_picker, &PickerList::activated, this, [this](int index) {
        m_picker->setSelected(index);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(m_prompt->edit(), &enter);
    });

    connect(&m_tasks, &TaskManager::taskFinished, this, &MainWindow::onTaskFinished);
    connect(&m_tasks, &TaskManager::tasksChanged, this, &MainWindow::updateStatus);
    connect(&m_marks, &Marks::changed, this, &MainWindow::updateStatus);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (!m_register.paths.isEmpty() && !QGuiApplication::clipboard()->ownsClipboard()) {
            m_register = {};
            updateStatus();
        }
    });

    connect(&m_walker, &FileWalker::batch, this, [this](const QStringList &paths) {
        m_finderItems << paths;
        if (!m_finderTimer.isActive())
            m_finderTimer.start();
    });
    connect(&m_walker, &FileWalker::finished, this, [this](bool truncated) {
        m_finderTruncated = truncated;
        updateFinderList();
    });
}

void MainWindow::applyTheme() {
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    auto style = [&](ColumnView *view, const QColor &bg) {
        QPalette p = view->palette();
        p.setColor(QPalette::Base, bg);
        p.setColor(QPalette::Window, bg);
        view->setPalette(p);
        // The app stylesheet pins the viewport's palette when it polishes it.
        view->viewport()->setPalette(p);
        view->setFont(font);
        view->doItemsLayout();
        view->viewport()->update();
    };
    style(m_parentView, pal.mantle);
    style(m_currentView, pal.base);
    style(m_preview->dirView(), pal.base);
    m_preview->applyTheme();
    m_prompt->applyTheme();
    if (m_chooserBar)
        m_chooserBar->applyTheme();
    const int barHeight = Theme::instance().rowHeight() + 8;
    m_bottom->setFixedHeight(barHeight);
    m_header->setFixedHeight(barHeight);
    QPalette wp = palette();
    wp.setColor(QPalette::Window, pal.crust);
    setPalette(wp);
    setAutoFillBackground(true);
    positionOverlays();
    update();
}

void MainWindow::applyOptions() {
    m_currentModel->setOptions(m_options);
    m_parentModel->setOptions(m_options);
    m_preview->setOptions(m_options);
}

const FileEntry *MainWindow::currentEntry() const {
    if (m_currentModel->path() != m_cwd)
        return nullptr;
    return m_currentModel->entry(cursorRow());
}

int MainWindow::cursorRow() const { return m_currentView->currentRow(); }

bool MainWindow::isIdle() const {
    const bool currentReady = m_currentModel->isLoaded() && !m_currentModel->isLoading() && m_currentModel->path() == m_cwd;
    const bool parentReady = m_parentModel->path().isEmpty() || (m_parentModel->isLoaded() && !m_parentModel->isLoading());
    const bool previewReady = m_preview->kind() != Preview::Kind::Directory ||
                              (m_preview->dirModel()->isLoaded() && !m_preview->dirModel()->isLoading());
    return currentReady && parentReady && previewReady && !m_previewTimer.isActive();
}

QStringList MainWindow::targets() const {
    if (!m_marks.isEmpty())
        return m_marks.paths();
    if (const FileEntry *e = currentEntry())
        return {e->path};
    return {};
}

bool MainWindow::cd(const QString &path, const QString &selectName, bool recordHistory) {
    QString target = expandHome(path.trimmed());
    if (target.startsWith(QLatin1String("file://")))
        target = QUrl(target).toLocalFile();
    if (target.isEmpty())
        return false;
    if (QDir::isRelativePath(target))
        target = QDir(m_cwd.isEmpty() ? QDir::currentPath() : m_cwd).filePath(target);
    target = QDir::cleanPath(target);
    const QFileInfo info(target);
    if (!info.exists()) {
        error(QStringLiteral("no such directory: %1").arg(tilde(target)));
        return false;
    }
    if (!info.isDir())
        return cd(info.absolutePath(), info.fileName(), recordHistory);

    if (target == m_cwd) {
        if (!selectName.isEmpty()) {
            const int row = m_currentModel->rowOf(selectName);
            if (row >= 0 && m_currentModel->isLoaded()) {
                setCursor(row);
            } else {
                m_pendingSelect = selectName;
                m_currentModel->reload();
            }
        }
        return true;
    }
    endVisual();
    if (recordHistory && !m_cwd.isEmpty()) {
        m_back << m_cwd;
        m_forward.clear();
    }
    m_cwd = target;
    m_pendingSelect = selectName;
    m_lastRow = 0;
    // Switching tabs can land on the folder the model already shows.
    const bool alreadyLoaded = m_currentModel->path() == target && m_currentModel->isLoaded();
    m_currentModel->setPath(target);
    if (alreadyLoaded)
        onCurrentRefreshed();
    const QString parent = parentDir(target);
    if (parent.isEmpty()) {
        m_parentModel->clearPath();
    } else {
        const bool already = m_parentModel->path() == parent && m_parentModel->isLoaded();
        m_parentModel->setPath(parent);
        if (already)
            onParentRefreshed();
    }
    if (!isChooser())
        setWindowTitle(QStringLiteral("scout — %1").arg(tilde(target)));
    updateHeader();
    updateStatus();
    updateChooserHint();
    return true;
}

void MainWindow::onCursorMoved() {
    if (const FileEntry *e = currentEntry()) {
        if (m_currentModel->isLoaded()) {
            m_lastSelected[m_cwd] = e->name;
            m_lastRow = cursorRow();
        }
    }
    updateVisual();
    updateHeader();
    updateStatus();
    m_previewTimer.start();
}

void MainWindow::onCurrentRefreshed() {
    if (m_currentModel->path() != m_cwd)
        return;
    const int count = m_currentModel->rowCount();
    const QString want = m_pendingSelect.isEmpty() ? m_lastSelected.value(m_cwd) : m_pendingSelect;
    int row = want.isEmpty() ? -1 : m_currentModel->rowOf(want);
    if (row < 0)
        row = std::clamp(m_lastRow, 0, std::max(0, count - 1));
    m_pendingSelect.clear();
    if (count == 0) {
        m_currentView->setCurrentRow(-1);
        onCursorMoved();
    } else if (row == cursorRow()) {
        onCursorMoved();
    } else {
        setCursor(row);
    }
    // A filter that hides the remembered entry shouldn't make us forget it.
    const bool hiddenByFilter = !want.isEmpty() && m_currentModel->rowOf(want) < 0 &&
                                !m_currentModel->filter().isEmpty();
    if (hiddenByFilter)
        m_lastSelected[m_cwd] = want;
    m_previewTimer.stop();
    updatePreview();
}

void MainWindow::onParentRefreshed() {
    if (m_parentModel->path() != parentDir(m_cwd))
        return;
    m_parentView->setCurrentRow(m_parentModel->rowOf(baseName(m_cwd)));
}

void MainWindow::updatePreview() {
    m_preview->setVisible(m_showPreview);
    if (!m_showPreview)
        return;
    const FileEntry *e = currentEntry();
    if (!e) {
        m_preview->clear();
        return;
    }
    if (e->isDir)
        m_preview->setDirCursorHint(m_lastSelected.value(e->path));
    m_preview->showEntry(*e);
}

void MainWindow::updateHeader() {
    const FileEntry *e = currentEntry();
    m_header->setPath(m_cwd, e ? e->name : QString());
    updateTabs();
}

void MainWindow::updateStatus() {
    QStringList left;
    if (const FileEntry *e = currentEntry()) {
        left << permissionString(*e);
        if (!e->isDir)
            left << humanSize(e->size);
        left << QDateTime::fromSecsSinceEpoch(e->mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        if (e->isSymlink)
            left << QStringLiteral("→ ") + e->linkTarget;
    }
    m_status->setLeft(left);
    m_status->setMode(m_visual ? QStringLiteral("VISUAL") : QString());

    QStringList right;
    if (m_keys.isPending())
        right << m_keys.pendingDisplay();
    if (!m_currentModel->filter().isEmpty())
        right << QStringLiteral("filter: %1").arg(m_currentModel->filter());
    if (m_options.sort != ViewOptions::Name || m_options.reverse) {
        static const char *names[] = {"name", "size", "modified", "extension"};
        right << QStringLiteral("sort: %1%2").arg(QLatin1String(names[m_options.sort])).arg(m_options.reverse ? " ↑" : "");
    }
    if (m_options.showHidden)
        right << QStringLiteral("hidden");
    if (!m_marks.isEmpty())
        right << QStringLiteral("%1 marked").arg(m_marks.size());
    if (!m_register.paths.isEmpty())
        right << QStringLiteral("%1 %2").arg(m_register.paths.size()).arg(m_register.cut ? "cut" : "copied");
    const int active = m_tasks.activeCount();
    if (active > 0) {
        double sum = 0;
        for (Task *t : m_tasks.tasks())
            if (!t->isFinished())
                sum += t->fraction();
        right << QStringLiteral("%1 %2%").arg(plural(active, "task")).arg(qRound(100 * sum / active));
    }
    const int count = m_currentModel->rowCount();
    right << QStringLiteral("%1/%2").arg(count == 0 ? 0 : cursorRow() + 1).arg(count);
    m_status->setRight(right);
    m_status->setDragItems(m_marks.size(), [this] { return m_marks.paths(); });
}

void MainWindow::message(const QString &text, StatusBar::Level level) {
    if (level == StatusBar::Level::Error)
        logging::error(text);
    else if (level == StatusBar::Level::Warning)
        logging::warning(text);
    m_status->showMessage(text, level, level == StatusBar::Level::Error ? 7000 : 3500);
}

void MainWindow::setCursor(int row) {
    const int count = m_currentModel->rowCount();
    if (count == 0) {
        m_currentView->setCurrentRow(-1);
        return;
    }
    m_currentView->setCurrentRow(std::clamp(row, 0, count - 1));
}

void MainWindow::moveCursor(int delta) { setCursor(std::max(0, cursorRow()) + delta); }

void MainWindow::enter() {
    const FileEntry *e = currentEntry();
    if (!e)
        return;
    if (e->isDir)
        cd(e->path);
    else
        openTargets();
}

void MainWindow::leave() {
    const QString parent = parentDir(m_cwd);
    if (!parent.isEmpty())
        cd(parent, baseName(m_cwd));
}

void MainWindow::openTargets() {
    QStringList paths = targets();
    if (paths.size() == 1 && QFileInfo(paths.first()).isDir() && !(isChooser() && m_chooser.multiple && !m_marks.isEmpty())) {
        cd(paths.first());
        return;
    }
    if (isChooser()) {
        chooserAccept(true);
        return;
    }
    paths.erase(std::remove_if(paths.begin(), paths.end(), [](const QString &p) { return QFileInfo(p).isDir(); }),
                paths.end());
    if (paths.isEmpty())
        return;
    QMimeDatabase db;
    QHash<QString, QStringList> byApp;
    QStringList unhandled;
    for (const QString &p : paths) {
        const auto apps = DesktopApps::instance().appsFor({db.mimeTypeForFile(p).name()});
        if (apps.isEmpty())
            unhandled << p;
        else
            byApp[apps.first().id] << p;
    }
    QStringList names;
    for (auto it = byApp.begin(); it != byApp.end(); ++it) {
        const DesktopApp *app = DesktopApps::instance().find(it.key());
        QString err;
        if (app && DesktopApps::launch(*app, it.value(), m_cwd, &err))
            names << app->name;
        else
            unhandled << it.value();
    }
    for (const QString &p : unhandled)
        launcher::openUrl(QUrl::fromLocalFile(p));
    if (!names.isEmpty())
        message(QStringLiteral("opened with %1").arg(names.join(QStringLiteral(", "))));
}

void MainWindow::openWith(const DesktopApp &app, const QStringList &paths) {
    QString err;
    if (DesktopApps::launch(app, paths, m_cwd, &err))
        message(QStringLiteral("opened with %1").arg(app.name));
    else
        error(err);
}

void MainWindow::showOpenWith() {
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    QMimeDatabase db;
    QStringList mimes;
    for (const QString &p : paths) {
        const QString m = QFileInfo(p).isDir() ? QStringLiteral("inode/directory") : db.mimeTypeForFile(p).name();
        if (!mimes.contains(m))
            mimes << m;
    }
    const QList<DesktopApp> recommended = DesktopApps::instance().appsFor(mimes);
    QList<DesktopApp> everything = DesktopApps::instance().all();
    QSet<QString> seen;
    for (const DesktopApp &a : recommended)
        seen.insert(a.id);

    const QString label = paths.size() == 1 ? baseName(paths.first()) : plural(int(paths.size()), "item");
    auto rebuild = [this, recommended, everything, seen, label](const QString &query) {
        QList<std::pair<int, PickerItem>> scored;
        auto consider = [&](const DesktopApp &a, bool rec, int order) {
            fuzzy::Match m;
            if (!query.isEmpty() && !fuzzy::match(query, a.name, &m))
                return;
            PickerItem item;
            item.text = a.name;
            item.description = rec ? (order == 0 ? QStringLiteral("default") : QStringLiteral("recommended"))
                                   : QString();
            item.hint = a.id;
            item.highlights = m.positions;
            item.data = a.id;
            const int score = query.isEmpty() ? -order : m.score + (rec ? 40 : 0);
            scored.append({score, item});
        };
        int order = 0;
        for (const DesktopApp &a : recommended)
            consider(a, true, order++);
        for (const DesktopApp &a : everything)
            if (!seen.contains(a.id))
                consider(a, false, order++);
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        QList<PickerItem> items;
        for (const auto &s : scored)
            items << s.second;
        m_picker->setItems(items);
        m_picker->setHeader(QStringLiteral("open %1 with").arg(label));
        m_picker->setFooter(QStringLiteral("↑↓ move · Enter open · Esc close"));
        positionOverlays();
    };
    PromptSpec spec;
    spec.label = QStringLiteral("open with:");
    spec.showList = true;
    spec.onChange = rebuild;
    spec.onAccept = [this, paths](const QString &, const PickerItem *item) {
        if (!item)
            return;
        if (const DesktopApp *app = DesktopApps::instance().find(item->data.toString()))
            openWith(*app, paths);
    };
    openPrompt(spec);
}

void MainWindow::openPrompt(PromptSpec spec) {
    hideWhichKey();
    m_keys.reset();
    m_bottom->setCurrentWidget(m_prompt);
    const bool list = spec.showList;
    m_prompt->open(std::move(spec), list ? m_picker : nullptr);
    positionOverlays();
}

bool MainWindow::event(QEvent *event) {
    if (event->type() == QEvent::WindowDeactivate && m_whichKey && (m_keys.isPending() || m_whichKey->isVisible()))
        cancelPendingKeys();
    if (event->type() == QEvent::KeyPress && !m_prompt->isOpen()) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool plainTab = (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
                              !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
        if (plainTab && m_chooserBar && m_chooserBar->nameEdit()) {
            m_chooserBar->nameEdit()->setFocus(Qt::TabFocusReason);
            return true;
        }
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
    }
    return QWidget::event(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (m_prompt->isOpen())
        return;
    if (m_overlay != Overlay::None) {
        handleOverlayKey(event);
        return;
    }
    const QString token = KeyMap::tokenFor(event);
    if (token.isEmpty())
        return;
    if (token == QLatin1String("<Esc>")) {
        if (m_keys.isPending())
            cancelPendingKeys();
        else if (!handleEscape() && isChooser())
            finishChooser(false);
        return;
    }
    const KeyMap::Result result = m_keys.feed(token);
    switch (result.kind) {
    case KeyMap::Result::Command:
        m_ambiguityTimer.stop();
        hideWhichKey();
        runFromKeys(result);
        break;
    case KeyMap::Result::Pending:
        if (m_keys.hasAmbiguousExact())
            m_ambiguityTimer.start();
        // A bare count ("12") would list every binding; the status bar shows it instead.
        if (m_whichKey->isVisible())
            showWhichKey();
        else if (m_keys.hasPendingKeys() && !m_keys.continuations().isEmpty())
            m_whichKeyTimer.start();
        break;
    case KeyMap::Result::None:
        m_ambiguityTimer.stop();
        hideWhichKey();
        break;
    }
    updateStatus();
}

bool MainWindow::handleEscape() {
    bool handled = true;
    if (m_visual) {
        endVisual();
    } else if (!m_currentModel->filter().isEmpty()) {
        m_currentModel->setFilter({});
        message(QStringLiteral("filter cleared"));
    } else if (!m_marks.isEmpty()) {
        m_marks.clear();
    } else if (!m_search.isEmpty()) {
        m_search.clear();
    } else if (!m_register.paths.isEmpty()) {
        m_register = {};
        message(QStringLiteral("clipboard selection dropped"));
    } else {
        handled = false;
    }
    updateStatus();
    return handled;
}

void MainWindow::showWhichKey() {
    const QList<Continuation> items = m_keys.continuations();
    if (items.isEmpty() || !m_keys.hasPendingKeys()) {
        hideWhichKey();
        return;
    }
    m_whichKey->setContinuations(m_keys.pendingDisplay(), items);
    m_whichKey->show();
    m_whichKey->raise();
    positionOverlays();
}

void MainWindow::hideWhichKey() {
    m_whichKeyTimer.stop();
    m_whichKey->hide();
}

void MainWindow::cancelPendingKeys() {
    m_keys.reset();
    m_ambiguityTimer.stop();
    hideWhichKey();
    updateStatus();
}

void MainWindow::startVisual() {
    const FileEntry *e = currentEntry();
    if (!e)
        return;
    m_visual = true;
    m_visualAnchor = e->name;
    m_visualAnchorRow = cursorRow();
    m_visualBase = m_marks.pathSet();
    updateVisual();
    updateStatus();
}

void MainWindow::endVisual() {
    if (!m_visual)
        return;
    m_visual = false;
    m_visualBase.clear();
    updateStatus();
}

void MainWindow::updateVisual() {
    if (!m_visual || m_currentModel->path() != m_cwd || !m_currentModel->isLoaded())
        return;
    QSet<QString> marks = m_visualBase;
    const int count = m_currentModel->rowCount();
    if (count > 0) {
        // The anchor follows its entry across reloads; if it is gone, its old row stands in.
        int anchor = m_currentModel->rowOf(m_visualAnchor);
        if (anchor < 0)
            anchor = std::clamp(m_visualAnchorRow, 0, count - 1);
        const int cursor = std::clamp(cursorRow(), 0, count - 1);
        for (int r = std::min(anchor, cursor); r <= std::max(anchor, cursor); ++r)
            marks.insert(m_currentModel->entry(r)->path);
    }
    m_marks.set(marks);
}

void MainWindow::handleOverlayKey(QKeyEvent *event) {
    const QString token = KeyMap::tokenFor(event);
    if (m_overlay == Overlay::Tasks) {
        if (token == QLatin1String("j") || token == QLatin1String("<Down>"))
            m_tasksView->moveSelection(1);
        else if (token == QLatin1String("k") || token == QLatin1String("<Up>"))
            m_tasksView->moveSelection(-1);
        else if (token == QLatin1String("x")) {
            if (Task *t = m_tasksView->selectedTask())
                t->cancel();
        } else if (token == QLatin1String("c"))
            m_tasks.clearFinished();
        else if (token == QLatin1String("<Esc>") || token == QLatin1String("q") || token == QLatin1String("w"))
            closeOverlay();
        positionOverlays();
        return;
    }
    if (m_overlay == Overlay::Properties &&
        (token == QLatin1String("<Esc>") || token == QLatin1String("q") || token == QLatin1String("<A-CR>") ||
         token == QLatin1String("<CR>")))
        closeOverlay();
}

void MainWindow::closeOverlay() {
    m_overlay = Overlay::None;
    m_tasksView->hide();
    m_properties->hide();
    m_propertiesPath.clear();
}

void MainWindow::showTasks() {
    hideWhichKey();
    closeOverlay();
    m_overlay = Overlay::Tasks;
    m_tasksView->show();
    m_tasksView->raise();
    positionOverlays();
}

void MainWindow::showProperties() {
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    closeOverlay();
    QList<QPair<QString, QString>> rows;
    QString title;
    QStringList sizeTargets = paths;
    if (paths.size() == 1) {
        const FileEntry e = statEntry(paths.first());
        QMimeDatabase db;
        const QMimeType mime = e.isDir ? db.mimeTypeForName(QStringLiteral("inode/directory")) : db.mimeTypeForFile(e.path);
        title = e.name;
        rows << QPair{QStringLiteral("Type"), QStringLiteral("%1 (%2)").arg(mime.comment(), mime.name())};
        rows << QPair{QStringLiteral("Location"), tilde(parentDir(e.path))};
        rows << QPair{QStringLiteral("Size"), e.isDir ? QStringLiteral("computing…") : QStringLiteral("%1 (%2 bytes)").arg(humanSize(e.size)).arg(e.size)};
        if (e.isDir) {
            QString err;
            rows << QPair{QStringLiteral("Contains"), plural(int(listDirectory(e.path, &err).size()), "item")};
        }
        rows << QPair{QStringLiteral("Modified"), QDateTime::fromSecsSinceEpoch(e.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))};
        rows << QPair{QStringLiteral("Permissions"), QStringLiteral("%1 (%2)").arg(permissionString(e)).arg(e.mode & 07777, 4, 8, QLatin1Char('0'))};
        const passwd *pw = getpwuid(e.uid);
        const group *gr = getgrgid(e.gid);
        rows << QPair{QStringLiteral("Owner"), QStringLiteral("%1:%2").arg(pw ? QString::fromLocal8Bit(pw->pw_name) : QString::number(e.uid),
                                                                          gr ? QString::fromLocal8Bit(gr->gr_name) : QString::number(e.gid))};
        if (e.isSymlink)
            rows << QPair{QStringLiteral("Link target"), e.linkTarget};
        if (!e.isDir)
            sizeTargets.clear();
    } else {
        title = plural(int(paths.size()), "item");
        rows << QPair{QStringLiteral("Size"), QStringLiteral("computing…")};
        rows << QPair{QStringLiteral("Location"), tilde(parentDir(paths.first()))};
    }
    m_properties->setRows(title, rows);
    m_overlay = Overlay::Properties;
    m_propertiesPath = paths.join(u'\n');
    m_properties->show();
    m_properties->raise();
    positionOverlays();
    if (sizeTargets.isEmpty())
        return;
    const QString key = m_propertiesPath;
    QPointer<MainWindow> self(this);
    QtConcurrent::run([sizeTargets] {
        qint64 total = 0;
        for (const QString &p : sizeTargets)
            total += treeSize(p);
        return total;
    }).then(this, [self, key](qint64 total) {
        if (self && self->m_propertiesPath == key)
            self->m_properties->updateRow(QStringLiteral("Size"),
                                          QStringLiteral("%1 (%2 bytes)").arg(humanSize(total)).arg(total));
    });
}

void MainWindow::positionOverlays() {
    if (!m_bottom)
        return;
    const int margin = 8;
    const int top = m_header->geometry().bottom() + 4;
    const int bottom = m_bottom->geometry().top() - 4;
    if (m_picker->isVisible()) {
        const int h = std::min(m_picker->preferredHeight(), int(height() * 0.6));
        m_picker->setGeometry(margin, bottom - h, width() - 2 * margin, h);
        m_picker->raise();
    }
    if (m_whichKey->isVisible()) {
        const QSize s = m_whichKey->preferredSize(std::max(1, width() - 2 * margin), std::max(1, bottom - top));
        m_whichKey->setGeometry(std::max(0, width() - s.width() - margin), std::max({0, top, bottom - s.height()}),
                                s.width(), s.height());
    }
    if (m_tasksView->isVisible()) {
        const QSize s = m_tasksView->preferredSize(width() - 4 * margin, height() - 120);
        m_tasksView->setGeometry((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
    }
    if (m_properties->isVisible()) {
        const QSize s = m_properties->preferredSize(width() - 4 * margin);
        m_properties->setGeometry((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    positionOverlays();
}

void MainWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    const int w = std::max(width(), 600);
    m_splitter->setSizes({int(w * 0.14), int(w * 0.36), int(w * 0.5)});
    setFocus(Qt::OtherFocusReason);
}

void MainWindow::yank(bool cut) {
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    m_register = {paths, cut};
    setClipboardFiles(paths, cut);
    m_marks.clear();
    message(QStringLiteral("%1 %2").arg(cut ? "cut" : "copied", plural(int(paths.size()), "item")));
    updateStatus();
}

void MainWindow::paste(Task::Kind kind) {
    ClipboardFiles files = clipboardFiles();
    if (files.paths.isEmpty())
        files = m_register;
    if (files.paths.isEmpty()) {
        error(QStringLiteral("nothing to paste"));
        return;
    }
    Task::Kind k = kind;
    if (kind != Task::Link)
        k = files.cut ? Task::Move : Task::Copy;
    startTask(k, files.paths, m_cwd);
    if (k == Task::Move) {
        m_register = {};
        if (QGuiApplication::clipboard()->ownsClipboard())
            QGuiApplication::clipboard()->clear();
        updateStatus();
    }
}

void MainWindow::startTask(Task::Kind kind, const QStringList &paths, const QString &target) {
    if (paths.isEmpty())
        return;
    Task *task = m_tasks.start(kind, paths, target);
    message(task->title() + QStringLiteral("…"));
}

void MainWindow::onTaskFinished(Task *task) {
    const Task::Progress p = task->progress();
    static const char *done[] = {"copied", "moved", "linked", "trashed", "deleted", "restored"};
    const QString summary = QStringLiteral("%1 %2").arg(QLatin1String(done[task->kind()]),
                                                        plural(int(task->sources().size()), "item"));
    switch (p.state) {
    case Task::Done:
        message(summary, StatusBar::Level::Success);
        break;
    case Task::Failed:
        logging::error(QStringLiteral("task failed: %1 (%2)\n%3")
                           .arg(task->title(), task->sources().join(QStringLiteral(", ")), p.errors.join(u'\n')));
        error(QStringLiteral("%1 failed: %2%3")
                  .arg(task->verb().toLower(), p.errors.value(0))
                  .arg(p.errors.size() > 1 ? QStringLiteral(" (+%1 more, see tasks: w)").arg(p.errors.size() - 1) : QString()));
        break;
    case Task::Cancelled:
        message(QStringLiteral("%1 cancelled").arg(task->verb().toLower()), StatusBar::Level::Warning);
        break;
    case Task::Running:
        break;
    }
    if (!m_undoTasks.remove(task)) {
        const QString what = task->sources().size() == 1 ? baseName(task->sources().first())
                                                         : plural(int(task->sources().size()), "item");
        switch (task->kind()) {
        case Task::Move:
        case Task::Trash:
            if (!p.moved.isEmpty())
                pushUndo({UndoStep::Moves, QStringLiteral("%1 %2").arg(QLatin1String(done[task->kind()]), what), p.moved});
            break;
        case Task::Copy:
        case Task::Link:
        case Task::Restore: {
            QList<QPair<QString, QString>> created;
            for (const QString &c : p.created)
                created.append({QString(), c});
            if (!created.isEmpty())
                pushUndo({UndoStep::Restored, QStringLiteral("%1 %2").arg(QLatin1String(done[task->kind()]), what), created});
            break;
        }
        case Task::Delete:
            break;
        }
    }
    if (!p.created.isEmpty() && parentDir(p.created.first()) == m_cwd)
        m_pendingSelect = baseName(p.created.first());
    m_currentModel->reload();
    if (!m_parentModel->path().isEmpty())
        m_parentModel->reload();
    if (m_preview->kind() == Preview::Kind::Directory)
        m_preview->dirModel()->reload();
}

void MainWindow::promptRename() {
    const FileEntry *e = currentEntry();
    if (!e)
        return;
    const QString oldName = e->name;
    PromptSpec spec;
    spec.label = QStringLiteral("rename:");
    spec.text = oldName;
    const int dot = int(oldName.lastIndexOf(u'.'));
    spec.selectionStart = 0;
    spec.selectionLength = (!e->isDir && dot > 0) ? dot : int(oldName.size());
    spec.onAccept = [this, oldName](const QString &text, const PickerItem *) {
        const QString name = text.trimmed();
        if (name.isEmpty() || name == oldName)
            return;
        const QString from = QDir(m_cwd).filePath(oldName);
        const QString to = QDir::cleanPath(QDir(m_cwd).filePath(expandHome(name)));
        if (const QString problem = renamePath(from, to); !problem.isEmpty()) {
            error(problem);
            return;
        }
        pushUndo({UndoStep::Renames, QStringLiteral("rename %1").arg(oldName), {{from, to}}});
        if (parentDir(to) == m_cwd) {
            m_lastSelected[m_cwd] = baseName(to);
            m_pendingSelect = baseName(to);
        }
        m_currentModel->reload();
        message(QStringLiteral("renamed to %1").arg(name), StatusBar::Level::Success);
    };
    openPrompt(spec);
}

void MainWindow::promptCreate(const QString &initial) {
    PromptSpec spec;
    spec.label = QStringLiteral("create (end with / for a folder):");
    spec.text = initial;
    spec.onAccept = [this](const QString &text, const PickerItem *) { createPath(text); };
    openPrompt(spec);
}

void MainWindow::createPath(const QString &input) {
    const QString name = expandHome(input.trimmed());
    if (name.isEmpty())
        return;
    const bool folder = name.endsWith(u'/');
    const QString path = QDir::cleanPath(QDir(m_cwd).filePath(name));
    if (QFileInfo::exists(path)) {
        error(QStringLiteral("%1 already exists").arg(tilde(path)));
        return;
    }
    bool ok = false;
    if (folder) {
        ok = QDir().mkpath(path);
    } else {
        QDir().mkpath(parentDir(path));
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!ok) {
        error(QStringLiteral("could not create %1").arg(tilde(path)));
        return;
    }
    QString first = path;
    while (parentDir(first) != m_cwd && !parentDir(first).isEmpty() && first.startsWith(m_cwd + u'/'))
        first = parentDir(first);
    if (parentDir(first) == m_cwd)
        m_pendingSelect = baseName(first);
    m_currentModel->reload();
    message(QStringLiteral("created %1").arg(tilde(path)), StatusBar::Level::Success);
}

void MainWindow::confirmDelete() {
    const QStringList paths = targets();
    if (paths.isEmpty())
        return;
    PromptSpec spec;
    spec.label = QStringLiteral("permanently delete %1? [y/N]")
                     .arg(paths.size() == 1 ? baseName(paths.first()) : plural(int(paths.size()), "item"));
    spec.onChange = [this, paths](const QString &text) {
        if (text.isEmpty())
            return;
        m_prompt->close(true);
        if (text == QLatin1String("y") || text == QLatin1String("Y")) {
            m_marks.clear();
            startTask(Task::Delete, paths, {});
        } else {
            message(QStringLiteral("delete cancelled"));
        }
    };
    spec.onAccept = [this](const QString &, const PickerItem *) { message(QStringLiteral("delete cancelled")); };
    openPrompt(spec);
}

void MainWindow::promptFilter() {
    PromptSpec spec;
    spec.label = QStringLiteral("filter:");
    spec.text = m_currentModel->filter();
    spec.onChange = [this](const QString &text) { m_currentModel->setFilter(text); };
    spec.onCancel = [this] { m_currentModel->setFilter({}); };
    spec.onAccept = [this](const QString &, const PickerItem *) { updateStatus(); };
    openPrompt(spec);
}

namespace {
bool nameMatches(const QString &name, const QString &query) {
    const bool caseSensitive = std::any_of(query.begin(), query.end(), [](QChar c) { return c.isUpper(); });
    return name.contains(query, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
}
} // namespace

void MainWindow::promptSearch(bool backwards) {
    const int origin = std::max(0, cursorRow());
    PromptSpec spec;
    spec.label = backwards ? QStringLiteral("?") : QStringLiteral("/");
    spec.onChange = [this, origin, backwards](const QString &text) {
        if (text.isEmpty()) {
            setCursor(origin);
            return;
        }
        const int n = m_currentModel->rowCount();
        for (int i = 0; i < n; ++i) {
            const int row = backwards ? ((origin - i) % n + n) % n : (origin + i) % n;
            if (nameMatches(m_currentModel->entry(row)->name, text)) {
                setCursor(row);
                return;
            }
        }
    };
    spec.onCancel = [this, origin] { setCursor(origin); };
    spec.onAccept = [this, backwards](const QString &text, const PickerItem *) {
        m_search = text;
        m_searchBackwards = backwards;
    };
    openPrompt(spec);
}

void MainWindow::searchNext(int direction) {
    if (m_search.isEmpty()) {
        message(QStringLiteral("no search; use / to search"));
        return;
    }
    const int n = m_currentModel->rowCount();
    const int step = m_searchBackwards ? -direction : direction;
    const int start = std::max(0, cursorRow());
    for (int i = 1; i <= n; ++i) {
        const int row = ((start + step * i) % n + n) % n;
        if (nameMatches(m_currentModel->entry(row)->name, m_search)) {
            setCursor(row);
            return;
        }
    }
    error(QStringLiteral("no match for %1").arg(m_search));
}

void MainWindow::promptFind() {
    m_finderRoot = m_cwd;
    m_finderItems.clear();
    m_finderTruncated = false;
    m_walker.start(m_cwd, m_options.showHidden);
    PromptSpec spec;
    spec.label = QStringLiteral("files:");
    spec.showList = true;
    spec.onChange = [this](const QString &) { updateFinderList(); };
    spec.onAccept = [this](const QString &, const PickerItem *item) {
        if (!item)
            return;
        const QString rel = item->data.toString();
        const QString abs = QDir::cleanPath(QDir(m_finderRoot).filePath(rel));
        if (rel.endsWith(u'/'))
            cd(abs);
        else
            cd(parentDir(abs), baseName(abs));
    };
    openPrompt(spec);
    m_finderActive = true;
    updateFinderList();
}

void MainWindow::updateFinderList() {
    if (!m_finderActive || !m_prompt->isOpen())
        return;
    const QString query = m_prompt->text();
    struct Scored {
        int score;
        int index;
        QList<int> positions;
    };
    constexpr int kMax = 200;
    std::vector<Scored> scored;
    if (query.isEmpty()) {
        for (int i = 0; i < m_finderItems.size() && i < kMax; ++i)
            scored.push_back({0, i, {}});
    } else {
        scored.reserve(4096);
        for (int i = 0; i < m_finderItems.size(); ++i) {
            fuzzy::Match m;
            if (fuzzy::match(query, m_finderItems[i], &m))
                scored.push_back({m.score, i, std::move(m.positions)});
        }
        auto better = [this](const Scored &a, const Scored &b) {
            if (a.score != b.score)
                return a.score > b.score;
            return m_finderItems[a.index].size() < m_finderItems[b.index].size();
        };
        const size_t keep = std::min<size_t>(kMax, scored.size());
        std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(), better);
    }
    const int matches = query.isEmpty() ? int(m_finderItems.size()) : int(scored.size());
    QList<PickerItem> items;
    for (size_t i = 0; i < scored.size() && i < size_t(kMax); ++i) {
        const QString &rel = m_finderItems[scored[i].index];
        PickerItem item;
        item.text = rel;
        item.highlights = scored[i].positions;
        FileEntry probe;
        probe.name = rel.endsWith(u'/') ? rel.chopped(1) : rel;
        probe.isDir = rel.endsWith(u'/');
        item.icon = entryKind(probe);
        item.data = rel;
        items << item;
    }
    const int previous = m_picker->selectedIndex();
    const bool keepSelection = query == m_picker->property("lastQuery").toString();
    m_picker->setItems(items);
    if (keepSelection && previous > 0)
        m_picker->setSelected(previous);
    m_picker->setProperty("lastQuery", query);
    QString status;
    if (m_walker.isRunning())
        status = QStringLiteral(" · scanning…");
    else if (m_finderTruncated)
        status = QStringLiteral(" · stopped at limit");
    m_picker->setHeader(QStringLiteral("%1 of %2 files and folders in %3%4")
                            .arg(matches)
                            .arg(m_finderItems.size())
                            .arg(tilde(m_finderRoot), status));
    m_picker->setFooter(QStringLiteral("↑↓ move · Enter go · Esc close"));
    positionOverlays();
}

void MainWindow::promptCommand(const QString &initial) {
    PromptSpec spec;
    spec.label = QStringLiteral(":");
    spec.text = initial;
    spec.showList = true;
    spec.onChange = [this](const QString &text) { updateCommandList(text); };
    spec.onComplete = [](const QString &, const PickerItem *item) -> QString {
        if (!item)
            return {};
        const QString data = item->data.toString();
        if (data.startsWith(QLatin1String("cmd:")))
            return data.mid(4) + u' ';
        if (data.startsWith(QLatin1String("complete:")))
            return data.mid(9);
        return {};
    };
    spec.onAccept = [this](const QString &text, const PickerItem *item) {
        QString line = text.trimmed();
        if (item) {
            const QString data = item->data.toString();
            if (data.startsWith(QLatin1String("cmd:"))) {
                const QString name = data.mid(4);
                const CommandDef *def = m_commands.find(name);
                if (def && def->requiresArgs) {
                    QTimer::singleShot(0, this, [this, name] { promptCommand(name + u' '); });
                    return;
                }
                line = name;
            } else if (data.startsWith(QLatin1String("complete:"))) {
                line = data.mid(9).trimmed();
            }
        }
        if (!line.isEmpty())
            runCommandLine(line);
    };
    openPrompt(spec);
}

void MainWindow::updateCommandList(const QString &text) {
    const QStringList parts = CommandRegistry::split(text);
    const bool trailingSpace = text.endsWith(u' ');
    QList<PickerItem> items;
    if (parts.isEmpty() || (parts.size() == 1 && !trailingSpace)) {
        const QString query = parts.value(0);
        QList<std::pair<int, PickerItem>> scored;
        for (const CommandDef *def : m_commands.all()) {
            fuzzy::Match m;
            if (!query.isEmpty() && !fuzzy::match(query, def->name, &m))
                continue;
            PickerItem item;
            item.text = def->name;
            item.description = def->usage.isEmpty() ? def->description : def->usage + QStringLiteral("  ") + def->description;
            item.hint = m_keys.keysFor(def->name);
            item.highlights = m.positions;
            item.data = QStringLiteral("cmd:") + def->name;
            scored.append({m.score, item});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &s : scored)
            items << s.second;
    } else {
        const QString name = parts.first();
        const QString arg = trailingSpace ? QString() : parts.last();
        const QStringList fixed = parts.mid(1, parts.size() - 1 - (trailingSpace ? 0 : 1));
        auto completion = [&](const QString &value, const QString &description, std::optional<EntryKind> icon,
                              const QList<int> &highlights) {
            PickerItem item;
            item.text = value;
            item.description = description;
            item.icon = icon;
            item.highlights = highlights;
            QStringList line{name};
            for (const QString &f : fixed)
                line << CommandRegistry::quote(f);
            line << CommandRegistry::quote(value);
            item.data = QStringLiteral("complete:") + line.join(u' ');
            items << item;
        };
        QStringList options;
        if (name == QLatin1String("sort"))
            options = {QStringLiteral("name"), QStringLiteral("size"), QStringLiteral("modified"), QStringLiteral("extension")};
        else if (name == QLatin1String("set") && fixed.isEmpty())
            options = {QStringLiteral("hidden"), QStringLiteral("theme"), QStringLiteral("font"), QStringLiteral("font-size"),
                       QStringLiteral("sort"), QStringLiteral("reverse"), QStringLiteral("dirs-first"), QStringLiteral("preview"),
                       QStringLiteral("editor")};
        else if (name == QLatin1String("set") && fixed.value(0) == QLatin1String("theme"))
            options = {QStringLiteral("auto"), QStringLiteral("dark"), QStringLiteral("light")};
        if (!options.isEmpty()) {
            for (const QString &o : options) {
                fuzzy::Match m;
                if (arg.isEmpty() || fuzzy::match(arg, o, &m))
                    completion(o, {}, std::nullopt, m.positions);
            }
        } else if (name == QLatin1String("cd") || name == QLatin1String("mkdir") || name == QLatin1String("touch") ||
                   name == QLatin1String("create")) {
            const QString expanded = expandHome(arg);
            const int slash = int(expanded.lastIndexOf(u'/'));
            const QString dirPart = slash >= 0 ? expanded.left(slash + 1) : QString();
            const QString leaf = slash >= 0 ? expanded.mid(slash + 1) : expanded;
            const QString dir = QDir::cleanPath(QDir(m_cwd).filePath(dirPart.isEmpty() ? QStringLiteral(".") : dirPart));
            QString err;
            QVector<FileEntry> entries = listDirectory(dir, &err);
            std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) { return naturalCompare(a.name, b.name) < 0; });
            const QString shownPrefix = slash >= 0 ? arg.left(arg.lastIndexOf(u'/') + 1) : QString();
            int added = 0;
            for (const FileEntry &e : entries) {
                if (!e.isDir || (e.isHidden() && !leaf.startsWith(u'.') && !m_options.showHidden))
                    continue;
                fuzzy::Match m;
                if (!leaf.isEmpty() && !fuzzy::match(leaf, e.name, &m))
                    continue;
                QList<int> hl;
                for (int p : m.positions)
                    hl << p + int(shownPrefix.size());
                completion(shownPrefix + e.name + u'/', {}, EntryKind::Directory, hl);
                if (++added >= 200)
                    break;
            }
        }
        if (items.isEmpty()) {
            if (const CommandDef *def = m_commands.find(name)) {
                PickerItem item;
                item.text = def->name + (def->usage.isEmpty() ? QString() : u' ' + def->usage);
                item.description = def->description;
                item.data = QStringLiteral("line");
                items << item;
            }
        }
    }
    m_picker->setItems(items);
    m_picker->setHeader({});
    m_picker->setFooter(QStringLiteral("↑↓ move · Tab complete · Enter run · Esc close"));
    positionOverlays();
}

void MainWindow::showHelp() {
    auto rebuild = [this](const QString &query) {
        QList<std::pair<int, PickerItem>> scored;
        for (const Binding &b : m_keys.bindings()) {
            const QString desc = b.description.isEmpty() ? b.command : b.description;
            fuzzy::Match m;
            if (!query.isEmpty() && !fuzzy::match(query, desc + u' ' + b.command, &m))
                continue;
            PickerItem item;
            item.text = desc;
            item.description = b.command == desc ? QString() : b.command;
            item.hint = KeyMap::display(b.keys);
            for (int p : m.positions)
                if (p < desc.size())
                    item.highlights << p;
            item.data = QStringLiteral("complete:") + b.command;
            scored.append({query.isEmpty() ? 0 : m.score, item});
        }
        std::stable_sort(scored.begin(), scored.end(), [&](const auto &a, const auto &b) {
            if (a.first != b.first)
                return a.first > b.first;
            return a.second.text.compare(b.second.text, Qt::CaseInsensitive) < 0;
        });
        QList<PickerItem> items;
        for (const auto &s : scored)
            items << s.second;
        m_picker->setItems(items);
        m_picker->setHeader(QStringLiteral("Key bindings · %1").arg(items.size()));
        m_picker->setFooter(QStringLiteral("type to filter · Enter run · Esc close"));
        positionOverlays();
    };
    PromptSpec spec;
    spec.label = QStringLiteral("help:");
    spec.showList = true;
    spec.onChange = rebuild;
    spec.onAccept = [this](const QString &, const PickerItem *item) {
        if (item)
            runCommandLine(item->data.toString().mid(9));
    };
    openPrompt(spec);
}

void MainWindow::showContextMenu(int row, const QPoint &globalPos) {
    endVisual();
    if (row >= 0)
        setCursor(row);
    QMenu menu(this);
    auto add = [&](const QString &label, const QString &command) {
        const QString keys = m_keys.keysFor(command);
        QAction *action = menu.addAction(keys.isEmpty() ? label : label + u'\t' + keys);
        connect(action, &QAction::triggered, this, [this, command] { runCommandLine(command); });
        return action;
    };
    const FileEntry *e = row >= 0 ? m_currentModel->entry(row) : nullptr;
    if (e) {
        add(QStringLiteral("Open"), QStringLiteral("open"));
        if (!e->isDir) {
            QMenu *with = menu.addMenu(QStringLiteral("Open With"));
            QMimeDatabase db;
            const auto apps = DesktopApps::instance().appsFor({db.mimeTypeForFile(e->path).name()});
            for (const DesktopApp &app : apps.mid(0, 12)) {
                QAction *a = with->addAction(app.name);
                const QString id = app.id;
                connect(a, &QAction::triggered, this, [this, id] {
                    if (const DesktopApp *found = DesktopApps::instance().find(id))
                        openWith(*found, targets());
                });
            }
            with->addSeparator();
            connect(with->addAction(QStringLiteral("Other Application…")), &QAction::triggered, this,
                    [this] { runCommandLine(QStringLiteral("open-with")); });
        }
        menu.addSeparator();
        add(QStringLiteral("Copy"), QStringLiteral("copy"));
        add(QStringLiteral("Cut"), QStringLiteral("cut"));
    }
    add(QStringLiteral("Paste"), QStringLiteral("paste"));
    menu.addSeparator();
    if (e) {
        add(QStringLiteral("Rename…"), QStringLiteral("rename"));
        add(QStringLiteral("Move to Trash"), QStringLiteral("trash"));
        add(QStringLiteral("Copy Path"), QStringLiteral("copy-path"));
        menu.addSeparator();
    }
    add(QStringLiteral("New Folder…"), QStringLiteral("create-folder"));
    add(QStringLiteral("New File…"), QStringLiteral("create"));
    add(QStringLiteral("Open Terminal Here"), QStringLiteral("terminal"));
    add(m_options.showHidden ? QStringLiteral("Hide Hidden Files") : QStringLiteral("Show Hidden Files"),
        QStringLiteral("toggle-hidden"));
    if (e) {
        menu.addSeparator();
        add(QStringLiteral("Properties"), QStringLiteral("properties"));
    }
    menu.exec(globalPos);
}

void MainWindow::runFromKeys(const KeyMap::Result &result) {
    m_trigger = KeyMap::display(result.keys);
    runCommandLine(result.command, result.count);
    m_trigger.clear();
}

QString MainWindow::trigger(const QString &command) const {
    return m_trigger.isEmpty() ? QStringLiteral("%1 (command)").arg(command)
                               : QStringLiteral("%1 (key %2)").arg(command, m_trigger);
}

bool MainWindow::runCommandLine(const QString &line, int count) {
    const QStringList parts = CommandRegistry::split(line);
    if (parts.isEmpty())
        return false;
    const CommandDef *def = m_commands.find(parts.first());
    if (!def) {
        error(QStringLiteral("unknown command: %1").arg(parts.first()));
        return false;
    }
    // Anything but cursor motion ends visual mode first and then acts on the marks it left.
    static const QSet<QString> keepsVisual = {
        QStringLiteral("visual"),         QStringLiteral("down"),         QStringLiteral("up"),
        QStringLiteral("top"),            QStringLiteral("bottom"),       QStringLiteral("half-page-down"),
        QStringLiteral("half-page-up"),   QStringLiteral("page-down"),    QStringLiteral("page-up"),
        QStringLiteral("search"),         QStringLiteral("search-back"),  QStringLiteral("search-next"),
        QStringLiteral("search-prev"),    QStringLiteral("preview-down"), QStringLiteral("preview-up"),
        QStringLiteral("zoom-in"),        QStringLiteral("zoom-out"),     QStringLiteral("zoom-reset"),
    };
    if (!keepsVisual.contains(def->name))
        endVisual();
    if (def->requiresArgs && parts.size() < 2) {
        promptCommand(def->name + u' ');
        return true;
    }
    def->run(CommandContext{parts.mid(1), count});
    updateStatus();
    return true;
}

void MainWindow::registerCommands() {
    auto cmd = [this](const QString &name, const QString &description, std::function<void(const CommandContext &)> fn,
                      const QString &usage = {}, bool requiresArgs = false) {
        m_commands.add(CommandDef{name, description, usage, requiresArgs, std::move(fn)});
    };

    // Movement
    cmd(QStringLiteral("down"), QStringLiteral("Move the cursor down"), [this](const CommandContext &c) { moveCursor(c.countOr(1)); });
    cmd(QStringLiteral("up"), QStringLiteral("Move the cursor up"), [this](const CommandContext &c) { moveCursor(-c.countOr(1)); });
    cmd(QStringLiteral("top"), QStringLiteral("Go to the first entry (or line N)"),
        [this](const CommandContext &c) { setCursor(c.countOr(1) - 1); });
    cmd(QStringLiteral("bottom"), QStringLiteral("Go to the last entry (or line N)"), [this](const CommandContext &c) {
        setCursor(c.count > 0 ? c.count - 1 : m_currentModel->rowCount() - 1);
    });
    cmd(QStringLiteral("half-page-down"), QStringLiteral("Move half a page down"),
        [this](const CommandContext &c) { moveCursor(c.countOr(1) * std::max(1, m_currentView->visibleRows() / 2)); });
    cmd(QStringLiteral("half-page-up"), QStringLiteral("Move half a page up"),
        [this](const CommandContext &c) { moveCursor(-c.countOr(1) * std::max(1, m_currentView->visibleRows() / 2)); });
    cmd(QStringLiteral("page-down"), QStringLiteral("Move a page down"),
        [this](const CommandContext &c) { moveCursor(c.countOr(1) * m_currentView->visibleRows()); });
    cmd(QStringLiteral("page-up"), QStringLiteral("Move a page up"),
        [this](const CommandContext &c) { moveCursor(-c.countOr(1) * m_currentView->visibleRows()); });
    cmd(QStringLiteral("enter"), QStringLiteral("Enter the folder or open the file"), [this](const CommandContext &) { enter(); });
    cmd(QStringLiteral("leave"), QStringLiteral("Go to the parent folder"), [this](const CommandContext &c) {
        for (int i = 0; i < c.countOr(1); ++i)
            leave();
    });
    cmd(QStringLiteral("open"), QStringLiteral("Open with the default application"), [this](const CommandContext &) { openTargets(); });
    cmd(QStringLiteral("open-with"), QStringLiteral("Choose an application to open with"), [this](const CommandContext &) { showOpenWith(); });
    cmd(QStringLiteral("cd"), QStringLiteral("Change directory"), [this](const CommandContext &c) {
        cd(c.args.isEmpty() ? QDir::homePath() : c.args.join(u' '));
    }, QStringLiteral("<path>"));
    cmd(QStringLiteral("back"), QStringLiteral("Go back in history"), [this](const CommandContext &) {
        if (m_back.isEmpty())
            return;
        const QString from = m_cwd;
        const QString to = m_back.takeLast();
        m_forward << from;
        cd(to, {}, false);
    });
    cmd(QStringLiteral("forward"), QStringLiteral("Go forward in history"), [this](const CommandContext &) {
        if (m_forward.isEmpty())
            return;
        m_back << m_cwd;
        cd(m_forward.takeLast(), {}, false);
    });

    // Marks
    cmd(QStringLiteral("toggle-mark"), QStringLiteral("Mark or unmark, then move down"), [this](const CommandContext &c) {
        for (int i = 0; i < c.countOr(1); ++i) {
            if (const FileEntry *e = currentEntry())
                m_marks.toggle(e->path);
            moveCursor(1);
        }
    });
    cmd(QStringLiteral("mark-all"), QStringLiteral("Mark everything in this folder"), [this](const CommandContext &) {
        QStringList paths;
        for (int r = 0; r < m_currentModel->rowCount(); ++r)
            paths << m_currentModel->entry(r)->path;
        m_marks.add(paths);
    });
    cmd(QStringLiteral("visual"), QStringLiteral("Visual mode: mark everything between here and the cursor as it moves"),
        [this](const CommandContext &) {
            if (m_visual)
                endVisual();
            else
                startVisual();
        });
    cmd(QStringLiteral("invert-marks"), QStringLiteral("Invert marks in this folder"), [this](const CommandContext &) {
        QStringList add, remove;
        for (int r = 0; r < m_currentModel->rowCount(); ++r) {
            const QString &p = m_currentModel->entry(r)->path;
            (m_marks.contains(p) ? remove : add) << p;
        }
        m_marks.remove(remove);
        m_marks.add(add);
    });
    cmd(QStringLiteral("unmark-all"), QStringLiteral("Clear all marks"), [this](const CommandContext &) { m_marks.clear(); });

    // File operations
    cmd(QStringLiteral("copy"), QStringLiteral("Copy to the clipboard"), [this](const CommandContext &) { yank(false); });
    cmd(QStringLiteral("cut"), QStringLiteral("Cut to the clipboard"), [this](const CommandContext &) { yank(true); });
    cmd(QStringLiteral("paste"), QStringLiteral("Paste here"), [this](const CommandContext &) { paste(Task::Copy); });
    cmd(QStringLiteral("paste-link"), QStringLiteral("Paste as symbolic links"), [this](const CommandContext &) { paste(Task::Link); });
    cmd(QStringLiteral("trash"), QStringLiteral("Move to the trash"), [this](const CommandContext &) {
        const QStringList paths = targets();
        m_marks.clear();
        startTask(Task::Trash, paths, {});
    });
    cmd(QStringLiteral("delete"), QStringLiteral("Delete permanently (asks first)"), [this](const CommandContext &) { confirmDelete(); });
    cmd(QStringLiteral("undo"), QStringLiteral("Undo the last trash, move, rename or copy"), [this](const CommandContext &) { undo(); });
    cmd(QStringLiteral("restore"), QStringLiteral("Restore trashed items to where they came from"),
        [this](const CommandContext &) { restoreFromTrash(); });
    cmd(QStringLiteral("empty-trash"), QStringLiteral("Permanently delete everything in the trash (asks first)"),
        [this](const CommandContext &) { confirmEmptyTrash(); });
    cmd(QStringLiteral("bulk-rename"), QStringLiteral("Rename marked items (or the whole folder) in $EDITOR"),
        [this](const CommandContext &) { bulkRename(); });
    cmd(QStringLiteral("rename"), QStringLiteral("Rename the entry under the cursor (several marked: bulk rename)"), [this](const CommandContext &c) {
        if (c.args.isEmpty()) {
            if (m_marks.size() > 1)
                bulkRename();
            else
                promptRename();
            return;
        }
        const FileEntry *e = currentEntry();
        if (!e)
            return;
        const QString from = e->path;
        const QString to = QDir::cleanPath(QDir(m_cwd).filePath(expandHome(c.args.join(u' '))));
        if (const QString problem = renamePath(from, to); !problem.isEmpty()) {
            error(problem);
            return;
        }
        pushUndo({UndoStep::Renames, QStringLiteral("rename %1").arg(baseName(from)), {{from, to}}});
        m_pendingSelect = baseName(to);
        m_currentModel->reload();
    }, QStringLiteral("[name]"));
    cmd(QStringLiteral("create"), QStringLiteral("Create a file, or a folder if it ends with /"), [this](const CommandContext &c) {
        if (c.args.isEmpty())
            promptCreate();
        else
            createPath(c.args.join(u' '));
    }, QStringLiteral("[name]"));
    cmd(QStringLiteral("create-folder"), QStringLiteral("Create a folder"), [this](const CommandContext &) { promptCreate(QStringLiteral("/")); });
    cmd(QStringLiteral("mkdir"), QStringLiteral("Create a folder"), [this](const CommandContext &c) {
        QString name = c.args.join(u' ');
        if (!name.endsWith(u'/'))
            name += u'/';
        createPath(name);
    }, QStringLiteral("<name>"), true);
    cmd(QStringLiteral("touch"), QStringLiteral("Create an empty file"),
        [this](const CommandContext &c) { createPath(c.args.join(u' ')); }, QStringLiteral("<name>"), true);
    cmd(QStringLiteral("copy-path"), QStringLiteral("Copy the full path as text"), [this](const CommandContext &) {
        const QStringList paths = targets();
        setClipboardText(paths.join(u'\n'));
        message(QStringLiteral("copied %1").arg(paths.size() == 1 ? tilde(paths.first()) : plural(int(paths.size()), "path")));
    });
    cmd(QStringLiteral("copy-name"), QStringLiteral("Copy the file name as text"), [this](const CommandContext &) {
        QStringList names;
        for (const QString &p : targets())
            names << baseName(p);
        setClipboardText(names.join(u'\n'));
        message(QStringLiteral("copied %1").arg(names.join(QStringLiteral(", "))));
    });
    cmd(QStringLiteral("copy-dir"), QStringLiteral("Copy this folder's path as text"), [this](const CommandContext &) {
        setClipboardText(m_cwd);
        message(QStringLiteral("copied %1").arg(tilde(m_cwd)));
    });

    // Finding things
    cmd(QStringLiteral("find"), QStringLiteral("Fuzzy find files below this folder"), [this](const CommandContext &) { promptFind(); });
    cmd(QStringLiteral("filter"), QStringLiteral("Filter this folder as you type"), [this](const CommandContext &c) {
        if (c.args.isEmpty())
            promptFilter();
        else
            m_currentModel->setFilter(c.args.join(u' '));
    }, QStringLiteral("[text]"));
    cmd(QStringLiteral("search"), QStringLiteral("Jump to a matching name"), [this](const CommandContext &) { promptSearch(false); });
    cmd(QStringLiteral("search-back"), QStringLiteral("Jump to a matching name, backwards"), [this](const CommandContext &) { promptSearch(true); });
    cmd(QStringLiteral("search-next"), QStringLiteral("Next search match"), [this](const CommandContext &) { searchNext(1); });
    cmd(QStringLiteral("search-prev"), QStringLiteral("Previous search match"), [this](const CommandContext &) { searchNext(-1); });

    // View
    cmd(QStringLiteral("toggle-hidden"), QStringLiteral("Show or hide dotfiles"), [this](const CommandContext &) {
        runCommandLine(QStringLiteral("set hidden toggle"));
        message(m_options.showHidden ? QStringLiteral("showing hidden files") : QStringLiteral("hiding hidden files"));
    });
    cmd(QStringLiteral("toggle-preview"), QStringLiteral("Show or hide the preview column"), [this](const CommandContext &) {
        runCommandLine(QStringLiteral("set preview toggle"));
    });
    cmd(QStringLiteral("sort"), QStringLiteral("Sort by name, size, modified or extension"), [this](const CommandContext &c) {
        const QString key = c.args.value(0).toLower();
        if (key == QLatin1String("reverse")) {
            m_options.reverse = !m_options.reverse;
        } else {
            static const QHash<QString, ViewOptions::SortKey> keys = {
                {QStringLiteral("name"), ViewOptions::Name},         {QStringLiteral("size"), ViewOptions::Size},
                {QStringLiteral("modified"), ViewOptions::Modified}, {QStringLiteral("mtime"), ViewOptions::Modified},
                {QStringLiteral("extension"), ViewOptions::Extension}, {QStringLiteral("ext"), ViewOptions::Extension},
            };
            if (!keys.contains(key)) {
                error(QStringLiteral("sort by name, size, modified, extension or reverse"));
                return;
            }
            m_options.sort = keys.value(key);
            if (c.args.value(1) == QLatin1String("reverse"))
                m_options.reverse = true;
        }
        applyOptions();
    }, QStringLiteral("<key>"), true);
    cmd(QStringLiteral("preview-down"), QStringLiteral("Scroll the preview down"), [this](const CommandContext &c) {
        if (auto *text = qobject_cast<QPlainTextEdit *>(m_preview->currentWidget()))
            text->verticalScrollBar()->setValue(text->verticalScrollBar()->value() + c.countOr(1) * 10);
        else if (m_preview->currentWidget() == m_preview->dirView())
            m_preview->dirView()->verticalScrollBar()->setValue(m_preview->dirView()->verticalScrollBar()->value() + c.countOr(1) * 10 * Theme::instance().rowHeight());
    });
    cmd(QStringLiteral("preview-up"), QStringLiteral("Scroll the preview up"), [this](const CommandContext &c) {
        if (auto *text = qobject_cast<QPlainTextEdit *>(m_preview->currentWidget()))
            text->verticalScrollBar()->setValue(text->verticalScrollBar()->value() - c.countOr(1) * 10);
        else if (m_preview->currentWidget() == m_preview->dirView())
            m_preview->dirView()->verticalScrollBar()->setValue(m_preview->dirView()->verticalScrollBar()->value() - c.countOr(1) * 10 * Theme::instance().rowHeight());
    });
    cmd(QStringLiteral("zoom-in"), QStringLiteral("Bigger text"), [](const CommandContext &) { Theme::instance().zoom(1); });
    cmd(QStringLiteral("zoom-out"), QStringLiteral("Smaller text"), [](const CommandContext &) { Theme::instance().zoom(-1); });
    cmd(QStringLiteral("zoom-reset"), QStringLiteral("Text back to the configured size"), [](const CommandContext &) { Theme::instance().resetZoom(); });
    cmd(QStringLiteral("reload"), QStringLiteral("Reload the listing"), [this](const CommandContext &) {
        m_currentModel->reload();
        m_parentModel->reload();
        DesktopApps::instance().reload();
    });

    // Prompts and panels
    cmd(QStringLiteral("command"), QStringLiteral("Open the command line"), [this](const CommandContext &) { promptCommand(); });
    cmd(QStringLiteral("terminal"), QStringLiteral("Open a terminal in this folder"), [this](const CommandContext &) {
        const QString term = terminalProgram();
        if (term.isEmpty()) {
            error(QStringLiteral("no terminal found; set $TERMINAL"));
            return;
        }
        launcher::startDetached(term, terminalArgsForDirectory(term, m_cwd), m_cwd);
    });
    cmd(QStringLiteral("tasks"), QStringLiteral("Show copy, move and delete progress"), [this](const CommandContext &) { showTasks(); });
    cmd(QStringLiteral("properties"), QStringLiteral("Show details and total size"), [this](const CommandContext &) { showProperties(); });
    cmd(QStringLiteral("help"), QStringLiteral("List key bindings"), [this](const CommandContext &) { showHelp(); });
    cmd(QStringLiteral("new-window"), QStringLiteral("Open another window here"), [this](const CommandContext &) {
        const QStringList args = QCoreApplication::arguments();
        launcher::startDetached(args.isEmpty() ? QCoreApplication::applicationFilePath() : args.first(), {m_cwd}, {});
    });
    cmd(QStringLiteral("new-tab"), QStringLiteral("Open a tab here (or at a path)"), [this](const CommandContext &c) {
        newTab(c.args.isEmpty() ? m_cwd : c.args.join(u' '));
    }, QStringLiteral("[path]"));
    cmd(QStringLiteral("close-tab"), QStringLiteral("Close this tab (the window, if it is the last)"), [this](const CommandContext &) {
        if (m_tabs.size() > 1) {
            logging::info(QStringLiteral("tab %1 of %2 closed by %3: %4").arg(m_tab + 1).arg(m_tabs.size()).arg(trigger(QStringLiteral("close-tab")), m_cwd));
            closeTab();
        } else {
            m_closeReason = trigger(QStringLiteral("close-tab on the last tab"));
            runCommandLine(QStringLiteral("quit"));
        }
    });
    cmd(QStringLiteral("next-tab"), QStringLiteral("Go to the next tab"), [this](const CommandContext &c) {
        if (m_tabs.size() < 2)
            return;
        saveTab();
        loadTab(c.count > 0 ? c.count - 1 : (m_tab + 1) % int(m_tabs.size()));
    });
    cmd(QStringLiteral("prev-tab"), QStringLiteral("Go to the previous tab"), [this](const CommandContext &c) {
        if (m_tabs.size() < 2)
            return;
        saveTab();
        const int n = int(m_tabs.size());
        loadTab(((m_tab - c.countOr(1)) % n + n) % n);
    });
    cmd(QStringLiteral("tab"), QStringLiteral("Go to tab N"), [this](const CommandContext &c) {
        const int n = c.args.value(0).toInt();
        if (n < 1 || n > m_tabs.size()) {
            error(QStringLiteral("no tab %1").arg(c.args.value(0)));
            return;
        }
        saveTab();
        loadTab(n - 1);
    }, QStringLiteral("<n>"), true);
    cmd(QStringLiteral("reopen-tab"), QStringLiteral("Reopen the last closed tab"), [this](const CommandContext &) {
        if (m_closedTabs.isEmpty()) {
            message(QStringLiteral("no closed tabs"));
            return;
        }
        saveTab();
        m_tabs.insert(m_tab + 1, m_closedTabs.takeLast());
        loadTab(m_tab + 1);
    });
    cmd(QStringLiteral("quit"), QStringLiteral("Close the window"), [this](const CommandContext &) {
        const int active = m_tasks.activeCount();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (active > 0 && now - m_quitArmedAt > 3000) {
            m_quitArmedAt = now;
            m_closeReason.clear();
            message(QStringLiteral("%1 still running; quit again to cancel them").arg(plural(active, "task")),
                    StatusBar::Level::Warning);
            return;
        }
        if (m_closeReason.isEmpty())
            m_closeReason = trigger(QStringLiteral("quit"));
        close();
    });

    cmd(QStringLiteral("choose"), QStringLiteral("File chooser: accept the marked items, this folder, or the file name"),
        [this](const CommandContext &) {
            if (isChooser())
                chooserAccept(false);
        });
    cmd(QStringLiteral("choose-filter"), QStringLiteral("File chooser: next file type filter"), [this](const CommandContext &) {
        if (m_chooserBar)
            m_chooserBar->cycleFilter();
    });

    // Configuration
    cmd(QStringLiteral("set"), QStringLiteral("Change an option"), [this](const CommandContext &c) {
        const QString key = c.args.value(0);
        const QString value = c.args.mid(1).join(u' ');
        if (key == QLatin1String("hidden")) {
            m_options.showHidden = parseBool(value, m_options.showHidden);
            applyOptions();
        } else if (key == QLatin1String("dirs-first")) {
            m_options.dirsFirst = parseBool(value, m_options.dirsFirst);
            applyOptions();
        } else if (key == QLatin1String("reverse")) {
            m_options.reverse = parseBool(value, m_options.reverse);
            applyOptions();
        } else if (key == QLatin1String("sort")) {
            runCommandLine(QStringLiteral("sort ") + value);
        } else if (key == QLatin1String("preview")) {
            m_showPreview = parseBool(value, m_showPreview);
            updatePreview();
        } else if (key == QLatin1String("theme")) {
            const QString v = value.toLower();
            Theme::instance().setMode(v == QLatin1String("dark")    ? ThemeMode::Dark
                                      : v == QLatin1String("light") ? ThemeMode::Light
                                                                    : ThemeMode::Auto);
        } else if (key == QLatin1String("editor")) {
            m_editor = value;
        } else if (key == QLatin1String("font")) {
            Theme::instance().setFontFamily(value);
        } else if (key == QLatin1String("font-size")) {
            bool ok = false;
            const double size = value.toDouble(&ok);
            // From scout.conf it is only the default; typed, it replaces any remembered zoom.
            if (ok)
                Theme::instance().setFontSize(size, !m_loadingConfig);
            else
                error(QStringLiteral("font-size needs a number"));
        } else {
            error(QStringLiteral("unknown option: %1").arg(key));
        }
    }, QStringLiteral("<option> <value>"), true);
    cmd(QStringLiteral("map"), QStringLiteral("Bind keys to a command line"), [this](const CommandContext &c) {
        if (c.args.size() < 2) {
            error(QStringLiteral("usage: map <keys> <command>"));
            return;
        }
        // `map gw 'cd ~/work'` and `map gw cd ~/work` mean the same thing.
        QString command;
        if (c.args.size() == 2) {
            command = c.args[1];
        } else {
            QStringList rest;
            for (const QString &a : c.args.mid(1))
                rest << CommandRegistry::quote(a);
            command = rest.join(u' ');
        }
        m_keys.bind(c.args.first(), command, command);
    }, QStringLiteral("<keys> <command>"), true);
    cmd(QStringLiteral("unmap"), QStringLiteral("Remove a key binding"),
        [this](const CommandContext &c) { m_keys.unbind(c.args.value(0)); }, QStringLiteral("<keys>"), true);
}

void MainWindow::bindDefaults() {
    auto bind = [this](const QString &keys, const QString &command, const QString &description = {}) {
        QString desc = description;
        if (desc.isEmpty())
            if (const CommandDef *def = m_commands.find(CommandRegistry::split(command).value(0)))
                desc = def->description;
        m_keys.bind(keys, command, desc);
    };
    bind(QStringLiteral("j"), QStringLiteral("down"));
    bind(QStringLiteral("<Down>"), QStringLiteral("down"));
    bind(QStringLiteral("k"), QStringLiteral("up"));
    bind(QStringLiteral("<Up>"), QStringLiteral("up"));
    bind(QStringLiteral("h"), QStringLiteral("leave"));
    bind(QStringLiteral("<Left>"), QStringLiteral("leave"));
    bind(QStringLiteral("<BS>"), QStringLiteral("leave"));
    bind(QStringLiteral("-"), QStringLiteral("leave"));
    bind(QStringLiteral("l"), QStringLiteral("enter"));
    bind(QStringLiteral("<Right>"), QStringLiteral("enter"));
    bind(QStringLiteral("<CR>"), QStringLiteral("open"));
    bind(QStringLiteral("gg"), QStringLiteral("top"));
    bind(QStringLiteral("<Home>"), QStringLiteral("top"));
    bind(QStringLiteral("G"), QStringLiteral("bottom"));
    bind(QStringLiteral("<End>"), QStringLiteral("bottom"));
    bind(QStringLiteral("<C-d>"), QStringLiteral("half-page-down"));
    bind(QStringLiteral("<C-u>"), QStringLiteral("half-page-up"));
    bind(QStringLiteral("<C-f>"), QStringLiteral("page-down"));
    bind(QStringLiteral("<C-b>"), QStringLiteral("page-up"));
    bind(QStringLiteral("<PageDown>"), QStringLiteral("page-down"));
    bind(QStringLiteral("<PageUp>"), QStringLiteral("page-up"));
    bind(QStringLiteral("H"), QStringLiteral("back"));
    bind(QStringLiteral("<A-Left>"), QStringLiteral("back"));
    bind(QStringLiteral("L"), QStringLiteral("forward"));
    bind(QStringLiteral("<A-Right>"), QStringLiteral("forward"));
    bind(QStringLiteral("J"), QStringLiteral("preview-down"));
    bind(QStringLiteral("K"), QStringLiteral("preview-up"));
    bind(QStringLiteral("~"), QStringLiteral("cd ~"), QStringLiteral("Home"));

    bind(QStringLiteral("gh"), QStringLiteral("cd ~"), QStringLiteral("Home"));
    // Follows ~/.config/user-dirs.dirs.
    bind(QStringLiteral("gd"),
         QStringLiteral("cd ") + CommandRegistry::quote(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)),
         QStringLiteral("Downloads"));
    bind(QStringLiteral("gc"), QStringLiteral("cd ~/.config"), QStringLiteral("~/.config"));
    bind(QStringLiteral("gr"), QStringLiteral("cd ~/repos"), QStringLiteral("~/repos"));
    bind(QStringLiteral("g/"), QStringLiteral("cd /"), QStringLiteral("Root /"));
    bind(QStringLiteral("gb"), QStringLiteral("cd ~/.local/share/Trash/files"), QStringLiteral("Trash (bin)"));
    bind(QStringLiteral("gt"), QStringLiteral("next-tab"));
    bind(QStringLiteral("gp"), QStringLiteral("prev-tab"));
    bind(QStringLiteral("gn"), QStringLiteral("new-tab"));
    bind(QStringLiteral("gf"), QStringLiteral("find"));
    bind(QStringLiteral("gx"), QStringLiteral("open"));
    bind(QStringLiteral("<C-p>"), QStringLiteral("find"));

    bind(QStringLiteral("<Space>"), QStringLiteral("toggle-mark"));
    bind(QStringLiteral("v"), QStringLiteral("visual"));
    bind(QStringLiteral("V"), QStringLiteral("visual"));
    bind(QStringLiteral("*"), QStringLiteral("invert-marks"));
    bind(QStringLiteral("<C-a>"), QStringLiteral("mark-all"));
    bind(QStringLiteral("uv"), QStringLiteral("unmark-all"));
    bind(QStringLiteral("uu"), QStringLiteral("undo"));
    bind(QStringLiteral("<C-z>"), QStringLiteral("undo"));
    bind(QStringLiteral("ur"), QStringLiteral("restore"), QStringLiteral("Restore from trash"));
    bind(QStringLiteral("uq"), QStringLiteral("reopen-tab"));

    bind(QStringLiteral("yy"), QStringLiteral("copy"));
    bind(QStringLiteral("yp"), QStringLiteral("copy-path"));
    bind(QStringLiteral("yn"), QStringLiteral("copy-name"));
    bind(QStringLiteral("yd"), QStringLiteral("copy-dir"));
    bind(QStringLiteral("dd"), QStringLiteral("cut"));
    bind(QStringLiteral("dD"), QStringLiteral("trash"));
    bind(QStringLiteral("dX"), QStringLiteral("delete"));
    bind(QStringLiteral("<Del>"), QStringLiteral("trash"));
    bind(QStringLiteral("<S-Del>"), QStringLiteral("delete"));
    bind(QStringLiteral("pp"), QStringLiteral("paste"));
    bind(QStringLiteral("pl"), QStringLiteral("paste-link"));
    bind(QStringLiteral("<C-c>"), QStringLiteral("copy"));
    bind(QStringLiteral("<C-x>"), QStringLiteral("cut"));
    bind(QStringLiteral("<C-v>"), QStringLiteral("paste"));
    bind(QStringLiteral("r"), QStringLiteral("rename"));
    bind(QStringLiteral("cw"), QStringLiteral("rename"));
    bind(QStringLiteral("<F2>"), QStringLiteral("rename"));
    bind(QStringLiteral("R"), QStringLiteral("bulk-rename"));
    bind(QStringLiteral("a"), QStringLiteral("create"));
    bind(QStringLiteral("A"), QStringLiteral("create-folder"));
    bind(QStringLiteral("o"), QStringLiteral("open-with"));
    bind(QStringLiteral("O"), QStringLiteral("open-with"));

    bind(QStringLiteral("/"), QStringLiteral("search"));
    bind(QStringLiteral("?"), QStringLiteral("help"));
    bind(QStringLiteral("n"), QStringLiteral("search-next"));
    bind(QStringLiteral("N"), QStringLiteral("search-prev"));
    bind(QStringLiteral("zf"), QStringLiteral("filter"));
    bind(QStringLiteral("f"), QStringLiteral("filter"));
    bind(QStringLiteral("zh"), QStringLiteral("toggle-hidden"));
    bind(QStringLiteral("<C-h>"), QStringLiteral("toggle-hidden"));
    bind(QStringLiteral("zp"), QStringLiteral("toggle-preview"));
    bind(QStringLiteral("sn"), QStringLiteral("sort name"), QStringLiteral("Sort by name"));
    bind(QStringLiteral("ss"), QStringLiteral("sort size"), QStringLiteral("Sort by size"));
    bind(QStringLiteral("sm"), QStringLiteral("sort modified"), QStringLiteral("Sort by modified time"));
    bind(QStringLiteral("se"), QStringLiteral("sort extension"), QStringLiteral("Sort by extension"));
    bind(QStringLiteral("sr"), QStringLiteral("sort reverse"), QStringLiteral("Reverse the sort"));

    bind(QStringLiteral(":"), QStringLiteral("command"));
    bind(QStringLiteral("T"), QStringLiteral("terminal"));
    bind(QStringLiteral("w"), QStringLiteral("tasks"));
    bind(QStringLiteral("<A-CR>"), QStringLiteral("properties"));
    bind(QStringLiteral("<C-r>"), QStringLiteral("reload"));
    bind(QStringLiteral("<C-n>"), QStringLiteral("new-window"));
    bind(QStringLiteral("<C-=>"), QStringLiteral("zoom-in"));
    bind(QStringLiteral("<C-+>"), QStringLiteral("zoom-in"));
    bind(QStringLiteral("<C-->"), QStringLiteral("zoom-out"));
    bind(QStringLiteral("<C-0>"), QStringLiteral("zoom-reset"));
    bind(QStringLiteral("q"), QStringLiteral("close-tab"));
    bind(QStringLiteral("<C-q>"), QStringLiteral("quit"));
    bind(QStringLiteral("<C-w>"), QStringLiteral("close-tab"));
    bind(QStringLiteral("<C-t>"), QStringLiteral("new-tab"));
    bind(QStringLiteral("<C-Tab>"), QStringLiteral("next-tab"));
    bind(QStringLiteral("<C-S-Tab>"), QStringLiteral("prev-tab"));
    for (int i = 1; i <= 9; ++i)
        bind(QStringLiteral("<A-%1>").arg(i), QStringLiteral("tab %1").arg(i), QStringLiteral("Go to tab %1").arg(i));
}

void MainWindow::loadConfig() {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                         QStringLiteral("/scout/scout.conf");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&f);
    int lineNo = 0;
    QStringList problems;
    while (!in.atEnd()) {
        ++lineNo;
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        const QString name = CommandRegistry::split(line).value(0);
        if (!m_commands.find(name)) {
            problems << QStringLiteral("line %1: unknown command %2").arg(lineNo).arg(name);
            continue;
        }
        m_loadingConfig = true;
        runCommandLine(line);
        m_loadingConfig = false;
    }
    if (!problems.isEmpty())
        error(QStringLiteral("scout.conf: ") + problems.join(QStringLiteral("; ")));
}

// Tabs

bool MainWindow::restoreSession() {
    const std::optional<state::Session> saved = state::session();
    if (!saved || saved->tabs.isEmpty())
        return false;
    QList<Tab> tabs;
    int active = 0;
    for (int i = 0; i < saved->tabs.size(); ++i) {
        if (!QFileInfo(saved->tabs[i]).isDir())
            continue;
        // A vanished active tab hands over to the one before it.
        if (i <= saved->active)
            active = int(tabs.size());
        Tab t;
        t.cwd = saved->tabs[i];
        tabs << t;
    }
    if (tabs.isEmpty()) {
        Tab home;
        home.cwd = QDir::homePath();
        tabs << home;
    }
    logging::info(QStringLiteral("restored %1 of %2 saved tabs").arg(tabs.size()).arg(saved->tabs.size()));
    m_tabs = tabs;
    m_back.clear();
    m_forward.clear();
    loadTab(active);
    return true;
}

void MainWindow::saveTab() {
    Tab &t = m_tabs[m_tab];
    t.cwd = m_cwd;
    t.back = m_back;
    t.forward = m_forward;
    t.lastSelected = m_lastSelected;
    t.lastRow = m_lastRow;
}

void MainWindow::loadTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    // Before the filter is cleared, which would otherwise stretch the range over the rows it hid.
    endVisual();
    m_tab = index;
    const Tab t = m_tabs[index];
    m_back = t.back;
    m_forward = t.forward;
    m_lastSelected = t.lastSelected;
    const QString path = t.cwd.isEmpty() ? QDir::homePath() : t.cwd;
    m_currentModel->setFilter({});
    m_cwd.clear();
    if (!cd(path, m_lastSelected.value(path), false))
        cd(QDir::homePath(), {}, false);
    m_lastRow = t.lastRow;
    updateTabs();
}

void MainWindow::newTab(const QString &path) {
    saveTab();
    Tab t;
    t.lastSelected = m_lastSelected;
    m_tabs.insert(m_tab + 1, t);
    m_tab += 1;
    m_back.clear();
    m_forward.clear();
    const QString from = m_cwd;
    m_cwd.clear();
    if (!cd(path, {}, false))
        cd(from.isEmpty() ? QDir::homePath() : from, {}, false);
    updateTabs();
}

void MainWindow::closeTab() {
    saveTab();
    m_closedTabs.append(m_tabs.takeAt(m_tab));
    loadTab(std::min(m_tab, int(m_tabs.size()) - 1));
}

void MainWindow::updateTabs() {
    QStringList labels;
    const QString home = QDir::homePath();
    for (int i = 0; i < m_tabs.size(); ++i) {
        const QString dir = i == m_tab ? m_cwd : m_tabs[i].cwd;
        labels << (dir == home ? QStringLiteral("~") : dir == QLatin1String("/") ? dir : baseName(dir));
    }
    m_header->setTabs(labels, m_tab);
}

// Undo

void MainWindow::pushUndo(UndoStep step) {
    step.ids.clear();
    for (const auto &[original, now] : step.pairs)
        step.ids << fileStamp(now);
    m_undo.append(std::move(step));
    while (m_undo.size() > 100)
        m_undo.removeFirst();
}

void MainWindow::undo() {
    if (m_undo.isEmpty()) {
        message(QStringLiteral("nothing to undo"));
        return;
    }
    const UndoStep step = m_undo.takeLast();
    QList<QPair<QString, QString>> back;
    QStringList current, replaced;
    for (int i = 0; i < step.pairs.size(); ++i) {
        const auto &[original, now] = step.pairs[i];
        const FileStamp stamp = fileStamp(now);
        if (!stamp.exists())
            replaced << QStringLiteral("%1 (gone)").arg(baseName(now));
        else if (stamp != step.ids.value(i))
            replaced << QStringLiteral("%1 (replaced)").arg(baseName(now));
        else {
            back.append({now, original});
            current << now;
        }
    }
    if (!replaced.isEmpty()) {
        error(QStringLiteral("can't undo %1: %2 changed since").arg(step.description, replaced.join(QStringLiteral(", "))));
        if (current.isEmpty())
            return;
    }
    switch (step.kind) {
    case UndoStep::Moves:
        m_undoTasks.insert(m_tasks.startRestore(back));
        message(QStringLiteral("undoing %1…").arg(step.description));
        break;
    case UndoStep::Renames:
        if (applyRenames(back, false))
            message(QStringLiteral("undid %1").arg(step.description), StatusBar::Level::Success);
        break;
    case UndoStep::Restored:
        m_undoTasks.insert(m_tasks.start(Task::Trash, current));
        message(QStringLiteral("undoing %1 (moving to the trash)…").arg(step.description));
        break;
    }
}

void MainWindow::restoreFromTrash() {
    QList<QPair<QString, QString>> moves;
    QStringList notTrashed;
    for (const QString &p : targets()) {
        const QString original = trashOriginalPath(p);
        if (original.isEmpty())
            notTrashed << baseName(p);
        else
            moves.append({p, original});
    }
    if (moves.isEmpty()) {
        error(notTrashed.isEmpty() ? QStringLiteral("nothing to restore")
                                   : QStringLiteral("not in the trash: %1").arg(notTrashed.join(QStringLiteral(", "))));
        return;
    }
    m_marks.clear();
    Task *task = m_tasks.startRestore(moves);
    message(task->title() + QStringLiteral("…"));
}

void MainWindow::confirmEmptyTrash() {
    const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash");
    QStringList paths;
    QString err;
    for (const char *sub : {"files", "info"})
        for (const FileEntry &e : listDirectory(trash + u'/' + QLatin1String(sub), &err))
            paths << e.path;
    if (paths.isEmpty()) {
        message(QStringLiteral("the trash is empty"));
        return;
    }
    PromptSpec spec;
    spec.label = QStringLiteral("permanently delete everything in the trash? [y/N]");
    spec.onChange = [this, paths](const QString &text) {
        if (text.isEmpty())
            return;
        m_prompt->close(true);
        if (text == QLatin1String("y") || text == QLatin1String("Y"))
            startTask(Task::Delete, paths, {});
        else
            message(QStringLiteral("trash left alone"));
    };
    spec.onAccept = [this](const QString &, const PickerItem *) { message(QStringLiteral("trash left alone")); };
    openPrompt(spec);
}

// Bulk rename

bool MainWindow::applyRenames(const QList<QPair<QString, QString>> &renames, bool recordUndo) {
    QStringList errors;
    const QList<QPair<QString, QString>> done = renameAll(renames, &errors);
    QStringList unmark, mark;
    for (const auto &[from, to] : done) {
        if (m_marks.contains(from)) {
            unmark << from;
            mark << to;
        }
        if (parentDir(from) == m_cwd && m_lastSelected.value(m_cwd) == baseName(from) && parentDir(to) == m_cwd)
            m_pendingSelect = baseName(to);
    }
    m_marks.remove(unmark);
    m_marks.add(mark);
    if (recordUndo && !done.isEmpty())
        pushUndo({UndoStep::Renames, done.size() == 1 ? QStringLiteral("rename %1").arg(baseName(done.first().first))
                                                      : plural(int(done.size()), "rename"),
                  done});
    m_currentModel->reload();
    if (!errors.isEmpty()) {
        error(errors.first() + (errors.size() > 1 ? QStringLiteral(" (+%1 more)").arg(errors.size() - 1) : QString()));
        return false;
    }
    if (recordUndo)
        message(QStringLiteral("renamed %1").arg(plural(int(done.size()), "item")), StatusBar::Level::Success);
    return true;
}

namespace {
QStringList bulkRenameLines(const QStringList &paths, const QString &cwd) {
    QStringList lines;
    for (const QString &p : paths)
        lines << (parentDir(p) == cwd ? baseName(p) : p);
    return lines;
}
} // namespace

void MainWindow::bulkRename() {
    QStringList paths;
    if (m_marks.isEmpty()) {
        for (int r = 0; r < m_currentModel->rowCount(); ++r)
            paths << m_currentModel->entry(r)->path;
    } else {
        // Marks in this folder keep their on-screen order; the rest follow.
        QStringList rest = m_marks.paths();
        for (int r = 0; r < m_currentModel->rowCount(); ++r) {
            const QString &p = m_currentModel->entry(r)->path;
            if (rest.removeOne(p))
                paths << p;
        }
        std::sort(rest.begin(), rest.end(), [](const QString &a, const QString &b) { return naturalCompare(a, b) < 0; });
        paths << rest;
    }
    if (paths.isEmpty())
        return;
    startBulkRename(paths);
}

QString MainWindow::editorCommand() const {
    for (const QString &candidate : {m_editor, qEnvironmentVariable("VISUAL"), qEnvironmentVariable("EDITOR")})
        if (!candidate.trimmed().isEmpty())
            return candidate.trimmed();
    for (const char *fallback : {"nvim", "vim", "vi", "nano"})
        if (!QStandardPaths::findExecutable(QLatin1String(fallback)).isEmpty())
            return QLatin1String(fallback);
    return QStringLiteral("vi");
}

void MainWindow::startBulkRename(const QStringList &paths) {
    for (const QString &p : paths) {
        if (p.contains(u'\n')) {
            error(QStringLiteral("can't bulk rename names with line breaks: %1").arg(baseName(p)));
            return;
        }
    }
    m_renamePoll.stop();
    m_renamePoll.disconnect();
    m_renameDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/scout-rename-XXXXXX"));
    if (!m_renameDir->isValid()) {
        error(QStringLiteral("could not create a temporary folder"));
        m_renameDir.reset();
        return;
    }
    const QString file = m_renameDir->filePath(QStringLiteral("names.txt"));
    const QString done = m_renameDir->filePath(QStringLiteral("done"));
    {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            error(QStringLiteral("could not write %1").arg(file));
            return;
        }
        f.write(bulkRenameLines(paths, m_cwd).join(u'\n').toUtf8() + '\n');
    }
    m_renamePaths = paths;

    // The editor runs inside sh so `set editor code --wait` style commands work,
    // and it reports through a file because terminals may hand the command to
    // an already running instance and exit right away.
    const QString editor = editorCommand();
    const QString script = editor + QStringLiteral(" \"$1\"; printf '%s' \"$?\" > \"$2\"");
    static const QStringList guiEditors = {
        QStringLiteral("code"),  QStringLiteral("codium"), QStringLiteral("cursor"), QStringLiteral("subl"),
        QStringLiteral("zed"),   QStringLiteral("zeditor"), QStringLiteral("gedit"), QStringLiteral("gnome-text-editor"),
        QStringLiteral("kate"),  QStringLiteral("kwrite"), QStringLiteral("mousepad"), QStringLiteral("gvim"),
        QStringLiteral("neovide")};
    QStringList argv{QStringLiteral("sh"), QStringLiteral("-c"), script, QStringLiteral("sh"), file, done};
    if (!guiEditors.contains(QFileInfo(CommandRegistry::split(editor).value(0)).fileName())) {
        const QString term = terminalProgram();
        if (term.isEmpty()) {
            error(QStringLiteral("no terminal found for %1; set $TERMINAL").arg(editor));
            return;
        }
        argv = QStringList{term} + terminalExecArgs(term) + argv;
    }
    if (!launcher::startDetached(argv.first(), argv.mid(1), m_cwd)) {
        error(QStringLiteral("could not start %1").arg(argv.first()));
        return;
    }
    const QString baseDir = m_cwd;
    connect(&m_renamePoll, &QTimer::timeout, this, [this, file, done, baseDir] {
        if (!QFile::exists(done))
            return;
        m_renamePoll.stop();
        QFile status(done);
        const QByteArray code = status.open(QIODevice::ReadOnly) ? status.readAll().trimmed() : QByteArray("?");
        QStringList lines;
        QFile names(file);
        if (names.open(QIODevice::ReadOnly | QIODevice::Text))
            lines = QString::fromUtf8(names.readAll()).split(u'\n');
        const QStringList paths = m_renamePaths;
        m_renameDir.reset();
        m_renamePaths.clear();
        if (code != "0") {
            error(QStringLiteral("the editor exited with status %1; nothing renamed").arg(QString::fromLatin1(code)));
            return;
        }
        finishBulkRename(paths, lines, baseDir);
    });
    m_renamePoll.start();
    message(QStringLiteral("editing %1 in %2; save and quit to rename").arg(plural(int(paths.size()), "name"), editor));
}

void MainWindow::bulkRenameWith(const std::function<QStringList(const QStringList &)> &edit) {
    QStringList paths;
    for (int r = 0; r < m_currentModel->rowCount(); ++r)
        paths << m_currentModel->entry(r)->path;
    if (!m_marks.isEmpty())
        paths = m_marks.paths();
    finishBulkRename(paths, edit(bulkRenameLines(paths, m_cwd)), m_cwd);
}

void MainWindow::finishBulkRename(const QStringList &paths, const QStringList &rawLines, const QString &baseDir) {
    const QStringList originals = bulkRenameLines(paths, baseDir);
    QStringList lines = rawLines;
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    // An editor set to CRLF adds a \r to every line, but on Linux a \r can
    // also end a real name.
    const bool crlf = !lines.isEmpty() && std::all_of(lines.begin(), lines.end(), [](const QString &l) { return l.endsWith(u'\r'); });
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i] == originals.value(i))
            continue;
        if (lines[i] == originals.value(i) + u'\r')
            lines[i] = originals.value(i);
        else if (crlf)
            lines[i].chop(1);
    }
    if (lines.size() != paths.size()) {
        error(QStringLiteral("expected %1 lines but got %2; nothing renamed").arg(paths.size()).arg(lines.size()));
        return;
    }
    QList<QPair<QString, QString>> renames;
    for (int i = 0; i < paths.size(); ++i) {
        const QString &from = paths[i];
        const QString &line = lines[i];
        if (line.isEmpty() || line == QLatin1String(".") || line == QLatin1String("..")) {
            error(QStringLiteral("line %1 is not a usable name; nothing renamed").arg(i + 1));
            return;
        }
        QString to;
        if (parentDir(from) == baseDir) {
            if (line.contains(u'/')) {
                error(QStringLiteral("line %1: names can't contain /; nothing renamed").arg(i + 1));
                return;
            }
            to = QDir(baseDir).filePath(line);
        } else {
            to = QDir::cleanPath(line);
            if (parentDir(to) != parentDir(from)) {
                error(QStringLiteral("line %1: bulk rename can't move items between folders").arg(i + 1));
                return;
            }
        }
        if (to != from)
            renames.append({from, to});
    }
    if (renames.isEmpty()) {
        message(QStringLiteral("no names changed"));
        return;
    }
    QList<PickerItem> items;
    for (const auto &[from, to] : renames) {
        PickerItem item;
        item.text = QStringLiteral("%1  →  %2").arg(baseName(from), baseName(to));
        if (parentDir(from) != m_cwd)
            item.description = tilde(parentDir(from));
        items << item;
    }
    PromptSpec spec;
    spec.label = QStringLiteral("rename %1? [y/N]").arg(plural(int(renames.size()), "item"));
    spec.showList = true;
    spec.onChange = [this, renames, items](const QString &text) {
        if (text.isEmpty()) {
            m_picker->setItems(items);
            m_picker->setHeader(QStringLiteral("bulk rename"));
            m_picker->setFooter(QStringLiteral("y rename · anything else cancels"));
            positionOverlays();
            return;
        }
        m_prompt->close(true);
        if (text == QLatin1String("y") || text == QLatin1String("Y"))
            applyRenames(renames);
        else
            message(QStringLiteral("bulk rename cancelled"));
    };
    spec.onAccept = [this](const QString &, const PickerItem *) { message(QStringLiteral("bulk rename cancelled")); };
    openPrompt(spec);
}

// File chooser

void MainWindow::setChooser(const ChooserRequest &request, std::function<void(const ChooserResult &)> done) {
    m_chooser = request;
    m_chooserDone = std::move(done);
    m_chooserBar = new ChooserBar(request, this);
    static_cast<QVBoxLayout *>(layout())->insertWidget(2, m_chooserBar);
    connect(m_chooserBar, &ChooserBar::acceptClicked, this, [this] { chooserAccept(false); });
    connect(m_chooserBar, &ChooserBar::cancelClicked, this, [this] { finishChooser(false); });
    auto applyFilter = [this](int index) {
        if (m_chooser.directory) {
            m_currentModel->setFileFilter([](const FileEntry &) { return false; });
        } else if (index >= 0 && index < m_chooser.filters.size()) {
            const FileFilter filter = m_chooser.filters[index];
            m_currentModel->setFileFilter([filter](const FileEntry &e) { return filter.accepts(e); });
        }
    };
    connect(m_chooserBar, &ChooserBar::filterChanged, this, applyFilter);
    applyFilter(m_chooserBar->filterIndex());

    QString title = request.title;
    if (title.isEmpty())
        title = request.mode == ChooserRequest::Mode::Open
                    ? (request.directory ? QStringLiteral("Select Folder") : QStringLiteral("Open File"))
                    : QStringLiteral("Save File");
    setWindowTitle(title);

    m_keys.bind(QStringLiteral("<C-CR>"), QStringLiteral("choose"), QStringLiteral("File chooser: accept"));
    m_keys.bind(QStringLiteral("<A-f>"), QStringLiteral("choose-filter"), QStringLiteral("File chooser: next file type"));

    QString start = request.currentFolder;
    QString select;
    if (!request.currentFile.isEmpty()) {
        start = parentDir(request.currentFile);
        select = baseName(request.currentFile);
    }
    if (!start.isEmpty() && QFileInfo(start).isDir())
        cd(start, select, false);
    if (QLineEdit *edit = m_chooserBar->nameEdit()) {
        edit->installEventFilter(this);
        m_chooserBar->setName(!request.currentName.isEmpty() ? request.currentName : baseName(request.currentFile));
        QTimer::singleShot(0, edit, [edit] { edit->setFocus(Qt::OtherFocusReason); });
    }
    applyTheme();
    updateChooserHint();
}

void MainWindow::updateChooserHint() {
    if (!m_chooserBar)
        return;
    QString accept = m_chooser.acceptLabel;
    accept.remove(QRegularExpression(QStringLiteral("_(?!_)")));
    QString hint;
    switch (m_chooser.mode) {
    case ChooserRequest::Mode::Open:
        if (m_chooser.directory) {
            if (accept.isEmpty())
                accept = QStringLiteral("Select");
            accept += QStringLiteral(" “%1”").arg(m_cwd == QDir::homePath() ? QStringLiteral("~") : baseName(m_cwd).isEmpty() ? QStringLiteral("/") : baseName(m_cwd));
            hint = QStringLiteral("l open folder · Ctrl-Enter select this folder · Esc cancel");
        } else {
            if (accept.isEmpty())
                accept = QStringLiteral("Open");
            hint = m_chooser.multiple ? QStringLiteral("Enter choose · Space mark several · Ctrl-Enter choose marked · Esc cancel")
                                      : QStringLiteral("Enter choose · Esc cancel");
        }
        break;
    case ChooserRequest::Mode::Save:
        if (accept.isEmpty())
            accept = QStringLiteral("Save");
        hint = QStringLiteral("Tab name ⇄ folders · Enter save · Esc cancel");
        break;
    case ChooserRequest::Mode::SaveFiles:
        if (accept.isEmpty())
            accept = QStringLiteral("Save here");
        hint = QStringLiteral("l open folder · Ctrl-Enter save %1 here · Esc cancel").arg(plural(int(m_chooser.files.size()), "file"));
        break;
    }
    if (!m_chooser.filters.isEmpty())
        hint += QStringLiteral(" · Alt-f file type");
    m_chooserBar->setHint(hint);
    m_chooserBar->setAcceptText(accept);
}

void MainWindow::chooserAccept(bool fromCursor) {
    const FileEntry *e = currentEntry();
    switch (m_chooser.mode) {
    case ChooserRequest::Mode::Open: {
        QStringList paths;
        if (m_chooser.multiple && !m_marks.isEmpty()) {
            for (const QString &p : m_marks.paths())
                if (QFileInfo(p).isDir() == m_chooser.directory)
                    paths << p;
        } else if (m_chooser.directory) {
            paths << m_cwd;
        } else if (e && !e->isDir) {
            paths << e->path;
        }
        if (paths.isEmpty()) {
            error(m_chooser.directory ? QStringLiteral("mark folders to choose them") : QStringLiteral("pick a file"));
            return;
        }
        chooserAcceptPaths(paths);
        break;
    }
    case ChooserRequest::Mode::Save:
        if (fromCursor && e && !e->isDir) {
            m_chooserBar->setName(e->name);
            m_chooserBar->nameEdit()->setFocus(Qt::OtherFocusReason);
            return;
        }
        chooserSave(m_chooserBar->name());
        break;
    case ChooserRequest::Mode::SaveFiles: {
        QStringList paths;
        QSet<QString> taken;
        for (const QString &name : m_chooser.files) {
            QString dest = uniqueDestination(m_cwd, baseName(name));
            // Two files with the same name must not get the same destination.
            for (int i = 2; taken.contains(dest) && i < 1000; ++i)
                dest = uniqueDestination(m_cwd, QStringLiteral("%1 (%2)").arg(baseName(name)).arg(i));
            taken.insert(dest);
            paths << dest;
        }
        chooserAcceptPaths(paths);
        break;
    }
    }
}

void MainWindow::chooserSave(const QString &input) {
    const QString name = expandHome(input.trimmed());
    if (name.isEmpty()) {
        error(QStringLiteral("type a file name"));
        m_chooserBar->nameEdit()->setFocus(Qt::OtherFocusReason);
        return;
    }
    const QString path = QDir::cleanPath(QDir(m_cwd).filePath(name));
    const QFileInfo info(path);
    if (info.isDir()) {
        cd(path);
        m_chooserBar->setName(m_chooser.currentName);
        return;
    }
    if (!QFileInfo(parentDir(path)).isDir()) {
        error(QStringLiteral("no such folder: %1").arg(tilde(parentDir(path))));
        return;
    }
    if (!info.exists() && !info.isSymLink()) {
        chooserAcceptPaths({path});
        return;
    }
    PromptSpec spec;
    spec.label = QStringLiteral("replace %1? [y/N]").arg(baseName(path));
    spec.onChange = [this, path](const QString &text) {
        if (text.isEmpty())
            return;
        m_prompt->close(true);
        if (text == QLatin1String("y") || text == QLatin1String("Y"))
            chooserAcceptPaths({path});
        else if (m_chooserBar)
            m_chooserBar->nameEdit()->setFocus(Qt::OtherFocusReason);
    };
    openPrompt(spec);
}

void MainWindow::chooserAcceptPaths(const QStringList &paths) {
    if (!m_chooserDone)
        return;
    ChooserResult result;
    result.accepted = true;
    result.paths = paths;
    result.filter = m_chooserBar->filterIndex();
    result.choices = m_chooserBar->choices();
    auto done = std::exchange(m_chooserDone, {});
    done(result);
    m_closeReason = QStringLiteral("file chooser accepted");
    QTimer::singleShot(0, this, &QWidget::close);
}

void MainWindow::finishChooser(bool accepted) {
    if (accepted) {
        chooserAccept(false);
        return;
    }
    if (!m_chooserDone)
        return;
    ChooserResult result;
    result.filter = m_chooserBar ? m_chooserBar->filterIndex() : -1;
    auto done = std::exchange(m_chooserDone, {});
    done(result);
    m_closeReason = QStringLiteral("file chooser cancelled");
    if (isVisible())
        QTimer::singleShot(0, this, &QWidget::close);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    logging::info(QStringLiteral("window closed by %1: %2 (%3)")
                      .arg(m_closeReason.isEmpty() ? QStringLiteral("the window manager") : m_closeReason, m_cwd,
                           plural(int(m_tabs.size()), "tab")));
    m_closeReason.clear();
    if (!isChooser()) {
        saveTab();
        state::Session session;
        for (const Tab &t : std::as_const(m_tabs))
            session.tabs << t.cwd;
        session.active = m_tab;
        state::setSession(session);
    }
    if (m_chooserDone) {
        ChooserResult result;
        auto done = std::exchange(m_chooserDone, {});
        done(result);
    }
    QWidget::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress && watched->isWidgetType() &&
        static_cast<QWidget *>(watched)->window() == this) {
        if (m_keys.isPending() || m_whichKey->isVisible())
            cancelPendingKeys();
        // A right press moves the cursor before the context menu opens; the menu acts on the range as it was.
        if (watched == m_currentView->viewport() && static_cast<QMouseEvent *>(event)->button() == Qt::RightButton)
            endVisual();
    }
    if (m_chooserBar && watched == m_chooserBar->nameEdit() && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            setFocus(Qt::TabFocusReason);
            return true;
        case Qt::Key_Escape:
            finishChooser(false);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            chooserSave(m_chooserBar->name());
            return true;
        case Qt::Key_Up:
            moveCursor(-1);
            return true;
        case Qt::Key_Down:
            moveCursor(1);
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
