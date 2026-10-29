#pragma once

#include <QHash>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <memory>
#include <optional>

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
    HeaderBar *headerBar() const { return m_header; }
    WhichKey *whichKey() const { return m_whichKey; }
    KeyMapSheet *keyMapSheet() const { return m_keySheet; }
    PropertiesShelf *propertiesShelf() const { return m_shelf; }
    bool isVisualMode() const { return m_visual; }
    TaskManager *tasks() { return &m_tasks; }
    Marks *marks() { return &m_marks; }
    KeyMap *keymap() { return &m_keys; }
    const ViewOptions &options() const { return m_options; }
    bool isIdle() const;

    bool runCommandLine(const QString &line, int count = 0);
    bool cd(const QString &path, const QString &selectName = {}, bool recordHistory = true);
    QStringList targets() const;

    int tabCount() const { return int(m_tabs.size()); }
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
    enum class Overlay { None, Tasks, KeyMap };

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
    void message(const QString &text, StatusBar::Level level = StatusBar::Level::Info);
    void error(const QString &text) { message(text, StatusBar::Level::Error); }

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
    void showKeyMap();
    void closeOverlay();
    void toggleProperties();
    void updateProperties();

    void yank(bool cut);
    void paste(Task::Kind kind);
    void startTask(Task::Kind kind, const QStringList &paths, const QString &target);
    void onTaskFinished(Task *task);
    void promptRename();
    void promptCreate(const QString &initial = {});
    void createPath(const QString &input);
    void confirm(const QString &title, const QString &meta, const QList<PickerItem> &items, const QString &body,
                 const QString &yes, std::function<void()> accept, std::function<void()> reject);
    void confirmDelete();
    void promptFilter();
    void promptSearch(bool backwards = false);
    void searchNext(int direction);
    QString searchPosition(const NameMatcher &matcher) const;
    void promptCommand(const QString &initial = {});
    void updateCommandList(const QString &text);
    QList<PickerItem> commandItems(const QString &query) const;
    void promptShell();
    void promptFind();
    void updateFinderList();
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
    QWidget *m_previewPane = nullptr;
    PropertiesShelf *m_shelf = nullptr;
    QSplitter *m_splitter = nullptr;
    HeaderBar *m_header = nullptr;
    QWidget *m_bottom = nullptr;
    QWidget *m_promptSpace = nullptr;
    StatusBar *m_status = nullptr;
    Prompt *m_prompt = nullptr;
    PickerList *m_picker = nullptr;
    Scrim *m_scrim = nullptr;
    WhichKey *m_whichKey = nullptr;
    TasksView *m_tasksView = nullptr;
    KeyMapSheet *m_keySheet = nullptr;
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
    bool m_searching = false; // the / bar is open
    ClipboardFiles m_register;

    // Properties shelf; folder sizes are counted on a worker thread.
    struct SizeCount {
        std::atomic_bool cancel = false;
        std::atomic_bool done = false;
        std::atomic<qint64> bytes = 0;
        std::atomic<int> children = -1;
    };
    bool m_propertiesOpen = false;
    // The targets the shelf shows; unset forces a refresh.
    std::optional<QString> m_propertiesKey;
    std::shared_ptr<SizeCount> m_sizeCount;
    QTimer m_sizeTimer;

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
    CommandRegistry m_commands;
    QTimer m_previewTimer;
    QTimer m_whichKeyTimer;
    QTimer m_ambiguityTimer;
    bool m_loadUserConfig;
};
