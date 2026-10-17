#include "dirmodel.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QThreadPool *listingPool() {
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(4);
        return p;
    }();
    return pool;
}

QString readLinkAt(int dirfd, const char *name) {
    QByteArray buf(4096, Qt::Uninitialized);
    const ssize_t n = readlinkat(dirfd, name, buf.data(), buf.size());
    if (n < 0)
        return {};
    buf.truncate(int(n));
    return QFile::decodeName(buf);
}

void fillFromStat(FileEntry &e, const struct stat &st) {
    e.isDir = S_ISDIR(st.st_mode);
    e.isRegular = S_ISREG(st.st_mode);
    e.size = st.st_size;
    e.mtime = st.st_mtim.tv_sec;
    e.mode = st.st_mode;
    e.uid = st.st_uid;
    e.gid = st.st_gid;
    e.isExecutable = !e.isDir && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

void statInto(FileEntry &e, int dirfd, const char *name) {
    struct stat st {};
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return;
    if (S_ISLNK(st.st_mode)) {
        e.isSymlink = true;
        e.linkTarget = readLinkAt(dirfd, name);
        struct stat target {};
        if (fstatat(dirfd, name, &target, 0) == 0) {
            fillFromStat(e, target);
        } else {
            fillFromStat(e, st);
            e.isBrokenLink = true;
        }
        e.mode = (e.mode & ~S_IFMT) | S_IFLNK;
        return;
    }
    fillFromStat(e, st);
}

QString extensionOf(const QString &name) {
    const int dot = int(name.lastIndexOf(u'.'));
    return dot > 0 ? name.mid(dot + 1).toLower() : QString();
}

} // namespace

void sortEntries(QVector<FileEntry> &entries, const ViewOptions &opt) {
    std::stable_sort(entries.begin(), entries.end(), [&opt](const FileEntry &a, const FileEntry &b) {
        if (opt.dirsFirst && a.isDir != b.isDir)
            return a.isDir;
        int c = 0;
        switch (opt.sort) {
        case ViewOptions::Size:
            c = a.size == b.size ? 0 : (a.size > b.size ? -1 : 1);
            break;
        case ViewOptions::Modified:
            c = a.mtime == b.mtime ? 0 : (a.mtime > b.mtime ? -1 : 1);
            break;
        case ViewOptions::Extension:
            c = naturalCompare(extensionOf(a.name), extensionOf(b.name));
            break;
        case ViewOptions::Name:
            break;
        }
        if (c == 0)
            c = naturalCompare(a.name, b.name);
        return opt.reverse ? c > 0 : c < 0;
    });
}

QString humanSize(qint64 bytes) {
    static const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 5) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(value < 10 ? QString::number(value, 'f', 1) : QString::number(qRound(value)),
                                       QLatin1String(units[unit]));
}

QString permissionString(const FileEntry &e) {
    QString s(10, u'-');
    if (e.isSymlink)
        s[0] = u'l';
    else if (e.isDir)
        s[0] = u'd';
    else if (S_ISCHR(e.mode))
        s[0] = u'c';
    else if (S_ISBLK(e.mode))
        s[0] = u'b';
    else if (S_ISFIFO(e.mode))
        s[0] = u'p';
    else if (S_ISSOCK(e.mode))
        s[0] = u's';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (e.mode & (1 << (8 - i)))
            s[i + 1] = QChar::fromLatin1(rwx[i]);
    if (e.mode & S_ISUID)
        s[3] = (e.mode & S_IXUSR) ? u's' : u'S';
    if (e.mode & S_ISGID)
        s[6] = (e.mode & S_IXGRP) ? u's' : u'S';
    if (e.mode & S_ISVTX)
        s[9] = (e.mode & S_IXOTH) ? u't' : u'T';
    return s;
}

