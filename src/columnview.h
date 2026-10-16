#pragma once

#include <QListView>
#include <QStyledItemDelegate>

#include "dirmodel.h"

enum class EntryKind { Directory, File, Executable, Image, Video, Audio, Archive, Document, Code, Symlink, Broken };

EntryKind entryKind(const FileEntry &e);
QColor kindColor(EntryKind kind);
void paintEntryIcon(QPainter *p, const QRectF &box, EntryKind kind);

// Starts a drag-and-drop of local files from `source`. Must be called from a
// mouse press/move handler, since Wayland ties drags to a pointer serial.
Qt::DropAction startFileDrag(QWidget *source, const QStringList &paths);

class EntryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

class ColumnView : public QListView {
    Q_OBJECT
public:
    enum class Role { Parent, Current, Preview };

    ColumnView(DirModel *model, Role role, QWidget *parent = nullptr);

    DirModel *dirModel() const { return m_model; }
    Role role() const { return m_role; }
    bool isActive() const { return m_role == Role::Current; }
    bool showsCursor() const { return m_role != Role::Preview || m_showCursor; }
    void setShowCursor(bool show);

    int currentRow() const { return currentIndex().row(); }
    void setCurrentRow(int row);
    int visibleRows() const;

signals:
    void dropRequested(const QStringList &paths, const QString &targetDir, Qt::DropAction action);
    void markToggleRequested(int row);
    void markRangeRequested(int row);
    void rowClicked(int row);
    void rowActivated(int row);
    void contextMenuRequested(int row, const QPoint &globalPos);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    Qt::DropAction chooseDropAction(const QDropEvent *event) const;
    QString dropTargetDir(const QPoint &pos, int *row) const;

    DirModel *m_model;
    Role m_role;
    bool m_showCursor = false;
    int m_pressedRow = -1;
};
