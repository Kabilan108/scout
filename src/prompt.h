#pragma once

#include <QColor>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QVariant>
#include <QWidget>

#include <functional>
#include <optional>

#include "columnview.h"

struct PickerItem {
    QString text;
    QString description;    // second column, secondary text
    QString hint;           // right-aligned secondary text, e.g. an app id
    QString keys;           // right-aligned key bindings, in the key color
    QList<int> highlights;  // character positions in `text`
    QList<int> descriptionHighlights;
    std::optional<EntryKind> icon;
    QVariant data;
};

// The floating card behind the palette, the finder, open-with and
// confirmations: an optional input row, a header, rows, and a footer hint.
class PickerList : public QWidget {
    Q_OBJECT
public:
    explicit PickerList(QWidget *parent = nullptr);

    // Back to an empty card with a header-less list.
    void reset(bool inputRow);
    void setItems(QList<PickerItem> items);
    const QList<PickerItem> &items() const { return m_items; }
    void setHeader(const QString &title, const QString &meta = {});
    QString header() const { return m_header; }
    QString headerMeta() const { return m_meta; }
    void setFooter(const QString &footer) { m_footer = footer; update(); }
    // Shown instead of rows when there are none.
    void setEmptyText(const QString &title, const QString &hint = {});
    QString emptyTitle() const { return m_emptyTitle; }
    // Confirmations list what they act on; nothing there is selectable.
    void setShowSelection(bool show) { m_showSelection = show; update(); }
    int selectedIndex() const { return m_selected; }
    const PickerItem *selectedItem() const;
    void setSelected(int index);
    void moveSelection(int delta);
    // True once the user moved the selection since the items last changed.
    bool selectionTouched() const { return m_touched; }
    void setMaxRows(int rows) { m_maxRows = rows; }
    // Full widget height, shadow included.
    int preferredHeight() const;
    // Where the input line goes, in this widget's coordinates.
    QRect inputRect() const;
    QRect bodyRect() const;

signals:
    void activated(int index);
    void selectionChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int rowHeight() const;
    int inputHeight() const;
    int headerHeight() const;
    int footerHeight() const;
    int rowsTop() const;
    int visibleRows() const;
    int rowAt(const QPoint &pos) const;

    QList<PickerItem> m_items;
    QString m_header;
    QString m_meta;
    QString m_footer;
    QString m_emptyTitle;
    QString m_emptyHint;
    bool m_inputRow = false;
    bool m_showSelection = true;
    bool m_touched = false;
    int m_selected = 0;
    int m_scroll = 0;
    int m_maxRows = 12;
};

struct PromptSpec {
    // Bar: a line above the status bar. Palette: the input row of the floating
    // card. Confirm: no visible input; the first key typed answers.
    enum class Style { Bar, Palette, Confirm };
    Style style = Style::Bar;
    QString label;
    QString text;
    QString placeholder;
    QString hint;           // right side of the bar, e.g. "Enter rename · Esc cancel"
    int selectionStart = -1;
    int selectionLength = 0;
    bool showList = false;
    std::function<void(const QString &)> onChange;
    std::function<void(const QString &, const PickerItem *)> onAccept;
    std::function<void()> onCancel;
    // Tab completion: returns the new text, or a null string to keep the current one.
    std::function<QString(const QString &, const PickerItem *)> onComplete;
};

// The input line, placed by the window according to its style.
class Prompt : public QWidget {
    Q_OBJECT
public:
    explicit Prompt(QWidget *parent = nullptr);

    void open(PromptSpec spec, PickerList *list);
    void close(bool accepted);
    bool isOpen() const { return m_open; }
    PromptSpec::Style style() const { return m_spec.style; }
    QString text() const { return m_edit->text(); }
    void setText(const QString &text);
    // Status on the right of the bar, such as "3 of 41".
    void setInfo(const QString &info, const QColor &color = {});
    QString info() const { return m_info; }
    QLineEdit *edit() const { return m_edit; }
    void applyTheme();

signals:
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateMargins();

    QLabel *m_label;
    QLineEdit *m_edit;
    PickerList *m_list = nullptr;
    PromptSpec m_spec;
    QString m_info;
    QColor m_infoColor;
    bool m_open = false;
};
