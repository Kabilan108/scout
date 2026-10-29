#include "columnview.h"

#include "clipboard.h"
#include "theme.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QUrl>

namespace {

const QHash<QString, EntryKind> &extensionKinds() {
    static const QHash<QString, EntryKind> kinds = [] {
        QHash<QString, EntryKind> k;
        for (const char *e : {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "tif", "tiff", "ico", "avif", "heic",
                              "heif", "jxl", "xcf", "psd", "raw", "cr2", "nef", "dng"})
            k.insert(QLatin1String(e), EntryKind::Image);
        for (const char *e : {"mp4", "mkv", "webm", "mov", "avi", "m4v", "flv", "wmv", "mpg", "mpeg", "ogv"})
            k.insert(QLatin1String(e), EntryKind::Video);
        for (const char *e : {"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma", "aiff"})
            k.insert(QLatin1String(e), EntryKind::Audio);
        for (const char *e : {"zip", "tar", "gz", "tgz", "xz", "txz", "bz2", "tbz2", "zst", "7z", "rar", "iso", "deb",
                              "rpm", "jar", "apk", "lz", "lzma", "cpio", "ar", "whl", "nar"})
            k.insert(QLatin1String(e), EntryKind::Archive);
        for (const char *e : {"pdf", "epub", "doc", "docx", "odt", "xls", "xlsx", "ods", "ppt", "pptx", "odp", "md",
                              "txt", "rtf", "tex", "org", "rst"})
            k.insert(QLatin1String(e), EntryKind::Document);
        for (const char *e : {"c", "h", "cpp", "hpp", "cc", "cxx", "rs", "go", "py", "js", "ts", "tsx", "jsx", "zig",
                              "lua", "nix", "sh", "bash", "zsh", "fish", "rb", "java", "kt", "swift", "cs", "json",
                              "yaml", "yml", "toml", "xml", "html", "css", "scss", "sql", "vim", "el", "hs", "ml",
                              "ex", "exs", "erl", "dart", "php", "pl", "r", "jl", "cmake", "mk", "ini", "conf"})
            k.insert(QLatin1String(e), EntryKind::Code);
        return k;
    }();
    return kinds;
}

QPainterPath folderPath(const QRectF &r) {
    const qreal w = r.width(), h = r.height();
    const qreal x = r.x(), y = r.y() + h * 0.12;
    const qreal bodyH = h * 0.76;
    QPainterPath path;
    path.moveTo(x + 1, y + 1);
    path.lineTo(x + w * 0.38, y + 1);
    path.lineTo(x + w * 0.48, y + bodyH * 0.2);
    path.lineTo(x + w - 1, y + bodyH * 0.2);
    path.lineTo(x + w - 1, y + bodyH);
    path.lineTo(x + 1, y + bodyH);
    path.closeSubpath();
    return path;
}

QPainterPath filePath(const QRectF &r) {
    const qreal w = r.width() * 0.72, h = r.height() * 0.9;
    const qreal x = r.x() + (r.width() - w) / 2, y = r.y() + (r.height() - h) / 2;
    const qreal ear = w * 0.36;
    QPainterPath path;
    path.moveTo(x, y);
    path.lineTo(x + w - ear, y);
    path.lineTo(x + w, y + ear);
    path.lineTo(x + w, y + h);
    path.lineTo(x, y + h);
    path.closeSubpath();
    path.moveTo(x + w - ear, y);
    path.lineTo(x + w - ear, y + ear);
    path.lineTo(x + w, y + ear);
    return path;
}

} // namespace

EntryKind entryKind(const FileEntry &e) {
    if (e.isBrokenLink)
        return EntryKind::Broken;
    if (e.isDir)
        return EntryKind::Directory;
    if (e.isExecutable)
        return EntryKind::Executable;
    const int dot = int(e.name.lastIndexOf(u'.'));
    if (dot > 0) {
        auto it = extensionKinds().constFind(e.name.mid(dot + 1).toLower());
        if (it != extensionKinds().constEnd())
            return it.value();
    }
    return e.isSymlink ? EntryKind::Symlink : EntryKind::File;
}

QColor kindColor(EntryKind kind) {
    const Palette &p = Theme::p();
    switch (kind) {
    case EntryKind::Directory: return p.accent;
    case EntryKind::Executable: return p.green;
    case EntryKind::Image: return p.magenta;
    case EntryKind::Video: return p.orange;
    case EntryKind::Audio: return p.cyan;
    case EntryKind::Archive: return p.red;
    case EntryKind::Symlink: return p.cyan;
    case EntryKind::Broken: return p.red;
    case EntryKind::Document:
    case EntryKind::Code:
    case EntryKind::File: return p.subtext;
    }
    return p.subtext;
}

