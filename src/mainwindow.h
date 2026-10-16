#pragma once

#include <QHash>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include "clipboard.h"
#include "columnview.h"
#include "commands.h"
#include "desktopentry.h"
#include "dirmodel.h"
#include "fileops.h"
#include "finder.h"
#include "keymap.h"
#include "overlays.h"
#include "preview.h"
#include "prompt.h"

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(const QString &startPath, bool loadUserConfig = true, QWidget *parent = nullptr);
    ~MainWindow() override;

    QString cwd() const { return m_cwd; }
    const FileEntry *currentEntry() const;
    int cursorRow() const;
    DirModel *currentModel() const { return m_currentModel; }
    DirModel *parentModel() const { return m_parentModel; }
    ColumnView *currentView() const { return m_currentView; }
    Preview *preview() const { return m_preview; }
    Prompt *prompt() const { return m_prompt; }
    PickerList *picker() const { return m_picker; }
    StatusBar *statusBar() const { return m_status; }
    TaskManager *tasks() { return &m_tasks; }
    Marks *marks() { return &m_marks; }
    KeyMap *keymap() { return &m_keys; }
    const ViewOptions &options() const { return m_options; }
    bool isIdle() const;

    bool runCommandLine(const QString &line, int count = 0);
    bool cd(const QString &path, const QString &selectName = {}, bool recordHistory = true);
    QStringList targets() const;

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class Overlay { None, Tasks, Properties };

    void buildUi();
    void registerCommands();
    void bindDefaults();
    void loadConfig();
    void applyTheme();
    void applyOptions();
    void positionOverlays();

    void onCursorMoved();
    void onCurrentRefreshed();
    void onParentRefreshed();
    void updatePreview();
    void updateStatus();
    void updateHeader();
    void message(const QString &text, StatusBar::Level level = StatusBar::Level::Info);
    void error(const QString &text) { message(text, StatusBar::Level::Error); }

    void setCursor(int row);
    void moveCursor(int delta);
    void enter();
    void leave();
    void openTargets();
    void openWith(const DesktopApp &app, const QStringList &paths);
    void showOpenWith();
    void handleEscape();
    void handleOverlayKey(QKeyEvent *event);
    void showWhichKey();
    void hideWhichKey();
    void showTasks();
    void showProperties();
    void closeOverlay();

    void yank(bool cut);
    void paste(Task::Kind kind);
    void startTask(Task::Kind kind, const QStringList &paths, const QString &target);
    void onTaskFinished(Task *task);
    void promptRename();
    void promptCreate(const QString &initial = {});
    void createPath(const QString &input);
    void confirmDelete();
    void promptFilter();
    void promptSearch(bool backwards = false);
    void searchNext(int direction);
    void promptCommand(const QString &initial = {});
    void updateCommandList(const QString &text);
    void promptShell();
    void promptFind();
    void updateFinderList();
    void showHelp();
    void showContextMenu(int row, const QPoint &globalPos);
    void openPrompt(PromptSpec spec);

    // Models and views
    Marks m_marks;
    ViewOptions m_options;
    DirModel *m_parentModel = nullptr;
    DirModel *m_currentModel = nullptr;
    ColumnView *m_parentView = nullptr;
    ColumnView *m_currentView = nullptr;
    Preview *m_preview = nullptr;
    QSplitter *m_splitter = nullptr;
    HeaderBar *m_header = nullptr;
    QStackedWidget *m_bottom = nullptr;
    StatusBar *m_status = nullptr;
    Prompt *m_prompt = nullptr;
    PickerList *m_picker = nullptr;
    WhichKey *m_whichKey = nullptr;
    TasksView *m_tasksView = nullptr;
    PropertiesView *m_properties = nullptr;
    Overlay m_overlay = Overlay::None;
    bool m_showPreview = true;

    // State
    QString m_cwd;
    QStringList m_back;
    QStringList m_forward;
    QHash<QString, QString> m_lastSelected;
    QString m_pendingSelect;
    int m_lastRow = 0;
    QString m_search;
    bool m_searchBackwards = false;
    ClipboardFiles m_register;
    QString m_propertiesPath;

    // Fuzzy finder
    FileWalker m_walker;
    QStringList m_finderItems;
    QString m_finderRoot;
    bool m_finderTruncated = false;
    bool m_finderActive = false;
    qint64 m_quitArmedAt = 0;
    QTimer m_finderTimer;

    TaskManager m_tasks;
    KeyMap m_keys;
    CommandRegistry m_commands;
    QTimer m_previewTimer;
    QTimer m_whichKeyTimer;
    QTimer m_ambiguityTimer;
    bool m_loadUserConfig;
};
