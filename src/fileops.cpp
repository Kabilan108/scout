#include "fileops.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThreadPool>

#include <cerrno>
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
    return joinPath(dir, name + QStringLiteral(".copy"));
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
            return true;
        }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(d.constData(), (st.st_mode & 07777) | S_IRWXU) != 0) {
                error(dst, errnoString());
                return false;
            }
            QString err;
            const QStringList children = childNames(s, &err);
            if (!err.isEmpty())
                error(src, err);
            bool ok = err.isEmpty();
            for (const QString &child : children)
                ok = copyTree(joinPath(src, child), joinPath(dst, child)) && ok;
            chmod(d.constData(), st.st_mode & 07777);
            const struct timespec times[2] = {st.st_atim, st.st_mtim};
            utimensat(AT_FDCWD, d.constData(), times, 0);
            return ok;
        }
        if (S_ISREG(st.st_mode))
            return copyFile(src, dst, st);
        error(src, QStringLiteral("unsupported file type"));
        return false;
    }

    bool removeTree(const QString &path) {
        struct stat st {};
        const QByteArray p = QFile::encodeName(path);
        if (lstat(p.constData(), &st) != 0) {
            error(path, errnoString());
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            QString err;
            bool ok = true;
            for (const QString &child : childNames(p, &err)) {
                if (m_cancelled())
                    return false;
                ok = removeTree(joinPath(path, child)) && ok;
            }
            if (!err.isEmpty()) {
                error(path, err);
                return false;
            }
            if (ok && rmdir(p.constData()) != 0) {
                error(path, errnoString());
                return false;
            }
            return ok;
        }
        if (unlink(p.constData()) != 0) {
            error(path, errnoString());
            return false;
        }
        return true;
    }

private:
    std::function<void(const std::function<void(Task::Progress &)> &)> m_update;
    std::function<bool()> m_cancelled;
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
            if (m_kind != Link && (target == src || target.startsWith(src + u'/'))) {
                worker.error(src, QStringLiteral("cannot put a folder inside itself"));
                break;
            }
            if (m_kind == Move && parentOf(src) == target)
                break;
            const QString dest = uniqueDestination(target, baseName(src));
            bool ok = false;
            if (m_kind == Copy) {
                ok = worker.copyTree(src, dest);
            } else if (m_kind == Link) {
                ok = symlink(s.constData(), QFile::encodeName(dest).constData()) == 0;
                if (!ok)
                    worker.error(dest, errnoString());
            } else if (::rename(s.constData(), QFile::encodeName(dest).constData()) == 0) {
                ok = true;
            } else if (errno == EXDEV) {
                qint64 bytes = 0;
                int items = 0;
                measure(src, &bytes, &items);
                update([&](Progress &p) { p.totalBytes += bytes; });
                ok = worker.copyTree(src, dest) && worker.removeTree(src);
            } else {
                worker.error(src, errnoString());
            }
            if (ok)
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
    taskPool()->waitForDone(5000);
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