void paintEntryIcon(QPainter *p, const QRectF &box, EntryKind kind) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QColor color = kindColor(kind);
    if (kind == EntryKind::Directory) {
        QColor fill = color;
        fill.setAlphaF(0.9);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawPath(folderPath(box));
    } else {
        QPen pen(color, 1.2);
        pen.setJoinStyle(Qt::RoundJoin);
        p->setPen(pen);
        QColor fill = color;
        fill.setAlphaF(kind == EntryKind::File || kind == EntryKind::Document || kind == EntryKind::Code ? 0.12 : 0.3);
        p->setBrush(fill);
        p->drawPath(filePath(box));
    }
    p->restore();
}

Qt::DropAction startFileDrag(QWidget *source, const QStringList &paths) {
    if (paths.isEmpty())
        return Qt::IgnoreAction;
    auto *drag = new QDrag(source);
    drag->setMimeData(makeFileMimeData(paths, false));

    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    const QString label = paths.size() == 1 ? QFileInfo(paths.first()).fileName()
                                            : QStringLiteral("%1 items").arg(paths.size());
    const qreal dpr = source->devicePixelRatioF();
    const int iconSize = fm.height();
    const QSize size(std::min(fm.horizontalAdvance(label), 320) + iconSize + 28, fm.height() + 14);
    QPixmap pixmap(size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(pal.accent, 1));
        p.setBrush(pal.mantle);
        p.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1, size.height() - 1), 6, 6);
        const FileEntry first = statEntry(paths.first());
        paintEntryIcon(&p, QRectF(8, 7, iconSize, iconSize),
                       paths.size() == 1 ? entryKind(first) : EntryKind::File);
        p.setPen(pal.text);
        p.setFont(font);
        p.drawText(QRect(iconSize + 16, 0, size.width() - iconSize - 22, size.height()), Qt::AlignVCenter,
                   fm.elidedText(label, Qt::ElideMiddle, 320));
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(-12, -12));
    return drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
}

void EntryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    auto *model = qobject_cast<const DirModel *>(index.model());
    const FileEntry *e = model ? model->entry(index.row()) : nullptr;
    if (!e)
        return;
    auto *view = qobject_cast<const ColumnView *>(option.widget);
    const Palette &pal = Theme::p();
    const bool isCurrent = view && view->showsCursor() && view->currentIndex() == index;
    const bool active = view && view->isActive();
    const bool marked = model->marks() && model->marks()->contains(e->path);
    const bool dropTarget = model->dropTarget() == index.row();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF row = QRectF(option.rect).adjusted(3, 0.5, -3, -0.5);
    if (dropTarget) {
        // The cursor row's fill, which every text color is checked against; the dashes set it apart.
        painter->setPen(QPen(pal.accent, 1.5, Qt::DashLine));
        painter->setBrush(pal.surface);
        painter->drawRoundedRect(row.adjusted(0.75, 0.75, -0.75, -0.75), 4, 4);
    } else if (isCurrent) {
        painter->setBrush(pal.surface);
        painter->setPen(active ? QPen(pal.accent, 1.2) : Qt::NoPen);
        painter->drawRoundedRect(row.adjusted(0.6, 0.6, -0.6, -0.6), 4, 4);
    }
    if (marked) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(pal.yellow);
        painter->drawRoundedRect(QRectF(row.left() + 1, row.top() + 3, 3, row.height() - 6), 1.5, 1.5);
    }

    const QFontMetrics fm(option.font);
    const qreal iconSize = std::round(fm.height() * 0.8);
    const EntryKind kind = entryKind(*e);
    paintEntryIcon(painter, QRectF(row.left() + 8, row.center().y() - iconSize / 2, iconSize, iconSize), kind);

    QString right;
    if (!e->isDir)
        right = humanSize(e->size);
    if (e->isSymlink)
        right = right.isEmpty() ? QStringLiteral("→") : QStringLiteral("→ ") + right;
    const int rightWidth = right.isEmpty() ? 0 : fm.horizontalAdvance(right) + 10;

    QColor nameColor = pal.text;
    if (marked)
        nameColor = pal.yellow;
    else if (kind == EntryKind::Directory)
        nameColor = pal.accent;
    else if (kind == EntryKind::Executable)
        nameColor = pal.green;
    else if (kind == EntryKind::Broken)
        nameColor = pal.red;
    else if (e->isSymlink)
        nameColor = pal.cyan;
    const QColor rowBg = isCurrent || dropTarget ? pal.surface : (view && view->role() == ColumnView::Role::Parent ? pal.mantle : pal.base);
    // Dimmed, but never below readable contrast.
    if (e->isHidden() && !marked)
        nameColor = Palette::readable(Palette::mix(nameColor, rowBg, 0.35), {rowBg}, nameColor);

    const QRectF textRect(row.left() + 14 + iconSize, row.top(), row.width() - 20 - iconSize - rightWidth, row.height());
    const QString shown = fm.elidedText(e->name, Qt::ElideMiddle, int(textRect.width()));
    painter->setFont(option.font);
    const QPair<int, int> hit = view ? view->highlight().span(e->name) : QPair<int, int>{0, 0};
    if (hit.second > 0) {
        // Highlight just the match when the name is shown whole, else all of it.
        const bool whole = shown != e->name;
        const int start = whole ? 0 : hit.first;
        const int length = whole ? int(shown.size()) : hit.second;
        const qreal x = textRect.left() + fm.horizontalAdvance(shown.left(start));
        const qreal w = fm.horizontalAdvance(shown.mid(start, length));
        painter->setPen(Qt::NoPen);
        painter->setBrush(pal.searchTint(rowBg));
        painter->drawRoundedRect(QRectF(x - 2, row.top() + 3, w + 4, row.height() - 6), 3, 3);
        const QString before = shown.left(start);
        const QString match = shown.mid(start, length);
        painter->setPen(nameColor);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, before);
        painter->setPen(pal.yellow);
        painter->drawText(textRect.adjusted(x - textRect.left(), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, match);
        painter->setPen(nameColor);
        painter->drawText(textRect.adjusted(x + w - textRect.left(), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          shown.mid(start + length));
    } else {
        painter->setPen(nameColor);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, shown);
    }
    if (!right.isEmpty()) {
        painter->setPen(pal.subtext);
        painter->drawText(QRectF(row.right() - rightWidth - 4, row.top(), rightWidth, row.height()),
                          Qt::AlignVCenter | Qt::AlignRight, right);
    }
    painter->restore();
}

QSize EntryDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const {
    return QSize(option.rect.width(), Theme::instance().rowHeight());
}

ColumnView::ColumnView(DirModel *model, Role role, QWidget *parent)
    : QListView(parent), m_model(model), m_role(role) {
    setModel(model);
    setItemDelegate(new EntryDelegate(this));
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setMouseTracking(false);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setDropIndicatorShown(false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAutoFillBackground(true);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex idx = indexAt(pos);
        emit contextMenuRequested(idx.isValid() ? idx.row() : -1, viewport()->mapToGlobal(pos));
    });
    verticalScrollBar()->setSingleStep(Theme::instance().rowHeight());
}

void ColumnView::setShowCursor(bool show) {
    if (show == m_showCursor)
        return;
    m_showCursor = show;
    viewport()->update();
}

void ColumnView::setCurrentRow(int row) {
    if (row < 0 || row >= m_model->rowCount()) {
        selectionModel()->clearCurrentIndex();
        return;
    }
    const QModelIndex idx = m_model->index(row);
    selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
    // Keep a little context above and below the cursor, ranger style.
    const int h = Theme::instance().rowHeight();
    const QRect r = visualRect(idx);
    const int margin = std::min(3 * h, viewport()->height() / 4);
    if (r.top() < margin || r.bottom() > viewport()->height() - margin) {
        int target = verticalScrollBar()->value();
        if (r.top() < margin)
            target += r.top() - margin;
        else
            target += r.bottom() - (viewport()->height() - margin);
        verticalScrollBar()->setValue(target);
    }
}

void ColumnView::setHighlight(const NameMatcher &matcher) {
    m_highlight = matcher;
    viewport()->update();
}

int ColumnView::visibleRows() const { return std::max(1, viewport()->height() / Theme::instance().rowHeight()); }

void ColumnView::startDrag(Qt::DropActions) {
    const FileEntry *e = m_model->entry(m_pressedRow >= 0 ? m_pressedRow : currentRow());
    if (!e)
        return;
    QStringList paths;
    Marks *marks = m_model->marks();
    if (marks && marks->contains(e->path))
        paths = marks->paths();
    else
        paths = {e->path};
    startFileDrag(this, paths);
    m_pressedRow = -1;
}