int naturalCompare(QStringView a, QStringView b) {
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            qsizetype si = i, sj = j;
            while (si < a.size() && a[si] == u'0')
                ++si;
            while (sj < b.size() && b[sj] == u'0')
                ++sj;
            qsizetype ei = si, ej = sj;
            while (ei < a.size() && a[ei].isDigit())
                ++ei;
            while (ej < b.size() && b[ej].isDigit())
                ++ej;
            if (ei - si != ej - sj)
                return ei - si < ej - sj ? -1 : 1;
            const int c = a.sliced(si, ei - si).compare(b.sliced(sj, ej - sj));
            if (c != 0)
                return c;
            i = ei;
            j = ej;
            continue;
        }
        const QChar ca = a[i].toCaseFolded();
        const QChar cb = b[j].toCaseFolded();
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return a.compare(b);
}

FileEntry statEntry(const QString &path) {
    FileEntry e;
    const QFileInfo info(path);
    e.path = info.absoluteFilePath();
    e.name = info.fileName().isEmpty() ? e.path : info.fileName();
    const QByteArray dir = QFile::encodeName(info.absolutePath());
    const QByteArray name = QFile::encodeName(info.fileName().isEmpty() ? QStringLiteral(".") : info.fileName());
    const int dirfd = open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd >= 0) {
        statInto(e, dirfd, name.constData());
        close(dirfd);
    }
    return e;
}

QVector<FileEntry> listDirectory(const QString &path, QString *error) {
    QVector<FileEntry> entries;
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = QString::fromLocal8Bit(strerror(errno));
        return entries;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        if (error)
            *error = QString::fromLocal8Bit(strerror(errno));
        close(fd);
        return entries;
    }
    const QString prefix = path.endsWith(u'/') ? path : path + u'/';
    while (dirent *de = readdir(dir)) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        FileEntry e;
        e.name = QFile::decodeName(de->d_name);
        e.path = prefix + e.name;
        statInto(e, fd, de->d_name);
        entries.append(std::move(e));
    }
    closedir(dir);
    return entries;
}

QStringList Marks::paths() const {
    QStringList out(m_paths.begin(), m_paths.end());
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) { return naturalCompare(a, b) < 0; });
    return out;
}

void Marks::toggle(const QString &path) {
    if (!m_paths.remove(path))
        m_paths.insert(path);
    emit changed();
}

void Marks::add(const QStringList &paths) {
    for (const QString &p : paths)
        m_paths.insert(p);
    emit changed();
}

void Marks::remove(const QStringList &paths) {
    for (const QString &p : paths)
        m_paths.remove(p);
    emit changed();
}

void Marks::clear() {
    if (m_paths.isEmpty())
        return;
    m_paths.clear();
    emit changed();
}

