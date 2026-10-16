#include "fileops.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QThreadPool>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QThreadPool *taskPool() {
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}

QString errnoString() { return QString::fromLocal8Bit(strerror(errno)); }

QString parentOf(const QString &path) {
    QString p = path;
    while (p.size() > 1 && p.endsWith(u'/'))
        p.chop(1);
    const int slash = int(p.lastIndexOf(u'/'));
    return slash <= 0 ? QStringLiteral("/") : p.left(slash);
}

QString baseName(const QString &path) {
    QString p = path;
    while (p.size() > 1 && p.endsWith(u'/'))
        p.chop(1);
    return p.mid(p.lastIndexOf(u'/') + 1);
}

QString joinPath(const QString &dir, const QString &name) {
    return dir.endsWith(u'/') ? dir + name : dir + u'/' + name;
}

QStringList childNames(const QByteArray &dir, QString *error) {
    QStringList names;
    DIR *d = opendir(dir.constData());
    if (!d) {
        *error = errnoString();
        return names;
    }
    while (dirent *de = readdir(d)) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        names << QFile::decodeName(de->d_name);
    }
    closedir(d);
    return names;
}

void measure(const QString &path, qint64 *bytes, int *items) {
    struct stat st {};
    const QByteArray enc = QFile::encodeName(path);
    if (lstat(enc.constData(), &st) != 0)
        return;
    ++*items;
    if (S_ISDIR(st.st_mode)) {
        QString err;
        for (const QString &child : childNames(enc, &err))
            measure(joinPath(path, child), bytes, items);
    } else if (S_ISREG(st.st_mode)) {
        *bytes += st.st_size;
    }
}

} // namespace

QString uniqueDestination(const QString &dir, const QString &name) {
    QString candidate = joinPath(dir, name);
    struct stat st {};
    if (lstat(QFile::encodeName(candidate).constData(), &st) != 0)
        return candidate;
    QString stem = name;
    QString ext;
    static const QStringList doubleExts = {QStringLiteral(".tar.gz"), QStringLiteral(".tar.xz"),
                                           QStringLiteral(".tar.bz2"), QStringLiteral(".tar.zst")};
    for (const QString &d : doubleExts) {
        if (name.endsWith(d, Qt::CaseInsensitive) && name.size() > d.size()) {
            stem = name.left(name.size() - d.size());
            ext = name.right(d.size());
            break;
        }
    }
    if (ext.isEmpty()) {
        const int dot = int(name.lastIndexOf(u'.'));
        if (dot > 0) {
            stem = name.left(dot);
            ext = name.mid(dot);
        }
    }
    for (int i = 2; i < 10000; ++i) {
        candidate = joinPath(dir, QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(ext));
        if (lstat(QFile::encodeName(candidate).constData(), &st) != 0)
            return candidate;
    }
    return {};
}

int renameNoReplace(const QString &from, const QString &to) {
    const QByteArray f = QFile::encodeName(from), t = QFile::encodeName(to);
    if (renameat2(AT_FDCWD, f.constData(), AT_FDCWD, t.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP)
        return errno;
    return renameByReservation(from, to);
}

int renameByReservation(const QString &from, const QString &to) {
    const QByteArray f = QFile::encodeName(from), t = QFile::encodeName(to);
    // Filesystems without RENAME_NOREPLACE: atomically reserve the destination,
    // then rename over our own reservation. rename() only replaces an empty
    // directory, so a folder someone fills in the meantime makes it fail.
    struct stat st {};
    if (lstat(f.constData(), &st) != 0)
        return errno;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(t.constData(), 0700) != 0)
            return errno;
    } else {
        if (link(f.constData(), t.constData()) == 0)
            return unlink(f.constData()) == 0 ? 0 : errno;
        if (errno == EEXIST)
            return EEXIST;
        const int fd = open(t.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            return errno;
        close(fd);
    }
    if (::rename(f.constData(), t.constData()) == 0)
        return 0;
    const int err = errno;
    if (S_ISDIR(st.st_mode))
        rmdir(t.constData());
    else
        unlink(t.constData());
    return err;
}

QString moveToUnique(const QString &from, const QString &dir, const QString &name, int *errorNumber) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString dest = uniqueDestination(dir, name);
        if (dest.isEmpty())
            break;
        const int err = renameNoReplace(from, dest);
        if (err == 0)
            return dest;
        if (err != EEXIST) {
            *errorNumber = err;
            return {};
        }
    }
    *errorNumber = EEXIST;
    return {};
}

