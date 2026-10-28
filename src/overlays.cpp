#include "overlays.h"

#include "columnview.h"
#include "fileops.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

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
    const Palette &pal = Theme::p();
    switch (level) {
    case StatusBar::Level::Success: return pal.green;
    case StatusBar::Level::Warning: return pal.yellow;
    case StatusBar::Level::Error: return pal.red;
    case StatusBar::Level::Info: break;
    }
    return pal.text;
}

QFont boldFont() {
    QFont f = Theme::instance().font();
    f.setBold(true);
    return f;
}

const QString kDot = QStringLiteral(" · ");

} // namespace

QMargins card::shadowMargins() { return {14, 8, 14, 18}; }
int card::radius() { return 10; }
int card::padding() { return 14; }
int card::rowHeight() { return Theme::instance().rowHeight() + 2; }
int card::headerHeight() { return Theme::instance().rowHeight() + 12; }
int card::footerHeight() { return Theme::instance().rowHeight() + 10; }
QRect card::body(const QRect &widgetRect) { return widgetRect.marginsRemoved(shadowMargins()); }
QRect card::around(const QRect &body) { return body.marginsAdded(shadowMargins()); }

void card::paintFrame(QPainter &p, const QRect &body) {
    const Palette &pal = Theme::p();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    // Stacked translucent outlines: darkest at the edge, fading outward.
    constexpr int layers = 10;
    QColor shade(0, 0, 0);
    shade.setAlphaF(pal.dark ? 0.05 : 0.012);
    p.setBrush(shade);
    for (int s = layers; s >= 1; --s)
        p.drawRoundedRect(QRectF(body).adjusted(-s, -s + 4, s, s + 4), radius() + s, radius() + s);
    p.setPen(QPen(pal.overlay, 1));
    p.setBrush(pal.mantle);
    p.drawRoundedRect(QRectF(body).adjusted(0.5, 0.5, -0.5, -0.5), radius(), radius());
    p.restore();
}

void card::paintHeader(QPainter &p, const QRect &row, const QString &title, const QString &meta, const QString &key) {
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.save();
    p.setFont(font);
    const int natural = fm.horizontalAdvance(meta);
    const int metaWidth = meta.isEmpty() ? 0 : std::min(natural, row.width() / 2);
    if (!meta.isEmpty()) {
        p.setPen(pal.subtext);
        p.drawText(QRect(row.right() - metaWidth - 2, row.top(), metaWidth + 3, row.height()), Qt::AlignVCenter | Qt::AlignRight,
                   natural <= metaWidth ? meta : fm.elidedText(meta, Qt::ElideLeft, metaWidth));
    }
    int x = row.left();
    const int limit = row.right() - metaWidth - (meta.isEmpty() ? 0 : 16);
    if (!key.isEmpty()) {
        p.setPen(pal.yellow);
        p.drawText(QRect(x, row.top(), limit - x, row.height()), Qt::AlignVCenter | Qt::AlignLeft, key);
        x += fm.horizontalAdvance(key);
        if (!title.isEmpty()) {
            p.setPen(pal.muted);
            p.drawText(QRect(x, row.top(), std::max(0, limit - x), row.height()), Qt::AlignVCenter | Qt::AlignLeft, kDot);
            x += fm.horizontalAdvance(kDot);
        }
    }
    p.setPen(pal.text);
    p.drawText(QRect(x, row.top(), std::max(0, limit - x), row.height()), Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(title, Qt::ElideMiddle, std::max(0, limit - x)));
    p.restore();
}

void card::paintRule(QPainter &p, int left, int right, int y) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(Theme::p().overlay, 1));
    p.drawLine(left, y, right, y);
    p.restore();
}

void card::paintSelection(QPainter &p, const QRectF &row) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::p().surface);
    p.drawRoundedRect(row, 6, 6);
    p.restore();
}

int card::hintWidth(const QString &hint) {
    return QFontMetrics(Theme::instance().font()).horizontalAdvance(hint);
}

