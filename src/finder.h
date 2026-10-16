#pragma once

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

// Walks a directory tree breadth first on a worker thread and reports paths
// relative to the root in batches. Directories carry a trailing slash.
class FileWalker : public QObject {
    Q_OBJECT
public:
    explicit FileWalker(QObject *parent = nullptr);
    ~FileWalker() override;

    void start(const QString &root, bool includeHidden, int limit = 300000);
    void cancel();
    QString root() const { return m_root; }
    bool isRunning() const { return m_running; }

    static bool skipDirectory(const QString &name, const QString &absolutePath);

signals:
    void batch(const QStringList &relativePaths);
    void finished(bool truncated);

private:
    QString m_root;
    std::shared_ptr<std::atomic_bool> m_cancel;
    bool m_running = false;
    quint64 m_generation = 0;
};