Qt::DropAction ColumnView::chooseDropAction(const QDropEvent *event) const {
    const auto mods = event->modifiers();
    const Qt::DropActions possible = event->possibleActions();
    auto allowed = [&](Qt::DropAction a) { return possible.testFlag(a) ? a : event->proposedAction(); };
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier))
        return allowed(Qt::LinkAction);
    if (mods & Qt::ShiftModifier)
        return allowed(Qt::MoveAction);
    if (mods & Qt::ControlModifier)
        return allowed(Qt::CopyAction);
    if (qobject_cast<const ColumnView *>(event->source()))
        return allowed(Qt::MoveAction);
    return allowed(Qt::CopyAction);
}

QString ColumnView::dropTargetDir(const QPoint &pos, int *row) const {
    const QModelIndex idx = indexAt(pos);
    const FileEntry *e = idx.isValid() ? m_model->entry(idx.row()) : nullptr;
    if (e && e->isDir) {
        *row = idx.row();
        return e->path;
    }
    *row = -1;
    return m_model->path();
}

void ColumnView::dragEnterEvent(QDragEnterEvent *event) {
    if (!event->mimeData()->hasUrls() || m_model->path().isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(chooseDropAction(event));
    event->accept();
}

void ColumnView::dragMoveEvent(QDragMoveEvent *event) {
    if (!event->mimeData()->hasUrls() || m_model->path().isEmpty()) {
        event->ignore();
        return;
    }
    int row = -1;
    const QString target = dropTargetDir(event->position().toPoint(), &row);
    const QList<QUrl> urls = event->mimeData()->urls();
    const bool self = std::any_of(urls.begin(), urls.end(), [&](const QUrl &u) { return u.toLocalFile() == target; });
    m_model->setDropTarget(self ? -1 : row);
    event->setDropAction(chooseDropAction(event));
    event->accept();
}

void ColumnView::dragLeaveEvent(QDragLeaveEvent *event) {
    m_model->setDropTarget(-1);
    QListView::dragLeaveEvent(event);
}

void ColumnView::dropEvent(QDropEvent *event) {
    m_model->setDropTarget(-1);
    int row = -1;
    const QString target = dropTargetDir(event->position().toPoint(), &row);
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (paths.isEmpty() || target.isEmpty()) {
        event->ignore();
        return;
    }
    const Qt::DropAction action = chooseDropAction(event);
    event->setDropAction(action);
    event->accept();
    emit dropRequested(paths, target, action);
}

void ColumnView::mousePressEvent(QMouseEvent *event) {
    const QModelIndex idx = indexAt(event->position().toPoint());
    m_pressedRow = idx.isValid() ? idx.row() : -1;
    if (event->button() == Qt::LeftButton && idx.isValid() && m_role == Role::Current) {
        if (event->modifiers() & Qt::ControlModifier) {
            emit markToggleRequested(idx.row());
            return;
        }
        if (event->modifiers() & Qt::ShiftModifier) {
            emit markRangeRequested(idx.row());
            return;
        }
    }
    QListView::mousePressEvent(event);
}

void ColumnView::mouseReleaseEvent(QMouseEvent *event) {
    const QModelIndex idx = indexAt(event->position().toPoint());
    const bool clicked = event->button() == Qt::LeftButton && idx.isValid() && idx.row() == m_pressedRow &&
                         !(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
    QListView::mouseReleaseEvent(event);
    if (clicked)
        emit rowClicked(idx.row());
    m_pressedRow = -1;
}

void ColumnView::mouseDoubleClickEvent(QMouseEvent *event) {
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && idx.isValid()) {
        emit rowActivated(idx.row());
        return;
    }
    QListView::mouseDoubleClickEvent(event);
}

void ColumnView::paintEvent(QPaintEvent *event) {
    QListView::paintEvent(event);
    if (m_model->rowCount() > 0 || m_model->path().isEmpty() || (!m_model->isLoaded() && m_model->isLoading()))
        return;
    QPainter p(viewport());
    p.setPen(m_model->error().isEmpty() ? Theme::p().subtext : Theme::p().red);
    QString message = m_model->error();
    if (message.isEmpty())
        message = m_model->filter().isEmpty() ? (m_model->totalCount() > 0 ? QStringLiteral("only hidden files")
                                                                            : QStringLiteral("empty"))
                                              : QStringLiteral("no matches");
    p.drawText(viewport()->rect().adjusted(12, 8, -12, -8), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, message);
}
