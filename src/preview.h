#pragma once

#include <QImage>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QStackedWidget>

#include <KSyntaxHighlighting/Repository>

#include "columnview.h"
#include "dirmodel.h"

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
}

class ImageView : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    void setImage(const QImage &image, const QString &path, const QString &caption);
    void clear();
    QString path() const { return m_path; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QImage m_image;
    QString m_path;
    QString m_caption;
    QPoint m_pressPos;
    bool m_pressed = false;
};

class InfoView : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    void setEntry(const FileEntry &entry, const QString &mimeDescription, const QString &note = {});

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    FileEntry m_entry;
    QString m_mime;
    QString m_note;
};

class Preview : public QStackedWidget {
    Q_OBJECT
public:
    enum class Kind { None, Directory, Text, Image, Info };

    Preview(Marks *marks, QWidget *parent = nullptr);

    void showEntry(const FileEntry &entry);
    void clear();
    void setOptions(const ViewOptions &options);
    void applyTheme();

    Kind kind() const { return m_kind; }
    QString path() const { return m_path; }
    QString text() const;
    ColumnView *dirView() const { return m_dirView; }
    DirModel *dirModel() const { return m_dirModel; }
    // Name to select when the previewed directory finishes loading.
    void setDirCursorHint(const QString &name) { m_dirCursorHint = name; }

private:
    void showText(const QString &text, const QString &path, bool highlight);
    void showImageAsync(const FileEntry &entry);
    void showArchiveAsync(const FileEntry &entry);
    void showProcessImage(const FileEntry &entry, const QString &program, const QStringList &args);
    void showInfo(const FileEntry &entry, const QString &note = {});
    QSize imageTargetSize() const;

    DirModel *m_dirModel;
    ColumnView *m_dirView;
    QPlainTextEdit *m_text;
    ImageView *m_image;
    InfoView *m_info;
    KSyntaxHighlighting::Repository m_repo;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    QPointer<QProcess> m_process;
    Kind m_kind = Kind::None;
    QString m_path;
    QString m_dirCursorHint;
    quint64 m_generation = 0;
};
