#pragma once

#include <QHash>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "chooser.h"
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

class QTemporaryDir;

// Device, inode and birth time: inode numbers alone get reused right away.
struct FileStamp {
    quint64 dev = 0;
    quint64 ino = 0;
    qint64 born = 0;
    bool exists() const { return ino != 0; }
    bool operator==(const FileStamp &) const = default;
};
FileStamp fileStamp(const QString &path);

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
    WhichKey *whichKey() const { return m_whichKey; }
    bool isVisualMode() const { return m_visual; }
    TaskManager *tasks() { return &m_tasks; }
    Marks *marks() { return &m_marks; }
    KeyMap *keymap() { return &m_keys; }
    // The bindings before scout.conf ran.
    const KeyMap &defaultKeys() const { return m_defaultKeys; }
    const CommandRegistry &commands() const { return m_commands; }
    const ViewOptions &options() const { return m_options; }
    bool isIdle() const;

    bool runCommandLine(const QString &line, int count = 0);
    bool cd(const QString &path, const QString &selectName = {}, bool recordHistory = true);
    QStringList targets() const;

    int tabCount() const { return int(m_tabs.size()); }
    // Reopens the tabs of the window closed last (see state.h), skipping
    // folders that are gone. False if nothing was saved.
    bool restoreSession();
    int currentTab() const { return m_tab; }
    int undoDepth() const { return int(m_undo.size()); }
    // Applies renames and records them for undo; returns false if anything failed.
    bool applyRenames(const QList<QPair<QString, QString>> &renames, bool recordUndo = true);
    // Bulk rename without an editor: `edit` receives the lines and returns the edited lines.
    void bulkRenameWith(const std::function<QStringList(const QStringList &)> &edit);

    // Turns this window into a file chooser. `done` runs exactly once, when the
    // user accepts or cancels, or the window closes.
    void setChooser(const ChooserRequest &request, std::function<void(const ChooserResult &)> done);
    bool isChooser() const { return m_chooserBar != nullptr; }
    ChooserBar *chooserBar() const { return m_chooserBar; }
    void finishChooser(bool accepted);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class Overlay { None, Tasks, Properties };

    struct Tab {
        QString cwd;
        QStringList back;
        QStringList forward;
        QHash<QString, QString> lastSelected;
        int lastRow = 0;
    };

    struct UndoStep {
        enum Kind { Moves, Renames, Restored } kind;
        QString description;
        QList<QPair<QString, QString>> pairs; // (original, current)
        // Identity of each current path when recorded; undo leaves alone
        // anything that has been replaced since.
        QList<FileStamp> ids;
    };

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
    // Errors and warnings also go to scout.log.
    void message(const QString &text, StatusBar::Level level = StatusBar::Level::Info);
    void error(const QString &text) { message(text, StatusBar::Level::Error); }

    void runFromKeys(const KeyMap::Result &result);
    // How a command was started, for the log: "quit (key <C-q>)".
    QString trigger(const QString &command) const;

    void setCursor(int row);
    void moveCursor(int delta);
    void enter();
    void leave();
    void openTargets();
    void openWith(const DesktopApp &app, const QStringList &paths);
    void showOpenWith();
    bool handleEscape();
    void handleOverlayKey(QKeyEvent *event);
    void showWhichKey();
    void hideWhichKey();
    void cancelPendingKeys();
    void startVisual();
    void endVisual();
    void updateVisual();
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
    void promptFind();
    void updateFinderList();
    void showHelp();
    void showContextMenu(int row, const QPoint &globalPos);
    void openPrompt(PromptSpec spec);

    void saveTab();
    void loadTab(int index);
    void newTab(const QString &path);
    void closeTab();
    void updateTabs();

    void pushUndo(UndoStep step);
    void undo();
    void restoreFromTrash();
    void confirmEmptyTrash();

    void bulkRename();
    void startBulkRename(const QStringList &paths);
    void finishBulkRename(const QStringList &paths, const QStringList &lines, const QString &baseDir);
    QString editorCommand() const;

    void chooserAccept(bool fromCursor);
    void chooserAcceptPaths(const QStringList &paths);
    void chooserSave(const QString &name);
    void updateChooserHint();

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

    // Visual mode: marks are the ones from before plus anchor..cursor.
    bool m_visual = false;
    QString m_visualAnchor;
    int m_visualAnchorRow = 0;
    QSet<QString> m_visualBase;

    // Fuzzy finder
    FileWalker m_walker;
    QStringList m_finderItems;
    QString m_finderRoot;
    bool m_finderTruncated = false;
    bool m_finderActive = false;
    qint64 m_quitArmedAt = 0;
    QString m_trigger;     // keys running the current command, if any
    QString m_closeReason; // logged when the window closes
    QTimer m_finderTimer;

    // Tabs
    QList<Tab> m_tabs;
    int m_tab = 0;
    QList<Tab> m_closedTabs;

    // Undo
    QList<UndoStep> m_undo;
    QSet<Task *> m_undoTasks;

    // Bulk rename
    std::unique_ptr<QTemporaryDir> m_renameDir;
    QStringList m_renamePaths;
    QTimer m_renamePoll;
    QString m_editor;

    // File chooser
    ChooserRequest m_chooser;
    ChooserBar *m_chooserBar = nullptr;
    std::function<void(const ChooserResult &)> m_chooserDone;

    TaskManager m_tasks;
    KeyMap m_keys;
    KeyMap m_defaultKeys;
    CommandRegistry m_commands;
    QTimer m_previewTimer;
    QTimer m_whichKeyTimer;
    QTimer m_ambiguityTimer;
    bool m_loadUserConfig;
    bool m_loadingConfig = false;
};
