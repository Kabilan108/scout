#pragma once

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
    QString description;
    QString hint;           // right-aligned, e.g. a key binding
    QList<int> highlights;  // character positions in `text`
    std::optional<EntryKind> icon;
    QVariant data;
};

// Floating list above the prompt: command completions, fuzzy results, apps.
class PickerList : public QWidget {
    Q_OBJECT
public:
    explicit PickerList(QWidget *parent = nullptr);

    void setItems(QList<PickerItem> items);
    const QList<PickerItem> &items() const { return m_items; }
    void setHeader(const QString &header) { m_header = header; update(); }
    void setFooter(const QString &footer) { m_footer = footer; update(); }
    int selectedIndex() const { return m_selected; }
    const PickerItem *selectedItem() const;
    void setSelected(int index);
    void moveSelection(int delta);
    void setMaxRows(int rows) { m_maxRows = rows; }
    int preferredHeight() const;

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
    int headerHeight() const;
    int footerHeight() const;
    int visibleRows() const;
    int rowAt(const QPoint &pos) const;

    QList<PickerItem> m_items;
    QString m_header;
    QString m_footer;
    int m_selected = 0;
    int m_scroll = 0;
    int m_maxRows = 12;
};

struct PromptSpec {
    QString label;
    QString text;
    int selectionStart = -1;
    int selectionLength = 0;
    bool showList = false;
    std::function<void(const QString &)> onChange;
    std::function<void(const QString &, const PickerItem *)> onAccept;
    std::function<void()> onCancel;
    // Tab completion: returns the new text, or a null string to keep the current one.
    std::function<QString(const QString &, const PickerItem *)> onComplete;
};

// The bottom input line. Replaces the status bar while active.
class Prompt : public QWidget {
    Q_OBJECT
public:
    explicit Prompt(QWidget *parent = nullptr);

    void open(PromptSpec spec, PickerList *list);
    void close(bool accepted);
    bool isOpen() const { return m_open; }
    QString text() const { return m_edit->text(); }
    void setText(const QString &text);
    QLineEdit *edit() const { return m_edit; }
    void applyTheme();

signals:
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_label;
    QLineEdit *m_edit;
    PickerList *m_list = nullptr;
    PromptSpec m_spec;
    bool m_open = false;
};
