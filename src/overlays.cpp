#include "overlays.h"

#include "columnview.h"
#include "fileops.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QPainter>

namespace {

QString tildePath(const QString &path) {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + u'/'))
        return u'~' + path.mid(home.size());
    return path;
}

QColor levelColor(StatusBar::Level level) {
    const Palette &p = Theme::p();
    switch (level) {
    case StatusBar::Level::Success: return p.green;
    case StatusBar::Level::Warning: return p.yellow;
    case StatusBar::Level::Error: return p.red;
    case StatusBar::Level::Info: break;
    }
    return p.text;
}

} // namespace

HeaderBar::HeaderBar(QWidget *parent) : QWidget(parent) { setFocusPolicy(Qt::NoFocus); }

void HeaderBar::setPath(const QString &dir, const QString &name) {
    m_dir = dir;
    m_name = name;
    update();
}

void HeaderBar::setTabs(const QStringList &labels, int active) {
    m_tabs = labels;
    m_activeTab = active;
    update();
}

QSize HeaderBar::sizeHint() const { return QSize(200, Theme::instance().rowHeight() + 6); }

void HeaderBar::mousePressEvent(QMouseEvent *event) {
    for (int i = 0; i < m_tabRects.size(); ++i)
        if (m_tabRects[i].contains(event->position().toPoint()))
            emit tabClicked(i);
}

void HeaderBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.crust);
    QFont font = Theme::instance().font();
    font.setBold(true);
    p.setFont(font);
    const QFontMetrics fm(font);

    // Tabs are laid out right to left so the path gets whatever is left.
    m_tabRects.clear();
    int tabsLeft = width() - 6;
    if (m_tabs.size() > 1) {
        QFont tabFont = Theme::instance().font();
        const QFontMetrics tfm(tabFont);
        QList<QRect> rects;
        for (int i = int(m_tabs.size()) - 1; i >= 0; --i) {
            const QString label = QStringLiteral("%1 %2").arg(i + 1).arg(tfm.elidedText(m_tabs[i], Qt::ElideMiddle, 140));
            const int w = tfm.horizontalAdvance(label) + 16;
            tabsLeft -= w + 4;
            rects.prepend(QRect(tabsLeft, 3, w, height() - 6));
        }
        p.setFont(tabFont);
        for (int i = 0; i < rects.size(); ++i) {
            const bool active = i == m_activeTab;
            p.setPen(Qt::NoPen);
            p.setBrush(active ? pal.accent : pal.surface);
            p.drawRoundedRect(rects[i], 4, 4);
            p.setPen(active ? pal.crust : pal.subtext);
            const QString label = QStringLiteral("%1 %2").arg(i + 1).arg(tfm.elidedText(m_tabs[i], Qt::ElideMiddle, 140));
            p.drawText(rects[i], Qt::AlignCenter, label);
        }
        m_tabRects = rects;
        p.setFont(font);
        tabsLeft -= 8;
    }

    QString dir = tildePath(m_dir);
    if (!dir.endsWith(u'/'))
        dir += u'/';
    const int nameWidth = fm.horizontalAdvance(m_name);
    const int available = std::max(60, tabsLeft - 10);
    const QString shownDir = fm.elidedText(dir, Qt::ElideLeft, std::max(40, available - nameWidth));
    p.setPen(pal.accent);
    p.drawText(QRect(10, 0, available, height()), Qt::AlignVCenter | Qt::AlignLeft, shownDir);
    p.setPen(pal.text);
    const int x = 10 + fm.horizontalAdvance(shownDir);
    p.drawText(QRect(x, 0, available + 10 - x, height()), Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(m_name, Qt::ElideMiddle, std::max(0, available + 10 - x)));
}

StatusBar::StatusBar(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        update();
    });
}

QSize StatusBar::sizeHint() const { return QSize(200, Theme::instance().rowHeight() + 6); }

void StatusBar::setLeft(const QStringList &segments) {
    m_left = segments;
    update();
}

void StatusBar::setRight(const QStringList &segments) {
    m_right = segments;
    update();
}

void StatusBar::showMessage(const QString &message, Level level, int timeoutMs) {
    m_message = message;
    m_level = level;
    if (timeoutMs > 0)
        m_messageTimer.start(timeoutMs);
    else
        m_messageTimer.stop();
    update();
}

void StatusBar::setDragItems(int count, std::function<QStringList()> provider) {
    m_dragCount = count;
    m_dragProvider = std::move(provider);
    update();
}

QRect StatusBar::dragChipRect() const {
    if (m_dragCount <= 0)
        return {};
    const QFontMetrics fm(Theme::instance().font());
    const QString label = QStringLiteral("⠿ drag %1").arg(m_dragCount);
    const int w = fm.horizontalAdvance(label) + 16;
    int rightWidth = 0;
    for (const QString &s : m_right)
        rightWidth += fm.horizontalAdvance(s) + 18;
    return QRect(width() - rightWidth - w - 14, 2, w, height() - 4);
}

void StatusBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.crust);
    const QFont font = Theme::instance().font();
    p.setFont(font);
    const QFontMetrics fm(font);

    int x = 10;
    p.setPen(pal.muted);
    for (const QString &s : m_left) {
        p.drawText(QRect(x, 0, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, s);
        x += fm.horizontalAdvance(s) + 18;
    }

    int right = width() - 10;
    for (int i = int(m_right.size()) - 1; i >= 0; --i) {
        const QString &s = m_right[i];
        const int w = fm.horizontalAdvance(s);
        p.setPen(i == m_right.size() - 1 ? pal.subtext : pal.yellow);
        p.drawText(QRect(right - w, 0, w, height()), Qt::AlignVCenter | Qt::AlignRight, s);
        right -= w + 18;
    }

    const QRect chip = dragChipRect();
    if (chip.isValid()) {
        p.setPen(QPen(pal.yellow, 1));
        QColor fill = pal.yellow;
        fill.setAlphaF(0.12);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p.setPen(pal.yellow);
        p.drawText(chip, Qt::AlignCenter, QStringLiteral("⠿ drag %1").arg(m_dragCount));
        right = chip.left() - 12;
    }

    if (!m_message.isEmpty()) {
        p.setPen(levelColor(m_level));
        const int available = right - x - 10;
        if (available > 40)
            p.drawText(QRect(x + 6, 0, available, height()), Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(m_message, Qt::ElideRight, available));
    }
}

void StatusBar::mousePressEvent(QMouseEvent *event) {
    m_pressedChip = event->button() == Qt::LeftButton && dragChipRect().contains(event->position().toPoint());
    m_pressPos = event->position().toPoint();
}