DirModel::DirModel(Marks *marks, QObject *parent) : QAbstractListModel(parent), m_marks(marks) {
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(120);
    connect(&m_reloadTimer, &QTimer::timeout, this, &DirModel::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
    if (m_marks)
        connect(m_marks, &Marks::changed, this, [this] {
            if (!m_rows.isEmpty())
                emit dataChanged(index(0), index(int(m_rows.size()) - 1));
        });
}

void DirModel::setPath(const QString &path) {
    if (path == m_path && (m_loaded || m_loading))
        return;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_path = path;
    m_filter.clear();
    m_loaded = false;
    if (!path.isEmpty())
        m_watcher.addPath(path);
    startLoad();
}

void DirModel::clearPath() {
    ++m_generation;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    beginResetModel();
    m_path.clear();
    m_all.clear();
    m_rows.clear();
    m_error.clear();
    m_filter.clear();
    m_loaded = false;
    m_loading = false;
    endResetModel();
    emit refreshed();
}

void DirModel::reload() {
    if (m_path.isEmpty())
        return;
    if (m_watcher.directories().isEmpty())
        m_watcher.addPath(m_path);
    startLoad();
}

void DirModel::startLoad() {
    const quint64 generation = ++m_generation;
    const QString path = m_path;
    const ViewOptions options = m_options;
    m_loading = true;
    QPointer<DirModel> self(this);
    // Sorting runs here too: natural-order sorting /nix/store takes long enough to stall the UI.
    QtConcurrent::run(listingPool(), [path, options] {
        QString error;
        QVector<FileEntry> entries = listDirectory(path, &error);
        sortEntries(entries, options);
        return std::make_pair(std::move(entries), error);
    }).then(this, [self, generation](std::pair<QVector<FileEntry>, QString> result) {
        if (!self || generation != self->m_generation)
            return;
        self->beginResetModel();
        self->m_all = std::move(result.first);
        self->m_error = result.second;
        self->m_loaded = true;
        self->m_loading = false;
        self->m_dropTarget = -1;
        self->rebuildRows();
        self->endResetModel();
        emit self->refreshed();
    });
}

void DirModel::setOptions(const ViewOptions &options) {
    const bool resort = options.sort != m_options.sort || options.reverse != m_options.reverse ||
                        options.dirsFirst != m_options.dirsFirst;
    m_options = options;
    if (resort && !m_path.isEmpty()) {
        startLoad();
        return;
    }
    if (!m_loaded)
        return;
    beginResetModel();
    rebuildRows();
    endResetModel();
    emit refreshed();
}

void DirModel::setFilter(const QString &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    if (!m_loaded)
        return;
    beginResetModel();
    rebuildRows();
    endResetModel();
    emit refreshed();
}

void DirModel::setFileFilter(std::function<bool(const FileEntry &)> accept) {
    m_fileFilter = std::move(accept);
    if (!m_loaded)
        return;
    beginResetModel();
    rebuildRows();
    endResetModel();
    emit refreshed();
}

void DirModel::rebuildRows() {
    m_rows.clear();
    m_rows.reserve(m_all.size());
    const bool caseSensitive = std::any_of(m_filter.begin(), m_filter.end(), [](QChar c) { return c.isUpper(); });
    for (int i = 0; i < m_all.size(); ++i) {
        const FileEntry &e = m_all[i];
        if (!m_options.showHidden && e.isHidden())
            continue;
        if (!m_filter.isEmpty() && !e.name.contains(m_filter, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive))
            continue;
        if (m_fileFilter && !e.isDir && !m_fileFilter(e))
            continue;
        m_rows.append(i);
    }
}

const FileEntry *DirModel::entry(int row) const {
    if (row < 0 || row >= m_rows.size())
        return nullptr;
    return &m_all[m_rows[row]];
}

int DirModel::rowOf(const QString &name) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_all[m_rows[i]].name == name)
            return i;
    return -1;
}

void DirModel::setDropTarget(int row) {
    if (row == m_dropTarget)
        return;
    const int old = m_dropTarget;
    m_dropTarget = row;
    if (old >= 0 && old < m_rows.size())
        emit dataChanged(index(old), index(old));
    if (row >= 0 && row < m_rows.size())
        emit dataChanged(index(row), index(row));
}

int DirModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant DirModel::data(const QModelIndex &index, int role) const {
    const FileEntry *e = entry(index.row());
    if (!e)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        return e->name;
    case Qt::ToolTipRole:
        return e->isSymlink ? QStringLiteral("%1 → %2").arg(e->name, e->linkTarget) : QVariant();
    default:
        return {};
    }
}

Qt::ItemFlags DirModel::flags(const QModelIndex &index) const {
    const FileEntry *e = entry(index.row());
    if (!e)
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (e->isDir)
        f |= Qt::ItemIsDropEnabled;
    return f;
}

QStringList DirModel::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }

QMimeData *DirModel::mimeData(const QModelIndexList &indexes) const {
    QList<QUrl> urls;
    for (const QModelIndex &i : indexes)
        if (const FileEntry *e = entry(i.row()))
            urls << QUrl::fromLocalFile(e->path);
    auto *data = new QMimeData;
    data->setUrls(urls);
    return data;
}

Qt::DropActions DirModel::supportedDragActions() const {
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

Qt::DropActions DirModel::supportedDropActions() const {
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}