bool isSameOrInside(const QString &dir, const QString &folder) {
    if (dir == folder || dir.startsWith(folder + u'/'))
        return true;
    struct stat st {};
    // Moving or copying a symlink never recurses into its target.
    if (lstat(QFile::encodeName(folder).constData(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    const QString realDir = QFileInfo(dir).canonicalFilePath();
    const QString realFolder = QFileInfo(folder).canonicalFilePath();
    return !realDir.isEmpty() && !realFolder.isEmpty() &&
           (realDir == realFolder || realDir.startsWith(realFolder + u'/'));
}

Task::Task(Kind kind, QStringList sources, QString targetDir, QObject *parent)
    : QObject(parent), m_kind(kind), m_sources(std::move(sources)), m_targetDir(std::move(targetDir)) {
    m_progress.totalItems = int(m_sources.size());
}

QString Task::verb() const {
    switch (m_kind) {
    case Copy: return QStringLiteral("Copying");
    case Move: return QStringLiteral("Moving");
    case Link: return QStringLiteral("Linking");
    case Trash: return QStringLiteral("Trashing");
    case Delete: return QStringLiteral("Deleting");
    }
    return {};
}

QString Task::title() const {
    const QString what = m_sources.size() == 1 ? baseName(m_sources.first())
                                               : QStringLiteral("%1 items").arg(m_sources.size());
    if (m_kind == Trash || m_kind == Delete)
        return QStringLiteral("%1 %2").arg(verb(), what);
    return QStringLiteral("%1 %2 to %3").arg(verb(), what, baseName(m_targetDir));
}

Task::Progress Task::progress() const {
    QMutexLocker lock(&m_mutex);
    return m_progress;
}

double Task::fraction() const {
    const Progress p = progress();
    if (p.state != Running)
        return 1.0;
    if (p.totalBytes > 0)
        return std::clamp(double(p.doneBytes) / double(p.totalBytes), 0.0, 1.0);
    return p.totalItems > 0 ? double(p.doneItems) / p.totalItems : 0.0;
}

void Task::update(const std::function<void(Progress &)> &fn) {
    QMutexLocker lock(&m_mutex);
    fn(m_progress);
}

namespace {

class Worker {
public:
    Worker(const std::function<void(const std::function<void(Task::Progress &)> &)> &update,
           const std::function<bool()> &cancelled)
        : m_update(update), m_cancelled(cancelled) {}

    void error(const QString &path, const QString &message) {
        m_update([&](Task::Progress &p) { p.errors << QStringLiteral("%1: %2").arg(path, message); });
    }

    bool copyFile(const QString &src, const QString &dst, const struct stat &st) {
        const QByteArray s = QFile::encodeName(src), d = QFile::encodeName(dst);
        const int in = open(s.constData(), O_RDONLY | O_CLOEXEC);
        if (in < 0) {
            error(src, errnoString());
            return false;
        }
        const int out = open(d.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
        if (out < 0) {
            error(dst, errnoString());
            close(in);
            return false;
        }
        noteCreated(dst);
        bool ok = true;
        bool useCopyRange = true;
        constexpr size_t kChunk = 8 << 20;
        std::vector<char> buffer;
        while (true) {
            if (m_cancelled()) {
                ok = false;
                break;
            }
            ssize_t n = -1;
            if (useCopyRange) {
                n = copy_file_range(in, nullptr, out, nullptr, kChunk, 0);
                if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
                    useCopyRange = false;
                    continue;
                }
            } else {
                if (buffer.empty())
                    buffer.resize(1 << 20);
                n = read(in, buffer.data(), buffer.size());
                if (n > 0) {
                    ssize_t written = 0;
                    while (written < n) {
                        const ssize_t w = write(out, buffer.data() + written, size_t(n - written));
                        if (w < 0) {
                            if (errno == EINTR)
                                continue;
                            n = -1;
                            break;
                        }
                        written += w;
                    }
                }
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                error(src, errnoString());
                ok = false;
                break;
            }
            if (n == 0)
                break;
            m_update([&](Task::Progress &p) { p.doneBytes += n; });
        }
        if (ok) {
            const struct timespec times[2] = {st.st_atim, st.st_mtim};
            futimens(out, times);
        }
        close(in);
        if (close(out) != 0 && ok) {
            error(dst, errnoString());
            ok = false;
        }
        if (!ok)
            unlink(d.constData());
        return ok;
    }

    bool copyTree(const QString &src, const QString &dst) {
        if (m_cancelled())
            return false;
        m_update([&](Task::Progress &p) { p.current = src; });
        struct stat st {};
        const QByteArray s = QFile::encodeName(src), d = QFile::encodeName(dst);
        if (lstat(s.constData(), &st) != 0) {
            error(src, errnoString());
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            QByteArray target(4096, Qt::Uninitialized);
            const ssize_t n = readlink(s.constData(), target.data(), target.size());
            if (n < 0) {
                error(src, errnoString());
                return false;
            }
            target.truncate(int(n));
            if (symlink(target.constData(), d.constData()) != 0) {
                error(dst, errnoString());
                return false;
            }
            noteCreated(dst);
            return true;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(d.constData(), (st.st_mode & 07777) | S_IRWXU) != 0) {
                error(dst, errnoString());
                return false;
            }
            noteCreated(dst);
            QString err;
            const QStringList children = childNames(s, &err);
            if (!err.isEmpty())
                error(src, err);
            bool ok = err.isEmpty();
            for (const QString &child : children)
                ok = copyTree(joinPath(src, child), joinPath(dst, child)) && ok;
            // A restrictive mode on a half-copied folder would block cleanup.
            if (ok) {
                chmod(d.constData(), st.st_mode & 07777);
                const struct timespec times[2] = {st.st_atim, st.st_mtim};
                utimensat(AT_FDCWD, d.constData(), times, 0);
            }
            return ok;
        }
        if (S_ISREG(st.st_mode))
            return copyFile(src, dst, st);
        error(src, QStringLiteral("unsupported file type"));
        return false;
    }

    // Deletes through directory descriptors opened with O_NOFOLLOW, so a folder
    // swapped for a symlink mid-delete can't redirect us into another tree.
    bool removeTree(const QString &path, bool honorCancel = true) {
        const QByteArray parent = QFile::encodeName(parentOf(path));
        const int parentFd = open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parentFd < 0) {
            error(path, errnoString());
            return false;
        }
        const bool ok = removeAt(parentFd, QFile::encodeName(baseName(path)), path, honorCancel);
        close(parentFd);
        return ok;
    }

    bool removeAt(int parentFd, const QByteArray &name, const QString &display, bool honorCancel) {
        struct stat st {};
        if (fstatat(parentFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            error(display, errnoString());
            return false;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlinkat(parentFd, name.constData(), 0) != 0) {
                error(display, errnoString());
                return false;
            }
            return true;
        }
        const int fd = openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            error(display, errnoString());
            return false;
        }
        struct stat opened {};
        if (fstat(fd, &opened) != 0 || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
            close(fd);
            error(display, QStringLiteral("changed while deleting"));
            return false;
        }
        DIR *dir = fdopendir(fd);
        if (!dir) {
            close(fd);
            error(display, errnoString());
            return false;
        }
        std::vector<QByteArray> children;
        while (dirent *de = readdir(dir)) {
            if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
                children.emplace_back(de->d_name);
        }
        bool ok = true;
        for (const QByteArray &child : children) {
            if (honorCancel && m_cancelled()) {
                closedir(dir);
                return false;
            }
            ok = removeAt(dirfd(dir), child, display + u'/' + QFile::decodeName(child), honorCancel) && ok;
        }
        closedir(dir);
        if (ok && unlinkat(parentFd, name.constData(), AT_REMOVEDIR) != 0) {
            error(display, errnoString());
            return false;
        }
        return ok;
    }

    // Copies into a hidden sibling and renames it into place only on success,
    // so failed or cancelled copies leave nothing behind.
    QString copyIntoPlace(const QString &src, const QString &target) {
        const QString name = baseName(src);
        const QString temp = uniqueDestination(
            target, QStringLiteral(".%1.scout-partial-%2").arg(name).arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
        if (temp.isEmpty()) {
            error(src, QStringLiteral("no free name in %1").arg(target));
            return {};
        }
        m_claimTarget = temp;
        m_claimed = false;
        const bool copied = copyTree(src, temp);
        m_claimTarget.clear();
        if (copied) {
            int err = 0;
            const QString dest = moveToUnique(temp, target, name, &err);
            if (!dest.isEmpty())
                return dest;
            error(src, QString::fromLocal8Bit(strerror(err)));
        }
        // Only remove the staging entry if this copy created it.
        if (m_claimed && !removeTree(temp, false))
            error(temp, QStringLiteral("partial copy left behind"));
        return {};
    }

    void noteCreated(const QString &path) {
        if (path == m_claimTarget)
            m_claimed = true;
    }

private:
    std::function<void(const std::function<void(Task::Progress &)> &)> m_update;
    std::function<bool()> m_cancelled;
    QString m_claimTarget;
    bool m_claimed = false;
};

} // namespace

void Task::run() {
    Worker worker([this](const std::function<void(Progress &)> &fn) { update(fn); },
                  [this] { return cancelled(); });

    if (m_kind == Copy) {
        qint64 bytes = 0;
        int items = 0;
        for (const QString &src : m_sources)
            measure(src, &bytes, &items);
        update([&](Progress &p) { p.totalBytes = bytes; });
    }

    const QString target = m_targetDir.size() > 1 && m_targetDir.endsWith(u'/') ? m_targetDir.chopped(1)
                                                                                : m_targetDir;
    for (const QString &src : m_sources) {
        if (cancelled())
            break;
        update([&](Progress &p) { p.current = src; });
        const QByteArray s = QFile::encodeName(src);

        switch (m_kind) {
        case Copy:
        case Move:
        case Link: {
            if (m_kind != Link && isSameOrInside(target, src)) {
                worker.error(src, QStringLiteral("cannot put a folder inside itself"));
                break;
            }
            if (m_kind == Move && parentOf(src) == target)
                break;
            QString dest;
            if (m_kind == Copy) {
                dest = worker.copyIntoPlace(src, target);
            } else if (m_kind == Link) {
                for (int attempt = 0; attempt < 64 && dest.isEmpty(); ++attempt) {
                    const QString candidate = uniqueDestination(target, baseName(src));
                    if (candidate.isEmpty())
                        break;
                    if (symlink(s.constData(), QFile::encodeName(candidate).constData()) == 0)
                        dest = candidate;
                    else if (errno != EEXIST)
                        break;
                }
                if (dest.isEmpty())
                    worker.error(src, errnoString());
            } else {
                int err = 0;
                dest = moveToUnique(src, target, baseName(src), &err);
                if (dest.isEmpty() && err == EXDEV) {
                    qint64 bytes = 0;
                    int items = 0;
                    measure(src, &bytes, &items);
                    update([&](Progress &p) { p.totalBytes += bytes; });
                    dest = worker.copyIntoPlace(src, target);
                    if (!dest.isEmpty() && !worker.removeTree(src, false))
                        worker.error(src, QStringLiteral("copied, but the original could not be fully removed"));
                } else if (dest.isEmpty()) {
                    worker.error(src, QString::fromLocal8Bit(strerror(err)));
                }
            }
            if (!dest.isEmpty())
                update([&](Progress &p) { p.created << dest; });
            break;
        }
        case Trash:
            if (!QFile::moveToTrash(src))
                worker.error(src, QStringLiteral("could not move to trash"));
            break;
        case Delete:
            worker.removeTree(src);
            break;
        }
        update([&](Progress &p) { ++p.doneItems; });
    }

    update([&](Progress &p) {
        p.current.clear();
        if (cancelled())
            p.state = Cancelled;
        else
            p.state = p.errors.isEmpty() ? Done : Failed;
    });
    QMetaObject::invokeMethod(this, &Task::finished, Qt::QueuedConnection);
}

TaskManager::TaskManager(QObject *parent) : QObject(parent) {
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        emit tasksChanged();
        if (activeCount() == 0)
            m_poll.stop();
    });
}

