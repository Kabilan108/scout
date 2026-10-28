#include "prompt.h"

#include "overlays.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

namespace {

// Draws `text` with the characters at `hits` (positions in `original`) in the
// highlight color; `text` may be `original` elided at either end.
qreal drawHighlighted(QPainter &p, qreal x, qreal baseline, const QString &text, const QString &original,
                      const QList<int> &hits, const QFont &font, const QColor &color, const QColor &hitColor) {
    const bool elided = text != original;
    const bool elidedLeft = elided && text.startsWith(QChar(0x2026));
    const int offset = int(original.size() - text.size());
    QFont hitFont = font;
    hitFont.setBold(true);
    for (int c = 0; c < text.size(); ++c) {
        const bool ellipsis = elided && (elidedLeft ? c == 0 : c == text.size() - 1);
        const int index = elidedLeft ? c + offset : c;
        const bool hit = !ellipsis && hits.contains(index);
        p.setFont(hit ? hitFont : font);
        p.setPen(hit ? hitColor : color);
        const QString ch(text[c]);
        p.drawText(QPointF(x, baseline), ch);
        x += QFontMetricsF(p.font()).horizontalAdvance(ch);
    }
    p.setFont(font);
    return x;
}

} // namespace

PickerList::PickerList(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    hide();
}

int PickerList::rowHeight() const { return card::rowHeight(); }
int PickerList::inputHeight() const { return m_inputRow ? Theme::instance().rowHeight() + 16 : 0; }
int PickerList::headerHeight() const { return m_header.isEmpty() && m_meta.isEmpty() ? 6 : card::headerHeight(); }
int PickerList::footerHeight() const { return m_footer.isEmpty() ? 6 : card::footerHeight() + 4; }
QRect PickerList::bodyRect() const { return card::body(rect()); }
int PickerList::rowsTop() const { return bodyRect().top() + inputHeight() + headerHeight(); }

QRect PickerList::inputRect() const {
    const QRect body = bodyRect();
    return QRect(body.left() + 4, body.top() + 2, body.width() - 8, inputHeight() - 4);
}

int PickerList::visibleRows() const {
    return std::max(1, (bodyRect().bottom() - rowsTop() - footerHeight()) / rowHeight());
}

int PickerList::preferredHeight() const {
    const QMargins m = card::shadowMargins();
    const int rows = m_items.isEmpty() ? (m_emptyTitle.isEmpty() ? 0 : (m_emptyHint.isEmpty() ? 1 : 2))
                                       : std::min(int(m_items.size()), m_maxRows);
    return m.top() + m.bottom() + inputHeight() + headerHeight() + rows * rowHeight() + footerHeight() + 4;
}

void PickerList::reset(bool inputRow) {
    m_inputRow = inputRow;
    m_header.clear();
    m_meta.clear();
    m_footer.clear();
    m_emptyTitle.clear();
    m_emptyHint.clear();
    m_showSelection = true;
    m_items.clear();
    m_selected = -1;
    m_scroll = 0;
    m_touched = false;
    update();
}

void PickerList::setItems(QList<PickerItem> items) {
    m_items = std::move(items);
    m_selected = m_items.isEmpty() ? -1 : 0;
    m_scroll = 0;
    m_touched = false;
    update();
    emit selectionChanged(m_selected);
}

void PickerList::setHeader(const QString &title, const QString &meta) {
    m_header = title;
    m_meta = meta;
    update();
}

void PickerList::setEmptyText(const QString &title, const QString &hint) {
    m_emptyTitle = title;
    m_emptyHint = hint;
    update();
}

const PickerItem *PickerList::selectedItem() const {
    return m_selected >= 0 && m_selected < m_items.size() ? &m_items[m_selected] : nullptr;
}

void PickerList::setSelected(int index) {
    if (m_items.isEmpty())
        return;
    m_selected = std::clamp(index, 0, int(m_items.size()) - 1);
    m_touched = true;
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
    const int y = pos.y() - rowsTop();
    if (y < 0 || !bodyRect().contains(pos))
        return -1;
    const int row = m_scroll + y / rowHeight();
    return row < m_items.size() && y / rowHeight() < visibleRows() ? row : -1;
}

