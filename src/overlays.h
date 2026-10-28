#pragma once

#include <QColor>
#include <QList>
#include <QMargins>
#include <QPair>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "keymap.h"

class QPainter;
class Task;
class TaskManager;

// The look shared by every floating surface: a soft drop shadow around a
// mantle body with a 1px border, a header row (title left, meta right), and a
// footer hint line. Card widgets are larger than their body by shadowMargins().
namespace card {
QMargins shadowMargins();
int radius();
int padding();
int rowHeight();
int headerHeight();
int footerHeight();
QRect body(const QRect &widgetRect);
QRect around(const QRect &body);
void paintFrame(QPainter &p, const QRect &body);
// `key`, when given, leads the title in the key color: "y · copy".
void paintHeader(QPainter &p, const QRect &row, const QString &title, const QString &meta, const QString &key = {});
void paintRule(QPainter &p, int left, int right, int y);
void paintSelection(QPainter &p, const QRectF &row);
// "↑↓ move · Enter run · Esc close": the first word of each part is its key.
void paintHint(QPainter &p, const QRect &row, const QString &hint, Qt::Alignment align = Qt::AlignLeft);
int hintWidth(const QString &hint);
// Keys as people write them: "Ctrl-k", "Enter", "←".
QString keyLabel(const QStringList &keys);
} // namespace card

class HeaderBar : public QWidget {
    Q_OBJECT
public:
    explicit HeaderBar(QWidget *parent = nullptr);
    void setPath(const QString &dir, const QString &name);
    // Tabs on the right, one per tab, even when there is only one.
    void setTabs(const QStringList &labels, int active);
    QList<QRect> tabRects() const;
    QSize sizeHint() const override;

signals:
    void tabClicked(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString m_dir;
    QString m_name;
    QStringList m_tabs;
    int m_activeTab = 0;
};

class StatusBar : public QWidget {
    Q_OBJECT
public:
    enum class Level { Info, Success, Warning, Error };
    struct Segment {
        QString text;
        QColor color;
    };

    explicit StatusBar(QWidget *parent = nullptr);
    void setLeft(const QStringList &segments);
    void setRight(const QList<Segment> &segments);
    // A mode badge such as "VISUAL", first on the right; empty hides it.
    void setMode(const QString &mode);
    QString mode() const { return m_mode; }
    void showMessage(const QString &message, Level level = Level::Info, int timeoutMs = 4000);
    QString message() const { return m_message; }
    // Number of items offered by the drag chip; 0 hides it.
    void setDragItems(int count, std::function<QStringList()> provider);
    QRect dragChipRect() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    int rightWidth() const;

    QStringList m_left;
    QList<Segment> m_right;
    QString m_mode;
    QString m_message;
    Level m_level = Level::Info;
    QTimer m_messageTimer;
    int m_dragCount = 0;
    std::function<QStringList()> m_dragProvider;
    QPoint m_pressPos;
    bool m_pressedChip = false;
};

class Card : public QWidget {
    Q_OBJECT
public:
    explicit Card(QWidget *parent = nullptr);

protected:
    QRect bodyRect() const { return card::body(rect()); }
    int lineHeight() const;
};

// Dims whatever is behind a floating card; a click on it dismisses the card.
class Scrim : public QWidget {
    Q_OBJECT
public:
    explicit Scrim(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
};

class WhichKey : public Card {
    Q_OBJECT
public:
    using Card::Card;
    void setContinuations(const QString &prefix, const QString &family, const QList<Continuation> &items);
    // Full widget size, shadow included.
    QSize preferredSize(int maxWidth, int maxHeight) const;
    // Entries left out at the current size, summarised as "+N more".
    int hiddenCount() const { return layout(bodyRect().size()).hidden; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Layout {
        int columns = 1;
        int rows = 0;
        int keyWidth = 0;
        int columnWidth = 0;
        int hidden = 0;
    };
    Layout layout(const QSize &body) const;

    QString m_prefix;
    QString m_family;
    QList<Continuation> m_items;
};

class TasksView : public Card {
    Q_OBJECT
public:
    TasksView(TaskManager *tasks, QWidget *parent = nullptr);
    void moveSelection(int delta);
    Task *selectedTask() const;
    QSize preferredSize(int maxWidth, int maxHeight) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    TaskManager *m_tasks;
    int m_selected = 0;
};

// The cheat sheet: groups of bindings, each in its own box.
class KeyMapSheet : public Card {
    Q_OBJECT
public:
    struct Entry {
        QString keys;
        QString description;
    };
    struct Group {
        QString title;
        QList<Entry> entries;
    };

    using Card::Card;
    // Prefix keys and what their family does: {"g", "go"}.
    void setContent(const QList<QPair<QString, QString>> &families, const QList<Group> &groups);
    const QList<Group> &groups() const { return m_groups; }
    QSize preferredSize(int maxWidth, int maxHeight) const;
    void scrollBy(int rows);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Placed {
        int group;
        QRect rect;
    };
    QList<Placed> place(int width, int *height) const;
    int groupWidth() const;
    int keyColumn() const;
    // An entry's keys, packed onto as many lines as the key column needs.
    QStringList keyLines(const Entry &entry) const;
    int groupHeight(const Group &group) const;
    int introHeight() const;

    QList<QPair<QString, QString>> m_families;
    QList<Group> m_groups;
    int m_scroll = 0;
};

// Details of the targets, docked under the preview.
class PropertiesShelf : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesShelf(QWidget *parent = nullptr);
    void setContent(const QString &title, const QString &meta, const QList<QPair<QString, QString>> &rows);
    void updateRow(const QString &label, const QString &value);
    QString title() const { return m_title; }
    QString value(const QString &label) const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_title;
    QString m_meta;
    QList<QPair<QString, QString>> m_rows;
};