TaskManager::~TaskManager() {
    for (Task *t : m_tasks)
        t->cancel();
    taskPool()->clear();
    if (taskPool()->waitForDone(5000))
        return;
    // A worker is stuck in a syscall. Leak its task rather than free memory it still uses.
    for (Task *t : m_tasks)
        if (!m_delivered.contains(t))
            t->setParent(nullptr);
}

Task *TaskManager::start(Task::Kind kind, const QStringList &sources, const QString &targetDir) {
    auto *task = new Task(kind, sources, targetDir, this);
    m_tasks << task;
    connect(task, &Task::finished, this, [this, task] {
        m_delivered.insert(task);
        emit taskFinished(task);
        emit tasksChanged();
    });
    taskPool()->start([task] { task->run(); });
    m_poll.start();
    emit tasksChanged();
    return task;
}

int TaskManager::activeCount() const {
    return int(std::count_if(m_tasks.begin(), m_tasks.end(), [](Task *t) { return !t->isFinished(); }));
}

void TaskManager::clearFinished() {
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        if (m_delivered.contains(*it)) {
            m_delivered.remove(*it);
            (*it)->deleteLater();
            it = m_tasks.erase(it);
        } else {
            ++it;
        }
    }
    emit tasksChanged();
}

bool TaskManager::waitForAll(int timeoutMs) {
    const bool done = taskPool()->waitForDone(timeoutMs);
    QCoreApplication::processEvents();
    return done;
}