void PickerList::mousePressEvent(QMouseEvent *event) {
    const int row = rowAt(event->position().toPoint());
    if (row >= 0 && m_showSelection)
        setSelected(row);
}

void PickerList::mouseDoubleClickEvent(QMouseEvent *event) {
    const int row = rowAt(event->position().toPoint());
    if (row >= 0 && m_showSelection)
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
    const QRect body = bodyRect();
    card::paintFrame(p, body);

    const QFont font = Theme::instance().font();
    QFont bold = font;
    bold.setBold(true);
    const QFontMetrics fm(font);
    p.setFont(font);
    const int pad = card::padding();
    const int rh = rowHeight();
    int y = body.top();
    if (m_inputRow) {
        y += inputHeight();
        card::paintRule(p, body.left() + 1, body.right() - 1, y - 1);
    }
    if (!m_header.isEmpty() || !m_meta.isEmpty())
        card::paintHeader(p, QRect(body.left() + pad, y + 2, body.width() - 2 * pad, headerHeight() - 6), m_header, m_meta);
    y = rowsTop();

    const int rows = visibleRows();
    for (int i = m_scroll; i < m_items.size() && i < m_scroll + rows; ++i) {
        const PickerItem &item = m_items[i];
        const bool selected = m_showSelection && i == m_selected;
        const QRectF row(body.left() + 6, y, body.width() - 12, rh);
        if (selected)
            card::paintSelection(p, row);
        qreal x = row.left() + pad - 6;
        if (item.icon) {
            const qreal s = std::round(fm.height() * 0.8);
            paintEntryIcon(&p, QRectF(x, row.center().y() - s / 2, s, s), *item.icon);
            x += s + 8;
        }
        const qreal end = row.right() - (pad - 6);
        const qreal full = end - x;
        const QString right = item.keys.isEmpty() ? item.hint : item.keys;
        const int rightText = right.isEmpty() ? 0 : std::min(fm.horizontalAdvance(right), int(full * 0.35));
        const int rightWidth = right.isEmpty() ? 0 : rightText + 16;
        const qreal baseline = row.center().y() + (fm.ascent() - fm.descent()) / 2.0;
        const QFont textFont = selected ? bold : font;
        const QFontMetrics tfm(textFont);

        // The second column lines up across rows whatever the right side holds.
        // Paths keep their file name when elided; descriptions keep their start.
        const bool hasColumn = !item.description.isEmpty();
        const qreal column = x + full * 0.55;
        const int textWidth = int(hasColumn ? column - x - 16 : full - rightWidth);
        const QString text = tfm.elidedText(item.text, hasColumn ? Qt::ElideRight : Qt::ElideLeft, textWidth);
        const QColor textColor = item.icon == EntryKind::Directory ? pal.accent : pal.text;
        drawHighlighted(p, x, baseline, text, item.text, item.highlights, textFont, textColor, pal.yellow);
        if (hasColumn) {
            const int width = int(end - rightWidth - column);
            const QString desc = fm.elidedText(item.description, Qt::ElideRight, std::max(0, width));
            drawHighlighted(p, column, baseline, desc, item.description, item.descriptionHighlights, font, pal.subtext,
                            pal.yellow);
        }
        if (!right.isEmpty()) {
            p.setFont(font);
            p.setPen(item.keys.isEmpty() ? pal.subtext : pal.yellow);
            p.drawText(QRectF(end - rightText - 1, row.top(), rightText + 2, rh), Qt::AlignVCenter | Qt::AlignRight,
                       fm.elidedText(right, Qt::ElideMiddle, rightText + 1));
        }
        y += rh;
    }
    if (m_items.isEmpty() && !m_emptyTitle.isEmpty()) {
        const QRect line(body.left() + pad, y, body.width() - 2 * pad, rh);
        p.setPen(pal.text);
        p.drawText(line, Qt::AlignVCenter, fm.elidedText(m_emptyTitle, Qt::ElideMiddle, line.width()));
        if (!m_emptyHint.isEmpty()) {
            p.setPen(pal.subtext);
            p.drawText(line.translated(0, rh), Qt::AlignVCenter, fm.elidedText(m_emptyHint, Qt::ElideRight, line.width()));
        }
    }
    if (!m_footer.isEmpty()) {
        const int top = body.bottom() - footerHeight() + 4;
        card::paintRule(p, body.left() + 1, body.right() - 1, top);
        card::paintHint(p, QRect(body.left() + pad, top + 1, body.width() - 2 * pad, footerHeight() - 5), m_footer);
    }
}

