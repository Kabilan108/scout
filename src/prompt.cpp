#include "prompt.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

PickerList::PickerList(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    hide();
}

int PickerList::rowHeight() const { return Theme::instance().rowHeight() + 2; }
int PickerList::headerHeight() const { return m_header.isEmpty() ? 6 : Theme::instance().rowHeight() + 4; }
int PickerList::footerHeight() const { return m_footer.isEmpty() ? 6 : Theme::instance().rowHeight() + 4; }

int PickerList::visibleRows() const {
    return std::max(1, (height() - headerHeight() - footerHeight()) / rowHeight());
}

int PickerList::preferredHeight() const {
    const int rows = std::clamp(int(m_items.size()), 1, m_maxRows);
    return headerHeight() + footerHeight() + rows * rowHeight() + 2;
}

void PickerList::setItems(QList<PickerItem> items) {
    m_items = std::move(items);
    m_selected = m_items.isEmpty() ? -1 : 0;
    m_scroll = 0;
    update();
    emit selectionChanged(m_selected);
}

const PickerItem *PickerList::selectedItem() const {
    return m_selected >= 0 && m_selected < m_items.size() ? &m_items[m_selected] : nullptr;
}

void PickerList::setSelected(int index) {
    if (m_items.isEmpty())
        return;
    m_selected = std::clamp(index, 0, int(m_items.size()) - 1);
    const int rows = visibleRows();
    if (m_selected < m_scroll)
        m_scroll = m_selected;
    else if (m_selected >= m_scroll + rows)
        m_scroll = m_selected - rows + 1;
    update();
    emit selectionChanged(m_selected);
}

void PickerList::moveSelection(int delta) {
    if (m_items.isEmpty())
        return;
    const int n = int(m_items.size());
    setSelected(((m_selected + delta) % n + n) % n);
}

int PickerList::rowAt(const QPoint &pos) const {
    const int y = pos.y() - headerHeight();
    if (y < 0)
        return -1;
    const int row = m_scroll + y / rowHeight();
    return row < m_items.size() && y / rowHeight() < visibleRows() ? row : -1;
}

void PickerList::mousePressEvent(QMouseEvent *event) {
    const int row = rowAt(event->position().toPoint());
    if (row >= 0)
        setSelected(row);
}

void PickerList::mouseDoubleClickEvent(QMouseEvent *event) {
    const int row = rowAt(event->position().toPoint());
    if (row >= 0)
        emit activated(row);
}

void PickerList::wheelEvent(QWheelEvent *event) {
    const int steps = event->angleDelta().y() / 120;
    m_scroll = std::clamp(m_scroll - steps * 3, 0, std::max(0, int(m_items.size()) - visibleRows()));
    update();
}

void PickerList::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &pal = Theme::p();
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(pal.overlay, 1));
    p.setBrush(pal.mantle);
    p.drawRoundedRect(frame, 6, 6);

    const QFont font = Theme::instance().font();
    const QFontMetrics fm(font);
    p.setFont(font);
    const int rh = rowHeight();
    int y = headerHeight();
    if (!m_header.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(QRect(12, 2, width() - 24, headerHeight()), Qt::AlignVCenter, m_header);
    }

    const int rows = visibleRows();
    for (int i = m_scroll; i < m_items.size() && i < m_scroll + rows; ++i) {
        const PickerItem &item = m_items[i];
        const QRectF row(4, y, width() - 8, rh);
        if (i == m_selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.surface);
            p.drawRoundedRect(row, 4, 4);
        }
        qreal x = row.left() + 8;
        if (item.icon) {
            const qreal s = std::round(fm.height() * 0.8);
            paintEntryIcon(&p, QRectF(x, row.center().y() - s / 2, s, s), *item.icon);
            x += s + 8;
        }
        const int hintWidth = item.hint.isEmpty() ? 0 : fm.horizontalAdvance(item.hint) + 16;
        const qreal available = row.right() - 8 - hintWidth - x;

        // Text with matched characters highlighted; directories end in '/'.
        QColor base = pal.text;
        if (item.icon == EntryKind::Directory)
            base = pal.accent;
        const QString text = fm.elidedText(item.text, Qt::ElideLeft, int(available * (item.description.isEmpty() ? 1.0 : 0.45)));
        const int offset = int(item.text.size() - text.size());
        const bool elided = text != item.text;
        QFont bold = font;
        bold.setBold(true);
        qreal tx = x;
        for (int c = 0; c < text.size(); ++c) {
            const int original = elided ? c + offset : c;
            const bool hit = (!elided || c > 0) && item.highlights.contains(original);
            p.setFont(hit ? bold : font);
            p.setPen(hit ? (i == m_selected ? pal.yellow : pal.orange) : base);
            const QString ch(text[c]);
            p.drawText(QPointF(tx, row.center().y() + fm.ascent() / 2.0 - 1), ch);
            tx += QFontMetricsF(hit ? bold : font).horizontalAdvance(ch);
        }
        p.setFont(font);
        if (!item.description.isEmpty()) {
            p.setPen(pal.muted);
            const qreal dx = std::max(tx + 16, x + available * 0.3);
            p.drawText(QRectF(dx, row.top(), row.right() - 8 - hintWidth - dx, rh), Qt::AlignVCenter,
                       fm.elidedText(item.description, Qt::ElideRight, int(row.right() - 8 - hintWidth - dx)));
        }
        if (!item.hint.isEmpty()) {
            p.setPen(pal.subtext);
            p.drawText(QRectF(row.right() - hintWidth - 4, row.top(), hintWidth, rh), Qt::AlignVCenter | Qt::AlignRight,
                       item.hint);
        }
        y += rh;
    }
    if (m_items.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(QRect(12, y, width() - 24, rh), Qt::AlignVCenter, QStringLiteral("no matches"));
    }
    if (!m_footer.isEmpty()) {
        p.setPen(pal.muted);
        p.drawText(QRect(12, height() - footerHeight(), width() - 24, footerHeight()), Qt::AlignVCenter, m_footer);
    }
}

