#include "finder.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QtConcurrent>

#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

FileWalker::FileWalker(QObject *parent) : QObject(parent) {}

FileWalker::~FileWalker() { cancel(); }

bool FileWalker::skipDirectory(const QString &name, const QString &absolutePath) {
    static const QSet<QString> names = {
        QStringLiteral(".git"),    QStringLiteral(".hg"),         QStringLiteral(".svn"),
        QStringLiteral(".jj"),     QStringLiteral("node_modules"), QStringLiteral(".direnv"),
        QStringLiteral(".venv"),   QStringLiteral("__pycache__"), QStringLiteral(".mypy_cache"),
        QStringLiteral(".ruff_cache"), QStringLiteral(".pytest_cache"), QStringLiteral(".zig-cache"),
        QStringLiteral(".cache"),  QStringLiteral(".npm"),        QStringLiteral(".cargo"),
        QStringLiteral(".rustup"),
    };
    static const QSet<QString> absolute = {
        QStringLiteral("/proc"), QStringLiteral("/sys"), QStringLiteral("/dev"), QStringLiteral("/run"),
        QStringLiteral("/nix/store"), QStringLiteral("/tmp"),
    };
    return names.contains(name) || name.startsWith(QLatin1String(".Trash")) || absolute.contains(absolutePath) ||
           absolutePath.endsWith(QLatin1String("/.local/share/Trash"));
}

void FileWalker::cancel() {
    if (m_cancel)
        *m_cancel = true;
    m_running = false;
}

void FileWalker::start(const QString &root, bool includeHidden, int limit) {
    cancel();
    m_root = root;
    m_running = true;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancelled;
    const quint64 generation = ++m_generation;
    QPointer<FileWalker> self(this);

    auto deliver = [self, generation](QStringList paths, bool done, bool truncated) {
        QMetaObject::invokeMethod(
            self.data(),
            [self, generation, paths = std::move(paths), done, truncated] {
                if (!self || generation != self->m_generation)
                    return;
                if (!paths.isEmpty())
                    emit self->batch(paths);
                if (done) {
                    self->m_running = false;
                    emit self->finished(truncated);
                }
            },
            Qt::QueuedConnection);
    };

    QThreadPool::globalInstance()->start([root, includeHidden, limit, cancelled, deliver] {
        std::deque<QString> queue{QString()};
        QStringList pending;
        int count = 0;
        bool truncated = false;
        QElapsedTimer flushTimer;
        flushTimer.start();
        const QString base = root.endsWith(u'/') ? root : root + u'/';
        while (!queue.empty() && !*cancelled) {
            const QString rel = queue.front();
            queue.pop_front();
            const QByteArray dirPath = QFile::encodeName(base + rel);
            const int fd = open(dirPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                continue;
            DIR *dir = fdopendir(fd);
            if (!dir) {
                close(fd);
                continue;
            }
            while (dirent *de = readdir(dir)) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                if (!includeHidden && de->d_name[0] == '.')
                    continue;
                const QString name = QFile::decodeName(de->d_name);
                bool isDir = de->d_type == DT_DIR;
                if (de->d_type == DT_UNKNOWN) {
                    struct stat st {};
                    isDir = fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
                }
                const QString relPath = rel + name;
                if (isDir) {
                    if (skipDirectory(name, base + relPath))
                        continue;
                    pending << relPath + u'/';
                    queue.push_back(relPath + u'/');
                } else {
                    pending << relPath;
                }
                if (++count >= limit) {
                    truncated = true;
                    break;
                }
            }
            closedir(dir);
            if (truncated)
                break;
            if (flushTimer.elapsed() > 40 && !pending.isEmpty()) {
                deliver(std::exchange(pending, {}), false, false);
                flushTimer.restart();
            }
        }
        if (!*cancelled)
            deliver(pending, true, truncated);
    });
}
