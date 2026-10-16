#pragma once

#include <QList>
#include <QPair>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "keymap.h"

class Task;
class TaskManager;

class HeaderBar : public QWidget {
    Q_OBJECT
public:
    explicit HeaderBar(QWidget *parent = nullptr);
    void setPath(const QString &dir, const QString &name);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_dir;
    QString m_name;
};

class StatusBar : public QWidget {
    Q_OBJECT
public:
    enum class Level { Info, Success, Warning, Error };

    explicit StatusBar(QWidget *parent = nullptr);
    void setLeft(const QStringList &segments);
    void setRight(const QStringList &segments);
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
    QStringList m_left;
    QStringList m_right;
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
    void paintFrame(QPainter &p, const QString &title, const QString &footer);
    int lineHeight() const;
};

class WhichKey : public Card {
    Q_OBJECT
public:
    using Card::Card;
    void setContinuations(const QString &prefix, const QList<Continuation> &items);
    QSize preferredSize(int maxWidth) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int columns(int width) const;
    int columnWidth() const;

    QString m_prefix;
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

class PropertiesView : public Card {
    Q_OBJECT
public:
    using Card::Card;
    void setRows(const QString &title, const QList<QPair<QString, QString>> &rows);
    void updateRow(const QString &key, const QString &value);
    QSize preferredSize(int maxWidth) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_title;
    QList<QPair<QString, QString>> m_rows;
};
