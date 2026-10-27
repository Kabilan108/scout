#pragma once

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>

#include <sys/types.h>

struct FileEntry {
    QString name;
    QString path;
    bool isDir = false;      // true for symlinks to directories too
    bool isSymlink = false;
    bool isBrokenLink = false;
    bool isExecutable = false;
    bool isRegular = false;  // of the link target, for symlinks
    qint64 size = 0;
    qint64 mtime = 0;        // seconds since epoch
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    QString linkTarget;

    bool isHidden() const { return name.startsWith(u'.'); }
};

struct ViewOptions {
    enum SortKey { Name, Size, Modified, Extension };
    SortKey sort = Name;
    bool reverse = false;
    bool dirsFirst = true;
    bool showHidden = false;
};

QString humanSize(qint64 bytes);
QString permissionString(const FileEntry &e);
int naturalCompare(QStringView a, QStringView b);
FileEntry statEntry(const QString &path);
QVector<FileEntry> listDirectory(const QString &path, QString *error);
void sortEntries(QVector<FileEntry> &entries, const ViewOptions &options);

// Paths marked for a bulk operation. Marks persist across directories.
class Marks : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    bool contains(const QString &path) const { return m_paths.contains(path); }
    bool isEmpty() const { return m_paths.isEmpty(); }
    int size() const { return int(m_paths.size()); }
    QStringList paths() const;
    void toggle(const QString &path);
    void add(const QStringList &paths);
    void remove(const QStringList &paths);
    void clear();
    void set(const QSet<QString> &paths);
    QSet<QString> pathSet() const { return m_paths; }

signals:
    void changed();

private:
    QSet<QString> m_paths;
};

class DirModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit DirModel(Marks *marks, QObject *parent = nullptr);

    void setPath(const QString &path);
    QString path() const { return m_path; }
    bool isLoaded() const { return m_loaded; }
    bool isLoading() const { return m_loading; }
    QString error() const { return m_error; }
    void reload();
    void clearPath();

    void setOptions(const ViewOptions &options);
    void setFilter(const QString &filter);
    QString filter() const { return m_filter; }
    // Hides files (never folders) the predicate rejects; the file chooser's type filter.
    void setFileFilter(std::function<bool(const FileEntry &)> accept);

    const FileEntry *entry(int row) const;
    int rowOf(const QString &name) const;
    int totalCount() const { return int(m_all.size()); }
    Marks *marks() const { return m_marks; }

    void setDropTarget(int row);
    int dropTarget() const { return m_dropTarget; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;

signals:
    // Emitted after every completed load or re-filter so views can restore the cursor.
    void refreshed();

private:
    void startLoad();
    void rebuildRows();

    Marks *m_marks;
    QString m_path;
    QVector<FileEntry> m_all;
    QVector<int> m_rows;
    ViewOptions m_options;
    QString m_filter;
    std::function<bool(const FileEntry &)> m_fileFilter;
    QString m_error;
    bool m_loaded = false;
    bool m_loading = false;
    quint64 m_generation = 0;
    int m_dropTarget = -1;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
};