Prompt::Prompt(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 3, 10, 3);
    layout->setSpacing(6);
    m_label = new QLabel(this);
    m_edit = new QLineEdit(this);
    m_edit->installEventFilter(this);
    layout->addWidget(m_label);
    layout->addWidget(m_edit, 1);
    connect(m_edit, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (m_spec.onChange)
            m_spec.onChange(text);
    });
    applyTheme();
    hide();
}

void Prompt::applyTheme() {
    const Palette &pal = Theme::p();
    QPalette lp = m_label->palette();
    lp.setColor(QPalette::WindowText, pal.accent);
    m_label->setPalette(lp);
    m_label->setFont(Theme::instance().font());
    m_edit->setFont(Theme::instance().font());
    update();
}

void Prompt::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.mantle);
    p.setPen(QPen(pal.accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(3.5, 1.5, -3.5, -1.5), 4, 4);
}

void Prompt::open(PromptSpec spec, PickerList *list) {
    if (m_open)
        close(false);
    m_spec = std::move(spec);
    m_list = list;
    m_label->setText(m_spec.label);
    m_edit->setText(m_spec.text);
    if (m_spec.selectionStart >= 0)
        m_edit->setSelection(m_spec.selectionStart, m_spec.selectionLength);
    else
        m_edit->end(false);
    m_open = true;
    show();
    m_edit->setFocus(Qt::OtherFocusReason);
    if (m_list && m_spec.showList) {
        m_list->setHeader({});
        m_list->setFooter({});
        m_list->show();
        m_list->raise();
    }
    if (m_spec.onChange)
        m_spec.onChange(m_edit->text());
}

void Prompt::setText(const QString &text) {
    m_edit->setText(text);
    if (m_spec.onChange)
        m_spec.onChange(text);
}

void Prompt::close(bool accepted) {
    if (!m_open)
        return;
    m_open = false;
    // close() may run inside one of the spec's own callbacks; keep them alive
    // until control returns to the event loop.
    auto spec = std::make_shared<PromptSpec>(std::move(m_spec));
    m_spec = {};
    QTimer::singleShot(0, this, [spec] {});
    if (m_list)
        m_list->hide();
    hide();
    if (!accepted && spec->onCancel)
        spec->onCancel();
    emit closed();
}

bool Prompt::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_edit || event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);
    const bool ctrl = key->modifiers() & Qt::ControlModifier;
    const bool listVisible = m_list && m_list->isVisible();
    switch (key->key()) {
    case Qt::Key_Escape:
        close(false);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const QString text = m_edit->text();
        auto accept = m_spec.onAccept;
        const PickerItem *item = listVisible ? m_list->selectedItem() : nullptr;
        const PickerItem copy = item ? *item : PickerItem{};
        close(true);
        if (accept)
            accept(text, item ? &copy : nullptr);
        return true;
    }
    case Qt::Key_Up:
        if (listVisible)
            m_list->moveSelection(-1);
        return true;
    case Qt::Key_Down:
        if (listVisible)
            m_list->moveSelection(1);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        if (m_spec.onComplete) {
            const QString next = m_spec.onComplete(m_edit->text(), listVisible ? m_list->selectedItem() : nullptr);
            if (!next.isNull())
                setText(next);
        } else if (listVisible) {
            m_list->moveSelection(key->key() == Qt::Key_Backtab ? -1 : 1);
        }
        return true;
    case Qt::Key_P:
    case Qt::Key_K:
        if (ctrl && listVisible) {
            m_list->moveSelection(-1);
            return true;
        }
        break;
    case Qt::Key_N:
    case Qt::Key_J:
        if (ctrl && listVisible) {
            m_list->moveSelection(1);
            return true;
        }
        break;
    case Qt::Key_Backspace:
        if (m_edit->text().isEmpty()) {
            close(false);
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