void card::paintHint(QPainter &p, const QRect &row, const QString &hint, Qt::Alignment align) {
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.save();
    p.setFont(font);
    int x = row.left();
    if (align & Qt::AlignRight)
        x = row.right() - hintWidth(hint);
    else if (align & Qt::AlignHCenter)
        x = row.left() + (row.width() - hintWidth(hint)) / 2;
    auto draw = [&](const QString &text, const QColor &color) {
        p.setPen(color);
        p.drawText(QRect(x, row.top(), fm.horizontalAdvance(text) + 1, row.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
        x += fm.horizontalAdvance(text);
    };
    const QStringList parts = hint.split(kDot);
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0)
            draw(kDot, pal.muted);
        const int space = int(parts[i].indexOf(u' '));
        draw(space < 0 ? parts[i] : parts[i].left(space), pal.subtext);
        if (space >= 0)
            draw(parts[i].mid(space), pal.muted);
    }
    p.restore();
}

QString card::keyLabel(const QStringList &keys) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("CR"), QStringLiteral("Enter")},   {QStringLiteral("BS"), QStringLiteral("Backspace")},
        {QStringLiteral("Up"), QStringLiteral("↑")},       {QStringLiteral("Down"), QStringLiteral("↓")},
        {QStringLiteral("Left"), QStringLiteral("←")},     {QStringLiteral("Right"), QStringLiteral("→")},
        {QStringLiteral("PageUp"), QStringLiteral("PgUp")}, {QStringLiteral("PageDown"), QStringLiteral("PgDn")},
    };
    static const QHash<QString, QString> mods = {
        {QStringLiteral("C"), QStringLiteral("Ctrl")}, {QStringLiteral("A"), QStringLiteral("Alt")},
        {QStringLiteral("S"), QStringLiteral("Shift")}};
    QString out;
    bool lastNamed = false;
    for (const QString &k : keys) {
        QString piece = k;
        if (k.size() > 2 && k.startsWith(u'<') && k.endsWith(u'>')) {
            QStringList parts = k.mid(1, k.size() - 2).split(u'-');
            // "<C-->" splits into an empty key; put the dash back.
            if (parts.size() > 1 && parts.last().isEmpty()) {
                parts.removeLast();
                parts.last() = QStringLiteral("-");
            }
            QString key = parts.takeLast();
            key = names.value(key, key);
            QStringList shown;
            for (const QString &m : parts)
                shown << mods.value(m, m);
            shown << key;
            piece = shown.join(u'-');
        }
        const bool named = piece.size() > 1;
        if (!out.isEmpty() && (named || lastNamed))
            out += u' ';
        out += piece;
        lastNamed = named;
    }
    return out;
}

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

QSize HeaderBar::sizeHint() const { return QSize(200, Theme::instance().rowHeight() + 8); }

QList<QRect> HeaderBar::tabRects() const {
    const QFontMetrics fm(Theme::instance().font());
    const QFontMetrics bfm(boldFont());
    QList<QRect> rects;
    int right = width() - 8;
    for (int i = int(m_tabs.size()) - 1; i >= 0; --i) {
        const QString name = fm.elidedText(m_tabs[i], Qt::ElideMiddle, 140);
        const int w = fm.horizontalAdvance(QStringLiteral("%1 ").arg(i + 1)) +
                      (i == m_activeTab ? bfm : fm).horizontalAdvance(name) + 20;
        rects.prepend(QRect(right - w, 0, w, height()));
        right -= w + 2;
    }
    return rects;
}

void HeaderBar::mousePressEvent(QMouseEvent *event) {
    const QList<QRect> rects = tabRects();
    for (int i = 0; i < rects.size(); ++i)
        if (rects[i].contains(event->position().toPoint()))
            emit tabClicked(i);
}

void HeaderBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    card::paintRule(p, 0, width(), height() - 1);
    const QFont font = Theme::instance().font();
    const QFont bold = boldFont();
    const QFontMetrics fm(font);
    const QFontMetrics bfm(bold);

    const QList<QRect> rects = tabRects();
    for (int i = 0; i < rects.size(); ++i) {
        const bool active = i == m_activeTab;
        const QRect r = rects[i].adjusted(10, 0, -10, -1);
        const QString number = QStringLiteral("%1 ").arg(i + 1);
        p.setFont(font);
        p.setPen(pal.muted);
        p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, number);
        p.setFont(active ? bold : font);
        p.setPen(active ? pal.text : pal.subtext);
        p.drawText(r.adjusted(fm.horizontalAdvance(number), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(m_tabs[i], Qt::ElideMiddle, 140));
        if (active)
            p.fillRect(QRect(r.left(), height() - 2, r.width(), 2), pal.accent);
    }
    const int tabsLeft = rects.isEmpty() ? width() : rects.first().left();

    QString dir = tildePath(m_dir);
    if (!dir.endsWith(u'/'))
        dir += u'/';
    const int nameWidth = bfm.horizontalAdvance(m_name);
    const int available = std::max(60, tabsLeft - 24);
    const QString shownDir = fm.elidedText(dir, Qt::ElideLeft, std::max(40, available - nameWidth));
    p.setFont(font);
    p.setPen(pal.accent);
    p.drawText(QRect(12, 0, available, height() - 1), Qt::AlignVCenter | Qt::AlignLeft, shownDir);
    p.setFont(bold);
    p.setPen(pal.text);
    const int x = 12 + fm.horizontalAdvance(shownDir);
    p.drawText(QRect(x, 0, std::max(0, available + 12 - x), height() - 1), Qt::AlignVCenter | Qt::AlignLeft,
               bfm.elidedText(m_name, Qt::ElideMiddle, std::max(0, available + 12 - x)));
}

StatusBar::StatusBar(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        update();
    });
}

QSize StatusBar::sizeHint() const { return QSize(200, Theme::instance().rowHeight() + 8); }

void StatusBar::setLeft(const QStringList &segments) {
    m_left = segments;
    update();
}

void StatusBar::setRight(const QList<Segment> &segments) {
    m_right = segments;
    update();
}