Prompt::Prompt(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setSpacing(8);
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
    updateMargins();
    hide();
}

void Prompt::applyTheme() {
    const Palette &pal = Theme::p();
    QPalette lp = m_label->palette();
    lp.setColor(QPalette::WindowText, pal.accent);
    m_label->setPalette(lp);
    m_label->setFont(Theme::instance().font());
    m_edit->setFont(Theme::instance().font());
    updateMargins();
    update();
}

void Prompt::updateMargins() {
    if (m_spec.style != PromptSpec::Style::Bar) {
        layout()->setContentsMargins(10, 0, 10, 0);
        return;
    }
    const QFontMetrics fm(Theme::instance().font());
    int right = 12;
    if (!m_spec.hint.isEmpty())
        right += card::hintWidth(m_spec.hint) + 16;
    if (!m_info.isEmpty())
        right += fm.horizontalAdvance(m_info) + 16;
    layout()->setContentsMargins(12, 1, right, 0);
}

void Prompt::setInfo(const QString &info, const QColor &color) {
    m_info = info;
    m_infoColor = color;
    updateMargins();
    update();
}

void Prompt::paintEvent(QPaintEvent *) {
    if (m_spec.style != PromptSpec::Style::Bar)
        return;
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    card::paintRule(p, 0, width(), 0);
    const QFontMetrics fm(Theme::instance().font());
    int right = width() - 12;
    const QRect line(0, 1, width(), height() - 1);
    if (!m_spec.hint.isEmpty()) {
        card::paintHint(p, QRect(12, line.top(), right - 12, line.height()), m_spec.hint, Qt::AlignRight);
        right -= card::hintWidth(m_spec.hint) + 16;
    }
    if (!m_info.isEmpty()) {
        p.setFont(Theme::instance().font());
        p.setPen(m_infoColor.isValid() ? m_infoColor : pal.subtext);
        p.drawText(QRect(12, line.top(), right - 12, line.height()), Qt::AlignVCenter | Qt::AlignRight, m_info);
    }
}

void Prompt::open(PromptSpec spec, PickerList *list) {
    if (m_open)
        close(false);
    m_spec = std::move(spec);
    m_list = list;
    m_info.clear();
    const bool confirm = m_spec.style == PromptSpec::Style::Confirm;
    m_label->setText(m_spec.label);
    m_label->setVisible(!confirm && !m_spec.label.isEmpty());
    // A confirmation reads single keys; read-only also hides the cursor.
    m_edit->setReadOnly(confirm);
    m_edit->setPlaceholderText(m_spec.placeholder);
    m_edit->setText(m_spec.text);
    if (m_spec.selectionStart >= 0)
        m_edit->setSelection(m_spec.selectionStart, m_spec.selectionLength);
    else
        m_edit->end(false);
    updateMargins();
    m_open = true;
    show();
    m_edit->setFocus(Qt::OtherFocusReason);
    if (m_list && m_spec.showList) {
        m_list->reset(m_spec.style == PromptSpec::Style::Palette);
        m_list->show();
        m_list->raise();
        raise();
    }
    if (m_spec.onChange && !confirm)
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
    m_info.clear();
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
    default:
        break;
    }
    if (m_spec.style == PromptSpec::Style::Confirm) {
        if (!key->text().isEmpty() && key->text()[0].isPrint() && m_spec.onChange)
            m_spec.onChange(key->text());
        return true;
    }
    switch (key->key()) {
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