void StatusBar::mouseMoveEvent(QMouseEvent *event) {
    if (!m_pressedChip ||
        (event->position().toPoint() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
        return;
    m_pressedChip = false;
    if (m_dragProvider)
        startFileDrag(this, m_dragProvider());
}

void StatusBar::mouseReleaseEvent(QMouseEvent *) { m_pressedChip = false; }

Card::Card(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    hide();
}

int Card::lineHeight() const { return Theme::instance().rowHeight(); }

void Card::paintFrame(QPainter &p, const QString &title, const QString &footer) {
    const Palette &pal = Theme::p();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(pal.overlay, 1));
    p.setBrush(pal.mantle);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setFont(Theme::instance().font());
    if (!title.isEmpty()) {
        QFont bold = Theme::instance().font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(pal.accent);
        p.drawText(QRect(14, 6, width() - 28, lineHeight()), Qt::AlignVCenter, title);
        p.setFont(Theme::instance().font());
    }
    if (!footer.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(QRect(14, height() - lineHeight() - 6, width() - 28, lineHeight()), Qt::AlignVCenter, footer);
    }
}

void WhichKey::setContinuations(const QString &prefix, const QList<Continuation> &items) {
    m_prefix = prefix;
    m_items = items;
    update();
}

int WhichKey::columnWidth() const {
    const QFontMetrics fm(Theme::instance().font());
    int w = 0;
    for (const Continuation &c : m_items)
        w = std::max(w, fm.horizontalAdvance(c.key) + fm.horizontalAdvance(c.label) + 28);
    return std::min(w, 360);
}

int WhichKey::columns(int width) const {
    return std::clamp((width - 28) / std::max(1, columnWidth()), 1, 4);
}

QSize WhichKey::preferredSize(int maxWidth) const {
    const int cols = std::min(columns(maxWidth), std::max(1, int(m_items.size())));
    const int rows = (int(m_items.size()) + cols - 1) / cols;
    return QSize(cols * columnWidth() + 28, (rows + 1) * lineHeight() + 22);
}

void WhichKey::paintEvent(QPaintEvent *) {
    QPainter p(this);
    paintFrame(p, m_prefix, {});
    const Palette &pal = Theme::p();
    const QFontMetrics fm(Theme::instance().font());
    const int cols = columns(width());
    const int rows = (int(m_items.size()) + cols - 1) / cols;
    const int cw = columnWidth();
    int keyWidth = 0;
    for (const Continuation &c : m_items)
        keyWidth = std::max(keyWidth, fm.horizontalAdvance(c.key));
    for (int i = 0; i < m_items.size(); ++i) {
        const Continuation &c = m_items[i];
        const int col = i / std::max(1, rows);
        const int row = i % std::max(1, rows);
        const int x = 14 + col * cw;
        const int y = 8 + (row + 1) * lineHeight();
        p.setPen(pal.yellow);
        p.drawText(QRect(x, y, keyWidth, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft, c.key);
        p.setPen(c.isPrefix ? pal.magenta : pal.text);
        p.drawText(QRect(x + keyWidth + 10, y, cw - keyWidth - 16, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(c.label, Qt::ElideRight, cw - keyWidth - 16));
    }
}

TasksView::TasksView(TaskManager *tasks, QWidget *parent) : Card(parent), m_tasks(tasks) {
    connect(tasks, &TaskManager::tasksChanged, this, [this] {
        if (isVisible())
            update();
    });
}

void TasksView::moveSelection(int delta) {
    const int n = int(m_tasks->tasks().size());
    if (n == 0)
        return;
    m_selected = std::clamp(m_selected + delta, 0, n - 1);
    update();
}

Task *TasksView::selectedTask() const {
    const auto tasks = m_tasks->tasks();
    if (tasks.isEmpty())
        return nullptr;
    return tasks.value(std::clamp(m_selected, 0, int(tasks.size()) - 1));
}

QSize TasksView::preferredSize(int maxWidth, int maxHeight) const {
    const int rows = std::max(1, int(m_tasks->tasks().size()));
    return QSize(std::min(maxWidth, 640), std::min(maxHeight, (rows * 2 + 2) * lineHeight() + 30));
}

void TasksView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    paintFrame(p, QStringLiteral("Tasks"), QStringLiteral("j/k move · x cancel · c clear finished · Esc close"));
    const Palette &pal = Theme::p();
    const QFontMetrics fm(Theme::instance().font());
    const auto tasks = m_tasks->tasks();
    int y = 10 + lineHeight();
    if (tasks.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(QRect(14, y, width() - 28, lineHeight()), Qt::AlignVCenter, QStringLiteral("no tasks"));
        return;
    }
    const int selected = std::clamp(m_selected, 0, int(tasks.size()) - 1);
    for (int i = 0; i < tasks.size(); ++i) {
        Task *t = tasks[i];
        const Task::Progress prog = t->progress();
        const QRect row(8, y, width() - 16, lineHeight() * 2);
        if (i == selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.surface);
            p.drawRoundedRect(QRectF(row), 4, 4);
        }
        QString state;
        QColor stateColor = pal.subtext;
        switch (prog.state) {
        case Task::Running:
            state = QStringLiteral("%1%").arg(qRound(t->fraction() * 100));
            break;
        case Task::Done:
            state = QStringLiteral("done");
            stateColor = pal.green;
            break;
        case Task::Failed:
            state = QStringLiteral("%1 error%2").arg(prog.errors.size()).arg(prog.errors.size() == 1 ? "" : "s");
            stateColor = pal.red;
            break;
        case Task::Cancelled:
            state = QStringLiteral("cancelled");
            stateColor = pal.yellow;
            break;
        }
        p.setPen(pal.text);
        p.drawText(QRect(row.left() + 8, row.top(), row.width() - 120, lineHeight()), Qt::AlignVCenter,
                   fm.elidedText(t->title(), Qt::ElideMiddle, row.width() - 120));
        p.setPen(stateColor);
        p.drawText(QRect(row.right() - 110, row.top(), 102, lineHeight()), Qt::AlignVCenter | Qt::AlignRight, state);

        const QRectF bar(row.left() + 8, row.top() + lineHeight() + 4, row.width() - 16, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.overlay);
        p.drawRoundedRect(bar, 2, 2);
        p.setBrush(prog.state == Task::Failed ? pal.red : pal.accent);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * t->fraction(), bar.height()), 2, 2);
        QString detail = prog.state == Task::Failed ? prog.errors.value(0) : prog.current;
        if (prog.state == Task::Running && prog.totalBytes > 0)
            detail = QStringLiteral("%1 of %2  %3").arg(humanSize(prog.doneBytes), humanSize(prog.totalBytes), detail);
        p.setPen(pal.muted);
        p.drawText(QRect(row.left() + 8, row.top() + lineHeight() + 8, row.width() - 16, lineHeight() - 8),
                   Qt::AlignVCenter, fm.elidedText(detail, Qt::ElideMiddle, row.width() - 16));
        y += lineHeight() * 2 + 4;
        if (y > height() - lineHeight() * 2)
            break;
    }
}

void PropertiesView::setRows(const QString &title, const QList<QPair<QString, QString>> &rows) {
    m_title = title;
    m_rows = rows;
    update();
}

void PropertiesView::updateRow(const QString &key, const QString &value) {
    for (auto &row : m_rows)
        if (row.first == key)
            row.second = value;
    update();
}

QSize PropertiesView::preferredSize(int maxWidth) const {
    return QSize(std::min(maxWidth, 620), (int(m_rows.size()) + 2) * lineHeight() + 30);
}

void PropertiesView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    paintFrame(p, m_title, QStringLiteral("Esc close"));
    const Palette &pal = Theme::p();
    const QFontMetrics fm(Theme::instance().font());
    int keyWidth = 0;
    for (const auto &row : m_rows)
        keyWidth = std::max(keyWidth, fm.horizontalAdvance(row.first));
    int y = 12 + lineHeight();
    for (const auto &row : m_rows) {
        p.setPen(pal.muted);
        p.drawText(QRect(14, y, keyWidth, lineHeight()), Qt::AlignVCenter | Qt::AlignRight, row.first);
        p.setPen(pal.text);
        const int x = 14 + keyWidth + 14;
        p.drawText(QRect(x, y, width() - x - 14, lineHeight()), Qt::AlignVCenter,
                   fm.elidedText(row.second, Qt::ElideMiddle, width() - x - 14));
        y += lineHeight();
    }
}