void StatusBar::setMode(const QString &mode) {
    if (mode == m_mode)
        return;
    m_mode = mode;
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

int StatusBar::rightWidth() const {
    const QFontMetrics fm(Theme::instance().font());
    int w = 0;
    for (const Segment &s : m_right)
        w += fm.horizontalAdvance(s.text) + 16;
    if (!m_mode.isEmpty())
        w += QFontMetrics(boldFont()).horizontalAdvance(m_mode) + 16 + 12;
    return w;
}

QRect StatusBar::dragChipRect() const {
    if (m_dragCount <= 0)
        return {};
    const QFontMetrics fm(Theme::instance().font());
    const QString label = QStringLiteral("⠿ drag %1").arg(m_dragCount);
    const int w = fm.horizontalAdvance(label) + 16;
    return QRect(width() - rightWidth() - w - 10, 4, w, height() - 7);
}

void StatusBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    card::paintRule(p, 0, width(), 0);
    const QFont font = Theme::instance().font();
    p.setFont(font);
    const QFontMetrics fm(font);
    const QRect line(0, 1, width(), height() - 1);

    int x = 12;
    for (int i = 0; i < m_left.size(); ++i) {
        if (i > 0) {
            p.setPen(pal.muted);
            p.drawText(QRect(x, line.top(), width(), line.height()), Qt::AlignVCenter | Qt::AlignLeft, kDot);
            x += fm.horizontalAdvance(kDot);
        }
        p.setPen(pal.subtext);
        p.drawText(QRect(x, line.top(), width(), line.height()), Qt::AlignVCenter | Qt::AlignLeft, m_left[i]);
        x += fm.horizontalAdvance(m_left[i]);
    }

    int right = width() - 12;
    for (int i = int(m_right.size()) - 1; i >= 0; --i) {
        const Segment &s = m_right[i];
        const int w = fm.horizontalAdvance(s.text);
        p.setPen(s.color.isValid() ? s.color : pal.subtext);
        p.drawText(QRect(right - w, line.top(), w + 1, line.height()), Qt::AlignVCenter | Qt::AlignRight, s.text);
        right -= w + 16;
    }
    if (!m_mode.isEmpty()) {
        const QFont bold = boldFont();
        const int w = QFontMetrics(bold).horizontalAdvance(m_mode) + 16;
        const QRect badge(right - w, 4, w, height() - 7);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.badgeTint(pal.base, pal.magenta));
        p.drawRoundedRect(QRectF(badge), 5, 5);
        p.setFont(bold);
        p.setPen(pal.magenta);
        p.drawText(badge, Qt::AlignCenter, m_mode);
        p.setFont(font);
        right = badge.left() - 12;
    }

    const QRect chip = dragChipRect();
    if (chip.isValid()) {
        p.setPen(QPen(pal.yellow, 1));
        p.setBrush(pal.searchTint(pal.base));
        p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        p.setPen(pal.yellow);
        p.drawText(chip, Qt::AlignCenter, QStringLiteral("⠿ drag %1").arg(m_dragCount));
        right = chip.left() - 12;
    }

    if (!m_message.isEmpty()) {
        p.setPen(levelColor(m_level));
        const int start = x + (m_left.isEmpty() ? 0 : 24);
        const int available = right - start;
        if (available > 40)
            p.drawText(QRect(start, line.top(), available, line.height()), Qt::AlignVCenter | Qt::AlignLeft,
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

int Card::lineHeight() const { return card::rowHeight(); }

Scrim::Scrim(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void Scrim::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QColor dim(0, 0, 0);
    dim.setAlphaF(Theme::p().dark ? 0.32 : 0.12);
    p.fillRect(rect(), dim);
}

void Scrim::mousePressEvent(QMouseEvent *) { emit clicked(); }

void WhichKey::setContinuations(const QString &prefix, const QString &family, const QList<Continuation> &items) {
    m_prefix = prefix;
    m_family = family;
    m_items = items;
    update();
}

WhichKey::Layout WhichKey::layout(const QSize &body) const {
    Layout l;
    const QFontMetrics fm(Theme::instance().font());
    const int n = int(m_items.size());
    int labelWidth = 0;
    for (const Continuation &c : m_items) {
        l.keyWidth = std::max(l.keyWidth, fm.horizontalAdvance(card::keyLabel({c.key})));
        labelWidth = std::max(labelWidth, fm.horizontalAdvance(c.label));
    }
    const int natural = l.keyWidth + 14 + std::min(labelWidth, 300);
    const int inner = body.width() - 2 * card::padding();
    constexpr int gap = 28;
    l.columns = n > 6 && inner >= 2 * natural + gap ? 2 : 1;
    l.columnWidth = std::max(1, std::min(natural, l.columns == 2 ? (inner - gap) / 2 : inner));
    const int maxRows = std::max(1, (body.height() - card::headerHeight() - 8) / lineHeight());
    l.rows = std::min((n + l.columns - 1) / l.columns, maxRows);
    const int capacity = l.columns * l.rows;
    // The last slot turns into the "+N more" line.
    l.hidden = n > capacity ? n - (capacity - 1) : 0;
    return l;
}

QSize WhichKey::preferredSize(int maxWidth, int maxHeight) const {
    const QMargins m = card::shadowMargins();
    const QSize bounds(maxWidth - m.left() - m.right(), maxHeight - m.top() - m.bottom());
    const Layout l = layout(bounds);
    const QFontMetrics fm(Theme::instance().font());
    const int headerWidth = fm.horizontalAdvance(m_prefix + kDot + m_family);
    const int width = std::max(l.columns * l.columnWidth + (l.columns - 1) * 28, headerWidth) + 2 * card::padding();
    const int height = card::headerHeight() + l.rows * lineHeight() + 8;
    return card::around(QRect(0, 0, std::min(bounds.width(), width), std::min(bounds.height(), height))).size();
}

void WhichKey::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QRect body = bodyRect();
    card::paintFrame(p, body);
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.setFont(font);
    const int pad = card::padding();
    card::paintHeader(p, QRect(body.left() + pad, body.top() + 2, body.width() - 2 * pad, card::headerHeight() - 4),
                      m_family, {}, m_prefix);
    const Layout l = layout(body.size());
    const int shown = int(m_items.size()) - l.hidden;
    for (int i = 0; i < shown + (l.hidden > 0 ? 1 : 0); ++i) {
        const int col = i / std::max(1, l.rows);
        const int row = i % std::max(1, l.rows);
        const int x = body.left() + pad + col * (l.columnWidth + 28);
        const int y = body.top() + card::headerHeight() + row * lineHeight();
        if (i == shown) {
            p.setPen(pal.subtext);
            p.drawText(QRect(x, y, l.columnWidth, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft,
                       QStringLiteral("+%1 more").arg(l.hidden));
            break;
        }
        const Continuation &c = m_items[i];
        p.setPen(pal.yellow);
        p.drawText(QRect(x, y, l.keyWidth, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft, card::keyLabel({c.key}));
        p.setPen(c.isPrefix ? pal.subtext : pal.text);
        const int labelWidth = std::max(0, l.columnWidth - l.keyWidth - 14);
        p.drawText(QRect(x + l.keyWidth + 14, y, labelWidth, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(c.label, Qt::ElideRight, labelWidth));
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
    const QMargins m = card::shadowMargins();
    const int rows = std::max(1, int(m_tasks->tasks().size()));
    const int body = card::headerHeight() + rows * (lineHeight() * 2 + 6) + 10 + card::footerHeight();
    return card::around(QRect(0, 0, std::min(maxWidth - m.left() - m.right(), 640),
                              std::min(maxHeight - m.top() - m.bottom(), body)))
        .size();
}

void TasksView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect body = bodyRect();
    card::paintFrame(p, body);
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.setFont(font);
    const int pad = card::padding();
    const auto tasks = m_tasks->tasks();
    const int running = m_tasks->activeCount();
    card::paintHeader(p, QRect(body.left() + pad, body.top() + 2, body.width() - 2 * pad, card::headerHeight() - 4),
                      QStringLiteral("Tasks"), running > 0 ? QStringLiteral("%1 running").arg(running) : QString());
    const QRect footer(body.left() + pad, body.bottom() - card::footerHeight() + 1, body.width() - 2 * pad, card::footerHeight());
    card::paintRule(p, body.left() + 1, body.right() - 1, footer.top());
    card::paintHint(p, footer, QStringLiteral("j/k move · x cancel · c clear finished · Esc close"));

    int y = body.top() + card::headerHeight();
    if (tasks.isEmpty()) {
        p.setPen(pal.text);
        p.drawText(QRect(body.left() + pad, y, body.width() - 2 * pad, lineHeight()), Qt::AlignVCenter,
                   QStringLiteral("No tasks"));
        p.setPen(pal.subtext);
        p.drawText(QRect(body.left() + pad, y + lineHeight(), body.width() - 2 * pad, lineHeight()), Qt::AlignVCenter,
                   QStringLiteral("Copies, moves and deletes show their progress here"));
        return;
    }
    const int selected = std::clamp(m_selected, 0, int(tasks.size()) - 1);
    const QFont bold = boldFont();
    for (int i = 0; i < tasks.size(); ++i) {
        Task *t = tasks[i];
        const Task::Progress prog = t->progress();
        const QRect row(body.left() + 6, y, body.width() - 12, lineHeight() * 2 + 4);
        if (row.bottom() > footer.top())
            break;
        if (i == selected)
            card::paintSelection(p, QRectF(row));
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
        const int inner = row.width() - 2 * (pad - 6);
        const int left = row.left() + pad - 6;
        p.setFont(i == selected ? bold : font);
        p.setPen(pal.text);
        p.drawText(QRect(left, row.top(), inner - 110, lineHeight()), Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(t->title(), Qt::ElideMiddle, inner - 110));
        p.setFont(font);
        p.setPen(stateColor);
        p.drawText(QRect(left + inner - 104, row.top(), 104, lineHeight()), Qt::AlignVCenter | Qt::AlignRight, state);

        const QRectF bar(left, row.top() + lineHeight() + 1, inner, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(i == selected ? pal.overlay : pal.surface);
        p.drawRoundedRect(bar, 1.5, 1.5);
        p.setBrush(prog.state == Task::Failed ? pal.red : pal.accent);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * t->fraction(), bar.height()), 1.5, 1.5);
        QString detail = prog.state == Task::Failed ? prog.errors.value(0) : prog.current;
        if (prog.state == Task::Running && prog.totalBytes > 0)
            detail = QStringLiteral("%1 of %2  %3").arg(humanSize(prog.doneBytes), humanSize(prog.totalBytes), detail);
        p.setPen(pal.subtext);
        p.drawText(QRect(left, row.top() + lineHeight() + 4, inner, lineHeight()), Qt::AlignVCenter,
                   fm.elidedText(detail, Qt::ElideMiddle, inner));
        y += row.height() + 2;
    }
}

void KeyMapSheet::setContent(const QList<QPair<QString, QString>> &families, const QList<Group> &groups) {
    m_families = families;
    m_groups = groups;
    m_scroll = 0;
    update();
}

int KeyMapSheet::groupWidth() const {
    const QFontMetrics fm(Theme::instance().font());
    int keys = 0, desc = 0;
    for (const Group &g : m_groups) {
        for (const Entry &e : g.entries) {
            keys = std::max(keys, fm.horizontalAdvance(e.keys));
            desc = std::max(desc, fm.horizontalAdvance(e.description));
        }
    }
    return std::clamp(std::min(keys, 150) + 14 + std::min(desc, 260) + 24, 200, 440);
}

int KeyMapSheet::keyColumn() const {
    const QFontMetrics fm(Theme::instance().font());
    int w = 0;
    for (const Group &g : m_groups)
        for (const Entry &e : g.entries)
            w = std::max(w, fm.horizontalAdvance(e.keys));
    return std::min(w, 150);
}

QStringList KeyMapSheet::keyLines(const Entry &entry) const {
    const QFontMetrics fm(Theme::instance().font());
    const int width = keyColumn();
    QStringList lines;
    for (const QString &key : entry.keys.split(u' ', Qt::SkipEmptyParts)) {
        if (!lines.isEmpty() && fm.horizontalAdvance(lines.last() + u' ' + key) <= width)
            lines.last() += u' ' + key;
        else
            lines << key;
    }
    return lines.isEmpty() ? QStringList{QString()} : lines;
}

int KeyMapSheet::groupHeight(const Group &group) const {
    int lines = 0;
    for (const Entry &e : group.entries)
        lines += int(keyLines(e).size());
    return card::headerHeight() - 4 + lines * lineHeight() + 8;
}

int KeyMapSheet::introHeight() const { return 2 * lineHeight() + 10; }

QList<KeyMapSheet::Placed> KeyMapSheet::place(int width, int *height) const {
    constexpr int gap = 12;
    const int gw = groupWidth();
    const int columns = std::max(1, (width + gap) / (gw + gap));
    const int cw = (width - (columns - 1) * gap) / columns;
    QList<int> heights(columns, 0);
    QList<Placed> out;
    // Shortest column first keeps the boxes evenly stacked.
    for (int i = 0; i < m_groups.size(); ++i) {
        const int col = int(std::min_element(heights.begin(), heights.end()) - heights.begin());
        const int h = groupHeight(m_groups[i]);
        out.append({i, QRect(col * (cw + gap), heights[col], cw, h)});
        heights[col] += h + gap;
    }
    *height = std::max(0, *std::max_element(heights.begin(), heights.end()) - gap);
    return out;
}

QSize KeyMapSheet::preferredSize(int maxWidth, int maxHeight) const {
    const QMargins m = card::shadowMargins();
    const int pad = card::padding();
    const int bodyMax = maxWidth - m.left() - m.right();
    const int gw = groupWidth();
    int columns = 4;
    while (columns > 1 && columns * gw + (columns - 1) * 12 + 2 * pad > bodyMax)
        --columns;
    const int width = std::min(bodyMax, columns * gw + (columns - 1) * 12 + 2 * pad);
    int content = 0;
    place(width - 2 * pad, &content);
    const int height = card::headerHeight() + introHeight() + content + 12 + card::footerHeight();
    return card::around(QRect(0, 0, width, std::min(maxHeight - m.top() - m.bottom(), height))).size();
}

void KeyMapSheet::scrollBy(int rows) {
    m_scroll = std::max(0, m_scroll + rows * lineHeight());
    update();
}

void KeyMapSheet::wheelEvent(QWheelEvent *event) { scrollBy(-event->angleDelta().y() / 40); }

void KeyMapSheet::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect body = bodyRect();
    card::paintFrame(p, body);
    const Palette &pal = Theme::p();
    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.setFont(font);
    const int pad = card::padding();
    const int inner = body.width() - 2 * pad;
    int count = 0;
    for (const Group &g : m_groups)
        count += int(g.entries.size());
    card::paintHeader(p, QRect(body.left() + pad, body.top() + 2, inner, card::headerHeight() - 4), QStringLiteral("Key map"),
                      QStringLiteral("%1 bindings").arg(count));

    int y = body.top() + card::headerHeight();
    auto drawRun = [&](int &x, const QString &text, const QColor &color) {
        p.setPen(color);
        p.drawText(QRect(x, y, std::max(0, body.left() + pad + inner - x), lineHeight()), Qt::AlignVCenter | Qt::AlignLeft,
                   text);
        x += fm.horizontalAdvance(text);
    };
    int x = body.left() + pad;
    drawRun(x, QStringLiteral("How keys work: "), pal.text);
    drawRun(x, QStringLiteral("a single key acts on the cursor, or on the marks if there are any; a number first repeats (5j)"),
            pal.subtext);
    y += lineHeight();
    x = body.left() + pad;
    for (int i = 0; i < m_families.size(); ++i) {
        if (i > 0)
            drawRun(x, kDot, pal.muted);
        drawRun(x, m_families[i].first, pal.yellow);
        drawRun(x, u' ' + m_families[i].second, pal.subtext);
    }
    y += lineHeight() + 10;

    const QRect footer(body.left() + pad, body.bottom() - card::footerHeight() + 1, inner, card::footerHeight());
    const QRect content(body.left() + pad, y, inner, footer.top() - y - 6);
    int contentHeight = 0;
    const QList<Placed> placed = place(inner, &contentHeight);
    m_scroll = std::clamp(m_scroll, 0, std::max(0, contentHeight - content.height()));
    const bool scrollable = contentHeight > content.height();
    card::paintRule(p, body.left() + 1, body.right() - 1, footer.top());
    card::paintHint(p, footer, scrollable ? QStringLiteral("j/k scroll · Esc close") : QStringLiteral("Esc close"));

    p.save();
    p.setClipRect(content);
    const int keyWidth = keyColumn();
    for (const Placed &pl : placed) {
        const Group &g = m_groups[pl.group];
        const QRect box = pl.rect.translated(content.left(), content.top() - m_scroll);
        if (!box.intersects(content))
            continue;
        p.setPen(QPen(pal.overlay, 1));
        p.setBrush(pal.base);
        p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        const QFont bold = boldFont();
        p.setFont(bold);
        p.setPen(pal.text);
        p.drawText(QRect(box.left() + 12, box.top() + 2, box.width() - 24, card::headerHeight() - 6),
                   Qt::AlignVCenter | Qt::AlignLeft, g.title);
        p.setFont(font);
        int ry = box.top() + card::headerHeight() - 4;
        for (const Entry &e : g.entries) {
            p.setPen(pal.yellow);
            const QStringList keyText = keyLines(e);
            for (int i = 0; i < keyText.size(); ++i)
                p.drawText(QRect(box.left() + 12, ry + i * lineHeight(), keyWidth, lineHeight()),
                           Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(keyText[i], Qt::ElideRight, keyWidth));
            const int dx = box.left() + 12 + keyWidth + 14;
            p.setPen(pal.text);
            p.drawText(QRect(dx, ry, box.right() - 12 - dx, lineHeight()), Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(e.description, Qt::ElideRight, box.right() - 12 - dx));
            ry += int(keyText.size()) * lineHeight();
        }
    }
    p.restore();
}

PropertiesShelf::PropertiesShelf(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void PropertiesShelf::setContent(const QString &title, const QString &meta, const QList<QPair<QString, QString>> &rows) {
    m_title = title;
    m_meta = meta;
    m_rows = rows;
    updateGeometry();
    update();
}

void PropertiesShelf::updateRow(const QString &label, const QString &value) {
    for (auto &row : m_rows)
        if (row.first == label)
            row.second = value;
    update();
}

QString PropertiesShelf::value(const QString &label) const {
    for (const auto &row : m_rows)
        if (row.first == label)
            return row.second;
    return {};
}

QSize PropertiesShelf::sizeHint() const {
    return QSize(200, card::headerHeight() + int(m_rows.size()) * Theme::instance().rowHeight() + 10);
}

void PropertiesShelf::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    card::paintRule(p, 0, width(), 0);
    const QFont font = Theme::instance().font();
    const QFont bold = boldFont();
    const QFontMetrics fm(font);
    const QFontMetrics bfm(bold);
    const int pad = 12;
    const int lh = Theme::instance().rowHeight();

    const QRect header(pad, 2, width() - 2 * pad, card::headerHeight() - 4);
    const QString title = bfm.elidedText(m_title, Qt::ElideMiddle, header.width() * 2 / 3);
    p.setFont(bold);
    p.setPen(pal.text);
    p.drawText(header, Qt::AlignVCenter | Qt::AlignLeft, title);
    if (!m_meta.isEmpty()) {
        int x = header.left() + bfm.horizontalAdvance(title);
        p.setFont(font);
        p.setPen(pal.muted);
        p.drawText(QRect(x, header.top(), header.right() - x, header.height()), Qt::AlignVCenter | Qt::AlignLeft, kDot);
        x += fm.horizontalAdvance(kDot);
        p.setPen(pal.subtext);
        p.drawText(QRect(x, header.top(), header.right() - x, header.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(m_meta, Qt::ElideRight, header.right() - x));
    }

    p.setFont(font);
    int labelWidth = 0;
    for (const auto &row : m_rows)
        labelWidth = std::max(labelWidth, fm.horizontalAdvance(row.first));
    int y = card::headerHeight() - 2;
    for (const auto &row : m_rows) {
        p.setPen(pal.subtext);
        p.drawText(QRect(pad, y, labelWidth, lh), Qt::AlignVCenter | Qt::AlignLeft, row.first);
        const int x = pad + labelWidth + 16;
        p.setPen(pal.text);
        p.drawText(QRect(x, y, width() - x - pad, lh), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(row.second, Qt::ElideMiddle, width() - x - pad));
        y += lh;
    }
}
