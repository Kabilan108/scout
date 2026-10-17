#pragma once

#include <QList>
#include <QPair>
#include <QStringList>
#include <QWidget>

#include <functional>

#include "dirmodel.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// One entry of a portal file filter: a name and (kind, pattern) rules, where
// kind 0 is a glob such as "*.png" and kind 1 a MIME type such as "image/*".
struct FileFilter {
    QString name;
    QList<QPair<uint, QString>> rules;

    bool accepts(const FileEntry &entry) const;
};

// A portal "choice": a checkbox when it has no options, otherwise a combo box.
struct ChooserChoice {
    QString id;
    QString label;
    QList<QPair<QString, QString>> options; // (id, label)
    QString selected;
};

struct ChooserRequest {
    enum class Mode { Open, Save, SaveFiles };
    Mode mode = Mode::Open;
    QString title;
    QString acceptLabel;
    bool multiple = false;
    bool directory = false;
    QList<FileFilter> filters;
    int currentFilter = -1;
    QList<ChooserChoice> choices;
    QString currentName;   // Save: suggested file name
    QString currentFolder; // where to start
    QString currentFile;   // Save: an existing file to start from
    QStringList files;     // SaveFiles: names of the files being saved
};

struct ChooserResult {
    bool accepted = false;
    QStringList paths;
    int filter = -1;
    QList<QPair<QString, QString>> choices; // (id, selected)
};

// The strip under the columns in chooser mode: file name, type filter,
// choices, and the cancel/accept buttons.
class ChooserBar : public QWidget {
    Q_OBJECT
public:
    explicit ChooserBar(const ChooserRequest &request, QWidget *parent = nullptr);

    QLineEdit *nameEdit() const { return m_name; }
    QString name() const;
    void setName(const QString &name);
    int filterIndex() const;
    void cycleFilter();
    QList<QPair<QString, QString>> choices() const;
    void setHint(const QString &hint);
    void setAcceptText(const QString &text);
    void applyTheme();

signals:
    void acceptClicked();
    void cancelClicked();
    void filterChanged(int index);

private:
    QLabel *m_hint = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_filter = nullptr;
    QList<QPair<QString, QWidget *>> m_choiceWidgets;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_accept = nullptr;
};
