#include "clipboard.h"
#include "commands.h"
#include "config.h"
#include "desktopentry.h"
#include "dirmodel.h"
#include "fileops.h"
#include "fuzzy.h"
#include "keymap.h"
#include "launcher.h"
#include "logging.h"
#include "mainwindow.h"
#include "service.h"
#include "startup.h"
#include "state.h"
#include "theme.h"

#include <QApplication>
#include <QDBusArgument>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <csignal>
#include <linux/landlock.h>
#include <random>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, const QByteArray &content = "hello\n") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

// From here on this process may only create, change or remove files below
// `root` (and write to /dev/null), and may not exec anything. False when the
// kernel has no Landlock.
QString readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// `scout_tests --crash-child <log>`: crashes with the handler installed.
[[noreturn]] void crashChild(const char *logPath) {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    logging::open(QString::fromLocal8Bit(logPath));
    logging::installCrashHandler();
    logging::info(QStringLiteral("about to crash"));
    raise(SIGSEGV);
    _exit(3);
}

bool confineWritesTo(const QString &root) {
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1)
        return false;
    __u64 write = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                  LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                  LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                  LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2)
        write |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3)
        write |= LANDLOCK_ACCESS_FS_TRUNCATE;
    landlock_ruleset_attr attr{};
    attr.handled_access_fs = write | LANDLOCK_ACCESS_FS_EXECUTE;
    const int ruleset = int(syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0));
    if (ruleset < 0)
        return false;
    auto allow = [ruleset](const char *path, __u64 access) {
        landlock_path_beneath_attr rule{};
        rule.allowed_access = access;
        rule.parent_fd = open(path, O_PATH | O_CLOEXEC);
        const bool ok = rule.parent_fd >= 0 &&
                        syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
        if (rule.parent_fd >= 0)
            close(rule.parent_fd);
        return ok;
    };
    const bool ok = allow(QFile::encodeName(root).constData(), write) &&
                    allow("/dev/null", LANDLOCK_ACCESS_FS_WRITE_FILE | (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0)) &&
                    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    close(ruleset);
    return ok;
}

} // namespace

class ScoutTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

    QString makeTree() {
        const QString base = m_root.path() + QStringLiteral("/tree-") + QString::number(++m_trees);
        QDir().mkpath(base + QStringLiteral("/alpha/inner"));
        QDir().mkpath(base + QStringLiteral("/beta"));
        writeFile(base + QStringLiteral("/alpha/inner/deep.txt"));
        writeFile(base + QStringLiteral("/alpha/one.md"), "# One\n");
        writeFile(base + QStringLiteral("/file10.txt"));
        writeFile(base + QStringLiteral("/file2.txt"));
        writeFile(base + QStringLiteral("/notes.md"), "# Notes\n\nsome text\n");
        writeFile(base + QStringLiteral("/.hidden"));
        return base;
    }

    static void keys(MainWindow &w, const QString &text) {
        for (QChar c : text) {
            QWidget *target = w.prompt()->isOpen() ? static_cast<QWidget *>(w.prompt()->edit()) : &w;
            QTest::keyClick(target, c.toLatin1());
        }
    }

    static void key(MainWindow &w, Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QWidget *target = w.prompt()->isOpen() ? static_cast<QWidget *>(w.prompt()->edit()) : &w;
        QTest::keyClick(target, k, mods);
    }

    static QString currentName(MainWindow &w) {
        const FileEntry *e = w.currentEntry();
        return e ? e->name : QString();
    }

    int m_trees = 0;
    QList<QStringList> m_launches;

private slots:
    void initTestCase() {
        QVERIFY(m_root.isValid());
        qputenv("XDG_DATA_HOME", (m_root.path() + QStringLiteral("/xdg-data")).toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (m_root.path() + QStringLiteral("/xdg-config")).toLocal8Bit());
        qputenv("XDG_STATE_HOME", (m_root.path() + QStringLiteral("/xdg-state")).toLocal8Bit());
        QDir().mkpath(m_root.path() + QStringLiteral("/xdg-data"));
        Theme::instance().init();
        // No test starts a real program: only stand-ins the tests write under m_root run.
        launcher::setHook([this](const QString &program, const QStringList &args, const QString &dir) {
            m_launches << (QStringList{program} + args);
            if (program.startsWith(m_root.path() + u'/'))
                return QProcess::startDetached(program, args, dir);
            return true;
        });
    }

    void cleanupTestCase() {
        for (const QStringList &argv : std::as_const(m_launches))
            qInfo("launch: %s", qPrintable(argv.join(u' ')));
    }

    void fuzzyRanksBasenameAndBoundaries() {
        QVERIFY(fuzzy::match(u"mw", u"src/mainwindow.cpp"));
        QVERIFY(!fuzzy::match(u"xyz", u"src/mainwindow.cpp"));
        fuzzy::Match a, b;
        QVERIFY(fuzzy::match(u"launch", u"Documents/notes/launch-plan.md", &a));
        QVERIFY(fuzzy::match(u"launch", u"lib/a/u/n/c/h/readme", &b));
        QVERIFY(a.score > b.score);
        QCOMPARE(a.positions.size(), 6);
        // Smartcase: an uppercase query only matches uppercase text.
        QVERIFY(!fuzzy::match(u"Notes", u"docs/notes.md"));
        QVERIFY(fuzzy::match(u"notes", u"docs/Notes.md"));
        fuzzy::Match exact, scattered;
        QVERIFY(fuzzy::match(u"main", u"src/main.zig", &exact));
        QVERIFY(fuzzy::match(u"main", u"src/my_animation.zig", &scattered));
        QVERIFY(exact.score > scattered.score);
    }

    void keymapSequencesAndCounts() {
        KeyMap km;
        km.bind(QStringLiteral("gg"), QStringLiteral("top"));
        km.bind(QStringLiteral("gh"), QStringLiteral("cd ~"), QStringLiteral("Home"));
        km.bind(QStringLiteral("j"), QStringLiteral("down"));
        km.bind(QStringLiteral("<C-d>"), QStringLiteral("half-page-down"));
        QCOMPARE(km.feed(QStringLiteral("g")).kind, KeyMap::Result::Pending);
        const auto conts = km.continuations();
        QCOMPARE(conts.size(), 2);
        QCOMPARE(conts[1].label, QStringLiteral("Home"));
        auto r = km.feed(QStringLiteral("g"));
        QCOMPARE(r.kind, KeyMap::Result::Command);
        QCOMPARE(r.command, QStringLiteral("top"));
        km.feed(QStringLiteral("1"));
        km.feed(QStringLiteral("2"));
        r = km.feed(QStringLiteral("j"));
        QCOMPARE(r.command, QStringLiteral("down"));
        QCOMPARE(r.count, 12);
        QCOMPARE(km.feed(QStringLiteral("x")).kind, KeyMap::Result::None);
        QCOMPARE(km.feed(QStringLiteral("<c-d>")).command, QStringLiteral("half-page-down"));
        QCOMPARE(KeyMap::normalizeToken(QStringLiteral("<S-x>")), QStringLiteral("X"));
        QCOMPARE(KeyMap::normalizeToken(QStringLiteral("<a-enter>")), QStringLiteral("<A-CR>"));
        QCOMPARE(KeyMap::normalizeToken(QStringLiteral("<C-D>")), QStringLiteral("<C-S-d>"));
        QCOMPARE(KeyMap::parseSequence(QStringLiteral("d<Space>x")),
                 (QStringList{QStringLiteral("d"), QStringLiteral("<Space>"), QStringLiteral("x")}));
    }

    void keyEventTokens() {
        QKeyEvent ctrlD(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier, QStringLiteral("\x04"));
        QCOMPARE(KeyMap::tokenFor(&ctrlD), QStringLiteral("<C-d>"));
        QKeyEvent shiftG(QEvent::KeyPress, Qt::Key_G, Qt::ShiftModifier, QStringLiteral("G"));
        QCOMPARE(KeyMap::tokenFor(&shiftG), QStringLiteral("G"));
        QKeyEvent altEnter(QEvent::KeyPress, Qt::Key_Return, Qt::AltModifier, QStringLiteral("\r"));
        QCOMPARE(KeyMap::tokenFor(&altEnter), QStringLiteral("<A-CR>"));
        QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
        QCOMPARE(KeyMap::tokenFor(&space), QStringLiteral("<Space>"));
        QKeyEvent colon(QEvent::KeyPress, Qt::Key_Colon, Qt::ShiftModifier, QStringLiteral(":"));
        QCOMPARE(KeyMap::tokenFor(&colon), QStringLiteral(":"));
        QKeyEvent ctrlTab(QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(KeyMap::tokenFor(&ctrlTab), QStringLiteral("<C-Tab>"));
        QKeyEvent ctrlShiftTab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(KeyMap::tokenFor(&ctrlShiftTab), QStringLiteral("<C-S-Tab>"));
        QKeyEvent shiftTab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(KeyMap::tokenFor(&shiftTab), QStringLiteral("<S-Tab>"));
        QKeyEvent alt3(QEvent::KeyPress, Qt::Key_3, Qt::AltModifier, QStringLiteral("3"));
        QCOMPARE(KeyMap::tokenFor(&alt3), QStringLiteral("<A-3>"));
    }

    void naturalSortAndSizes() {
        QVERIFY(naturalCompare(u"file2.txt", u"file10.txt") < 0);
        QVERIFY(naturalCompare(u"Alpha", u"beta") < 0);
        QVERIFY(naturalCompare(u"a01", u"a1") != 0);
        QCOMPARE(humanSize(28), QStringLiteral("28 B"));
        QCOMPARE(humanSize(8000), QStringLiteral("7.8 K"));
        QCOMPARE(humanSize(352256), QStringLiteral("344 K"));
    }

    void commandLineSplitting() {
        QCOMPARE(CommandRegistry::split(QStringLiteral("cd \"my dir\"")),
                 (QStringList{QStringLiteral("cd"), QStringLiteral("my dir")}));
        QCOMPARE(CommandRegistry::split(QStringLiteral("map gw 'cd ~/work'")),
                 (QStringList{QStringLiteral("map"), QStringLiteral("gw"), QStringLiteral("cd ~/work")}));
        const QString tricky = QStringLiteral("it's here");
        QCOMPARE(CommandRegistry::split(QStringLiteral("x ") + CommandRegistry::quote(tricky)).value(1), tricky);
    }

    void clipboardEncoding() {
        std::unique_ptr<QMimeData> data(makeFileMimeData({QStringLiteral("/tmp/a b"), QStringLiteral("/tmp/c")}, true));
        QCOMPARE(data->urls().size(), 2);
        QVERIFY(data->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut\nfile:///tmp/a%20b"));
        const ClipboardFiles parsed = parseFileMimeData(data.get());
        QVERIFY(parsed.cut);
        QCOMPARE(parsed.paths, (QStringList{QStringLiteral("/tmp/a b"), QStringLiteral("/tmp/c")}));
    }

    void desktopExecExpansion() {
        QCOMPARE(DesktopApps::splitExec(QStringLiteral("foo --title \"a b\" %F")),
                 (QStringList{QStringLiteral("foo"), QStringLiteral("--title"), QStringLiteral("a b"), QStringLiteral("%F")}));
        DesktopApp multi{QStringLiteral("m.desktop"), QStringLiteral("M"), QStringLiteral("m %F"), {}, {}, false, {}};
        QCOMPARE(DesktopApps::commandLines(multi, {QStringLiteral("/a"), QStringLiteral("/b")}).size(), 1);
        DesktopApp single{QStringLiteral("s.desktop"), QStringLiteral("S"), QStringLiteral("s --open=%u"), {}, {}, false, {}};
        const auto lines = DesktopApps::commandLines(single, {QStringLiteral("/a"), QStringLiteral("/b")});
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], (QStringList{QStringLiteral("s"), QStringLiteral("--open=file:///a")}));
        DesktopApp bare{QStringLiteral("b.desktop"), QStringLiteral("B"), QStringLiteral("b"), {}, {}, false, {}};
        QCOMPARE(DesktopApps::commandLines(bare, {QStringLiteral("/a")}).first(),
                 (QStringList{QStringLiteral("b"), QStringLiteral("/a")}));
    }

    void fileOperations() {
        const QString base = makeTree();
        const QString dest = base + QStringLiteral("/beta");
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);

        Task *copy = tasks.start(Task::Copy, {base + QStringLiteral("/alpha"), base + QStringLiteral("/notes.md")}, dest);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(copy->progress().state, Task::Done);
        QVERIFY(QFile::exists(dest + QStringLiteral("/alpha/inner/deep.txt")));
        QVERIFY(QFile::exists(dest + QStringLiteral("/notes.md")));

        // Copying again never overwrites.
        tasks.start(Task::Copy, {base + QStringLiteral("/notes.md")}, dest);
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(QFile::exists(dest + QStringLiteral("/notes (2).md")));

        Task *intoSelf = tasks.start(Task::Copy, {base + QStringLiteral("/alpha")}, base + QStringLiteral("/alpha/inner"));
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(intoSelf->progress().state, Task::Failed);

        tasks.start(Task::Move, {base + QStringLiteral("/file2.txt")}, dest);
        QTRY_COMPARE(finished.size(), 4);
        QVERIFY(!QFile::exists(base + QStringLiteral("/file2.txt")));
        QVERIFY(QFile::exists(dest + QStringLiteral("/file2.txt")));

        Task *trash = tasks.start(Task::Trash, {base + QStringLiteral("/file10.txt")});
        QTRY_COMPARE(finished.size(), 5);
        QCOMPARE(trash->progress().state, Task::Done);
        QVERIFY(!QFile::exists(base + QStringLiteral("/file10.txt")));

        tasks.start(Task::Delete, {dest + QStringLiteral("/alpha")});
        QTRY_COMPARE(finished.size(), 6);
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/alpha")));
    }

    void navigationAndPreview() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base);
        QCOMPARE(currentName(w), QStringLiteral("alpha"));
        QCOMPARE(w.preview()->kind(), Preview::Kind::Directory);
        QCOMPARE(w.currentModel()->rowCount(), 5); // .hidden is hidden

        keys(w, QStringLiteral("j"));
        QCOMPARE(currentName(w), QStringLiteral("beta"));
        keys(w, QStringLiteral("G"));
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.preview()->kind(), Preview::Kind::Text);
        QVERIFY(w.preview()->text().contains(QStringLiteral("some text")));
        keys(w, QStringLiteral("gg"));
        QCOMPARE(currentName(w), QStringLiteral("alpha"));
        keys(w, QStringLiteral("3j"));
        QCOMPARE(currentName(w), QStringLiteral("file10.txt"));
        keys(w, QStringLiteral("k"));
        QCOMPARE(currentName(w), QStringLiteral("file2.txt")); // natural order

        keys(w, QStringLiteral("ggl"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/alpha"));
        keys(w, QStringLiteral("h"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base);
        QCOMPARE(currentName(w), QStringLiteral("alpha"));

        keys(w, QStringLiteral("zh"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.currentModel()->rowCount(), 6);

        keys(w, QStringLiteral("H"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/alpha"));
    }

    void copyPasteRenameCreate() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());

        keys(w, QStringLiteral("G"));
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));
        keys(w, QStringLiteral("yy"));
        QVERIFY(w.statusBar()->message().contains(QStringLiteral("copied")));
        keys(w, QStringLiteral("ggjl")); // into beta
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/beta"));
        keys(w, QStringLiteral("pp"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/beta/notes.md")));
        QTRY_COMPARE(currentName(w), QStringLiteral("notes.md"));

        keys(w, QStringLiteral("r"));
        QVERIFY(w.prompt()->isOpen());
        QCOMPARE(w.prompt()->edit()->selectedText(), QStringLiteral("notes"));
        keys(w, QStringLiteral("renamed"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/beta/renamed.md")));
        QTRY_COMPARE(currentName(w), QStringLiteral("renamed.md"));

        keys(w, QStringLiteral("a"));
        keys(w, QStringLiteral("sub/"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(QFileInfo(base + QStringLiteral("/beta/sub")).isDir());
        QTRY_COMPARE(currentName(w), QStringLiteral("sub"));

        // Marks + cut + paste moves several files.
        keys(w, QStringLiteral("gg"));
        key(w, Qt::Key_Space);
        key(w, Qt::Key_Space);
        QCOMPARE(w.marks()->size(), 2);
        keys(w, QStringLiteral("dd"));
        QCOMPARE(w.marks()->size(), 0);
        keys(w, QStringLiteral("h"));
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("pp"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/renamed.md")));
        QTRY_VERIFY(QFileInfo(base + QStringLiteral("/sub")).isDir());
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/beta/sub")));

        // Trash
        QTRY_VERIFY(w.isIdle());
        QTRY_COMPARE(w.currentModel()->rowOf(QStringLiteral("renamed.md")) >= 0, true);
        w.runCommandLine(QStringLiteral("cd ") + base, 0);
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("/renamed"));
        key(w, Qt::Key_Return);
        QCOMPARE(currentName(w), QStringLiteral("renamed.md"));
        keys(w, QStringLiteral("dD"));
        QTRY_VERIFY(!QFile::exists(base + QStringLiteral("/renamed.md")));
    }

    void filterFindAndCommands() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());

        keys(w, QStringLiteral("zf"));
        keys(w, QStringLiteral("file"));
        QTRY_COMPARE(w.currentModel()->rowCount(), 2);
        key(w, Qt::Key_Return);
        QCOMPARE(w.currentModel()->filter(), QStringLiteral("file"));
        key(w, Qt::Key_Escape);
        QTRY_COMPARE(w.currentModel()->rowCount(), 5);

        keys(w, QStringLiteral("gf"));
        QVERIFY(w.prompt()->isOpen());
        keys(w, QStringLiteral("deep"));
        QTRY_VERIFY(!w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->items().first().text, QStringLiteral("alpha/inner/deep.txt"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/alpha/inner"));
        QTRY_COMPARE(currentName(w), QStringLiteral("deep.txt"));

        keys(w, QStringLiteral(":"));
        keys(w, QStringLiteral("mkdir made"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(QFileInfo(base + QStringLiteral("/alpha/inner/made")).isDir());

        keys(w, QStringLiteral(":"));
        keys(w, QStringLiteral("hidd"));
        QTRY_VERIFY(!w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->items().first().text, QStringLiteral("toggle-hidden"));
        key(w, Qt::Key_Return);
        QVERIFY(w.options().showHidden);

        QVERIFY(w.runCommandLine(QStringLiteral("map gw 'cd ") + base + QStringLiteral("/beta'")));
        keys(w, QStringLiteral("gw"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/beta"));
    }

    void themeSwitchRepaintsColumns() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.resize(900, 500);
        w.show();
        QTRY_VERIFY(w.isIdle());
        auto backgroundLightness = [&] {
            const QImage img = w.currentView()->viewport()->grab().toImage();
            return img.pixelColor(img.width() - 5, img.height() - 5).lightnessF();
        };
        QVERIFY(w.runCommandLine(QStringLiteral("set theme light")));
        QCoreApplication::processEvents();
        QVERIFY2(backgroundLightness() > 0.7, qPrintable(QString::number(backgroundLightness())));
        QVERIFY(w.runCommandLine(QStringLiteral("set theme dark")));
        QCoreApplication::processEvents();
        QVERIFY(backgroundLightness() < 0.3);
    }

    void deleteNeverFollowsSymlinks() {
        const QString base = makeTree();
        writeFile(base + QStringLiteral("/victim/keep.txt"));
        QDir().mkpath(base + QStringLiteral("/doomed"));
        QVERIFY(QFile::link(base + QStringLiteral("/victim"), base + QStringLiteral("/doomed/link")));
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Delete, {base + QStringLiteral("/doomed")});
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(t->progress().state, Task::Done);
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/doomed")));
        QVERIFY(QFile::exists(base + QStringLiteral("/victim/keep.txt")));
    }

    void symlinkAliasCannotCopyIntoItself() {
        const QString base = makeTree();
        QVERIFY(QFile::link(base + QStringLiteral("/alpha"), base + QStringLiteral("/alias")));
        QVERIFY(isSameOrInside(base + QStringLiteral("/alias/inner"), base + QStringLiteral("/alpha")));
        QVERIFY(!isSameOrInside(base + QStringLiteral("/beta"), base + QStringLiteral("/alpha")));
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Copy, {base + QStringLiteral("/alpha")}, base + QStringLiteral("/alias/inner"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(t->progress().state, Task::Failed);
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/alpha/inner/alpha")));
    }

    void renamesNeverReplace() {
        const QString base = makeTree();
        const QString a = base + QStringLiteral("/file2.txt");
        const QString b = base + QStringLiteral("/file10.txt");
        QCOMPARE(renameNoReplace(a, b), EEXIST);
        QVERIFY(QFile::exists(a));
        QVERIFY(QFile::link(base + QStringLiteral("/missing"), base + QStringLiteral("/dangling")));
        QCOMPARE(renameNoReplace(a, base + QStringLiteral("/dangling")), EEXIST);
        QCOMPARE(renameNoReplace(a, base + QStringLiteral("/fresh.txt")), 0);
        QCOMPARE(renameByReservation(base + QStringLiteral("/fresh.txt"), b), EEXIST);
        QCOMPARE(renameByReservation(base + QStringLiteral("/fresh.txt"), base + QStringLiteral("/dangling")), EEXIST);
        QCOMPARE(renameByReservation(base + QStringLiteral("/alpha"), base + QStringLiteral("/beta")), EEXIST);
        QVERIFY(QFileInfo(base + QStringLiteral("/alpha/one.md")).exists());
        QCOMPARE(renameByReservation(base + QStringLiteral("/alpha"), base + QStringLiteral("/gamma")), 0);
        QVERIFY(QFileInfo(base + QStringLiteral("/gamma/one.md")).exists());
        QCOMPARE(renameByReservation(base + QStringLiteral("/fresh.txt"), base + QStringLiteral("/fresh2.txt")), 0);
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/fresh.txt")));
        QCOMPARE(renameByReservation(base + QStringLiteral("/gamma"), base + QStringLiteral("/alpha")), 0);

        // Moving onto an existing name keeps both files.
        writeFile(base + QStringLiteral("/beta/notes.md"), "original\n");
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        tasks.start(Task::Move, {base + QStringLiteral("/notes.md")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(finished.size(), 1);
        QFile original(base + QStringLiteral("/beta/notes.md"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original\n"));
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/notes (2).md")));
    }

    void failedCopyLeavesNothingBehind() {
        if (geteuid() == 0)
            QSKIP("root can read unreadable files");
        const QString base = makeTree();
        writeFile(base + QStringLiteral("/src/ok.txt"));
        writeFile(base + QStringLiteral("/src/secret.txt"));
        QVERIFY(chmod(QFile::encodeName(base + QStringLiteral("/src/secret.txt")).constData(), 0) == 0);
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Copy, {base + QStringLiteral("/src")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(t->progress().state, Task::Failed);
        QCOMPARE(QDir(base + QStringLiteral("/beta")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList());
    }

    void mimeRemovalOnlyAffectsLowerLevels() {
        const QString apps = m_root.path() + QStringLiteral("/xdg-data/applications");
        QDir().mkpath(apps);
        for (const char *id : {"scout-test-a", "scout-test-b"})
            writeFile(apps + QStringLiteral("/%1.desktop").arg(QLatin1String(id)),
                      QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nExec=%1 %f\nMimeType=text/x-scouttest;\n")
                          .arg(QLatin1String(id))
                          .toUtf8());
        const QString sysData = m_root.path() + QStringLiteral("/sys-data");
        writeFile(sysData + QStringLiteral("/applications/mimeapps.list"),
                  "[Removed Associations]\ntext/x-scouttest=scout-test-a.desktop;\n");
        writeFile(m_root.path() + QStringLiteral("/xdg-config/mimeapps.list"),
                  "[Default Applications]\ntext/x-scouttest=scout-test-b.desktop;\n"
                  "[Added Associations]\ntext/x-scouttest=scout-test-a.desktop;\n");
        qputenv("XDG_DATA_DIRS", sysData.toLocal8Bit());
        DesktopApps::instance().reload();
        const auto found = DesktopApps::instance().appsFor({QStringLiteral("text/x-scouttest")});
        QCOMPARE(found.size(), 2);
        QCOMPARE(found[0].id, QStringLiteral("scout-test-b.desktop"));
        QCOMPARE(found[1].id, QStringLiteral("scout-test-a.desktop"));
    }

    void filterKeepsRememberedCursor() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("G"));
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));
        keys(w, QStringLiteral("zffile"));
        key(w, Qt::Key_Return);
        QTRY_COMPARE(w.currentModel()->rowCount(), 2);
        key(w, Qt::Key_Escape);
        QTRY_COMPARE(w.currentModel()->rowCount(), 5);
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));
    }

    void crossDeviceMoveCopiesThenRemoves() {
        struct stat shm {}, root {};
        if (stat("/dev/shm", &shm) != 0 || stat(QFile::encodeName(m_root.path()).constData(), &root) != 0 ||
            shm.st_dev == root.st_dev || access("/dev/shm", W_OK) != 0)
            QSKIP("needs /dev/shm on a separate, writable filesystem");
        QTemporaryDir other(QStringLiteral("/dev/shm/scout-test-XXXXXX"));
        QVERIFY(other.isValid());
        writeFile(other.path() + QStringLiteral("/pkg/a/one.txt"), "one\n");
        writeFile(other.path() + QStringLiteral("/pkg/two.txt"), "two\n");
        const QString base = makeTree();
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Move, {other.path() + QStringLiteral("/pkg")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(t->progress().state, Task::Done);
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/pkg/a/one.txt")));
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/pkg/two.txt")));
        QVERIFY(!QFileInfo::exists(other.path() + QStringLiteral("/pkg")));
        QCOMPARE(QDir(base + QStringLiteral("/beta")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList{QStringLiteral("pkg")});
    }

    void unreadableFolderCopyCleansUp() {
        if (geteuid() == 0)
            QSKIP("root can read unreadable folders");
        const QString base = makeTree();
        writeFile(base + QStringLiteral("/src/ok.txt"));
        QDir().mkpath(base + QStringLiteral("/src/locked"));
        QVERIFY(chmod(QFile::encodeName(base + QStringLiteral("/src/locked")).constData(), 0) == 0);
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Copy, {base + QStringLiteral("/src")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(finished.size(), 1);
        chmod(QFile::encodeName(base + QStringLiteral("/src/locked")).constData(), 0700);
        QCOMPARE(t->progress().state, Task::Failed);
        QCOMPARE(QDir(base + QStringLiteral("/beta")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 QStringList());
    }

    void dropCopiesIntoFolder() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        // A drop onto a folder row is a copy unless the drag came from scout.
        emit w.currentView()->dropRequested({base + QStringLiteral("/notes.md")}, base + QStringLiteral("/beta"),
                                            Qt::CopyAction);
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/beta/notes.md")));
        QVERIFY(QFile::exists(base + QStringLiteral("/notes.md")));
    }
    void tabsKeepTheirOwnPlace() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("G"));
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));

        w.runCommandLine(QStringLiteral("new-tab ") + base + QStringLiteral("/alpha"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.tabCount(), 2);
        QCOMPARE(w.currentTab(), 1);
        QCOMPARE(w.cwd(), base + QStringLiteral("/alpha"));

        keys(w, QStringLiteral("gt"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base);
        QTRY_COMPARE(currentName(w), QStringLiteral("notes.md"));

        // A second tab in the same folder still restores its own cursor.
        w.runCommandLine(QStringLiteral("new-tab"));
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("gg"));
        QCOMPARE(w.tabCount(), 3);
        w.runCommandLine(QStringLiteral("tab 1"));
        QTRY_VERIFY(w.isIdle());
        QTRY_COMPARE(currentName(w), QStringLiteral("notes.md"));

        keys(w, QStringLiteral("q"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.tabCount(), 2);
        QVERIFY(w.isVisible());
        keys(w, QStringLiteral("uq"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.tabCount(), 3);
        QCOMPARE(w.cwd(), base);
    }

    void whichKeySkipsCountsAndFitsSmallWindows() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.resize(500, 300);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTRY_VERIFY(w.isIdle());

        keys(w, QStringLiteral("12"));
        QVERIFY(w.keymap()->isPending());
        QTest::qWait(500);
        QVERIFY(!w.whichKey()->isVisible());
        key(w, Qt::Key_Escape);
        QVERIFY(!w.keymap()->isPending());

        keys(w, QStringLiteral("g"));
        QTRY_VERIFY(w.whichKey()->isVisible());
        const QRect popup = w.whichKey()->geometry();
        QVERIFY2(w.rect().contains(popup), qPrintable(QStringLiteral("popup %1,%2 %3x%4 in %5x%6")
                                                          .arg(popup.x()).arg(popup.y()).arg(popup.width())
                                                          .arg(popup.height()).arg(w.width()).arg(w.height())));
        QVERIFY(popup.top() > w.statusBar()->height());
        QVERIFY(popup.bottom() < w.height() - w.statusBar()->height());
        QVERIFY(w.whichKey()->hiddenCount() > 0);

        // Any click drops the half-typed sequence, and Esc still reaches the window afterwards.
        QTest::mouseClick(w.currentView()->viewport(), Qt::LeftButton, {},
                          w.currentView()->visualRect(w.currentModel()->index(1)).center());
        QVERIFY(!w.whichKey()->isVisible());
        QVERIFY(!w.keymap()->isPending());
        QCOMPARE(currentName(w), QStringLiteral("beta"));
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QCOMPARE(QApplication::focusWidget(), &w);
        keys(w, QStringLiteral("5g"));
        QTRY_VERIFY(w.whichKey()->isVisible());
        key(w, Qt::Key_Escape);
        QVERIFY(!w.whichKey()->isVisible());
        QVERIFY(!w.keymap()->isPending());

        w.resize(1200, 760);
        keys(w, QStringLiteral("g"));
        QTRY_VERIFY(w.whichKey()->isVisible());
        QVERIFY(w.rect().contains(w.whichKey()->geometry()));
        QCOMPARE(w.whichKey()->hiddenCount(), 0);
        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&w, &deactivate);
        QVERIFY(!w.whichKey()->isVisible());
        QVERIFY(!w.keymap()->isPending());
    }

    void visualModeMarksARange() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        auto marked = [&](const char *name) { return w.marks()->contains(base + u'/' + QLatin1String(name)); };

        keys(w, QStringLiteral("vjj"));
        QVERIFY(w.isVisualMode());
        QCOMPARE(w.statusBar()->mode(), QStringLiteral("VISUAL"));
        QCOMPARE(w.marks()->size(), 3);
        QVERIFY(marked("alpha") && marked("beta") && marked("file2.txt"));
        keys(w, QStringLiteral("k"));
        QCOMPARE(w.marks()->size(), 2);
        QVERIFY(!marked("file2.txt"));
        key(w, Qt::Key_Escape);
        QVERIFY(!w.isVisualMode());
        QVERIFY(w.statusBar()->mode().isEmpty());
        QCOMPARE(w.marks()->size(), 2);
        key(w, Qt::Key_Escape);
        QCOMPARE(w.marks()->size(), 0);

        // Marks from before stay put while the range grows and shrinks.
        keys(w, QStringLiteral("G"));
        key(w, Qt::Key_Space);
        keys(w, QStringLiteral("ggvj"));
        QCOMPARE(w.marks()->size(), 3);
        keys(w, QStringLiteral("k"));
        QCOMPARE(w.marks()->size(), 2);
        QVERIFY(marked("alpha") && marked("notes.md"));
        QTest::mouseClick(w.currentView()->viewport(), Qt::LeftButton, {},
                          w.currentView()->visualRect(w.currentModel()->index(2)).center());
        QCOMPARE(currentName(w), QStringLiteral("file2.txt"));
        QCOMPARE(w.marks()->size(), 4);
        keys(w, QStringLiteral("V"));
        QVERIFY(!w.isVisualMode());
        QCOMPARE(w.marks()->size(), 4);
        key(w, Qt::Key_Escape);

        // A command on the targets leaves visual mode and acts on the range.
        keys(w, QStringLiteral("ggjvj"));
        keys(w, QStringLiteral("yy"));
        QVERIFY(!w.isVisualMode());
        QCOMPARE(w.statusBar()->message(), QStringLiteral("copied 2 items"));
        QCOMPARE(clipboardFiles().paths, (QStringList{base + QStringLiteral("/beta"), base + QStringLiteral("/file2.txt")}));

        // Leaving the folder ends visual mode and keeps the marks.
        keys(w, QStringLiteral("ggvj"));
        keys(w, QStringLiteral("l"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/beta"));
        QVERIFY(!w.isVisualMode());
        QCOMPARE(w.marks()->size(), 2);
        keys(w, QStringLiteral("h"));
        QTRY_VERIFY(w.isIdle());
        key(w, Qt::Key_Escape);
        QCOMPARE(w.marks()->size(), 0);

        keys(w, QStringLiteral("gg"));
        key(w, Qt::Key_Space);
        keys(w, QStringLiteral("*"));
        QCOMPARE(w.marks()->size(), 4);
        QVERIFY(!marked("alpha"));
        key(w, Qt::Key_Escape);

        // Right-clicking another row ends visual mode with the range it had.
        auto closeMenuSoon = [&w] {
            QTimer::singleShot(0, &w, [] {
                for (QWidget *top : QApplication::topLevelWidgets())
                    if (auto *menu = qobject_cast<QMenu *>(top); menu && menu->isVisible())
                        menu->close();
            });
        };
        const QPoint fourth = w.currentView()->visualRect(w.currentModel()->index(3)).center();
        keys(w, QStringLiteral("ggvj"));
        const QSet<QString> range = w.marks()->pathSet();
        closeMenuSoon();
        QTest::mouseClick(w.currentView()->viewport(), Qt::RightButton, {}, fourth);
        QVERIFY(!w.isVisualMode());
        QCOMPARE(w.marks()->pathSet(), range);
        keys(w, QStringLiteral("ggvj"));
        closeMenuSoon();
        emit w.currentView()->customContextMenuRequested(fourth);
        QVERIFY(!w.isVisualMode());
        QCOMPARE(currentName(w), QStringLiteral("file10.txt"));
        QCOMPARE(w.marks()->pathSet(), range);
        key(w, Qt::Key_Escape);

        // Switching tabs clears the filter, which must not stretch the range over the rows it hid.
        keys(w, QStringLiteral("G"));
        w.runCommandLine(QStringLiteral("new-tab"));
        QTRY_VERIFY(w.isIdle());
        w.currentModel()->setFilter(QStringLiteral("l"));
        keys(w, QStringLiteral("ggvj"));
        QVERIFY(marked("alpha") && marked("file2.txt"));
        emit w.findChild<HeaderBar *>()->tabClicked(0);
        QTRY_VERIFY(w.isIdle());
        QVERIFY(!w.isVisualMode());
        QCOMPARE(w.marks()->size(), 2);
        QVERIFY(!marked("beta"));
    }

    void visualModeSurvivesRandomUse() {
        // Runs in a child confined to its own temporary folder, so random
        // navigation can never act on anything outside it.
        if (!qEnvironmentVariableIsSet("SCOUT_STRESS_CHILD")) {
            QProcess child;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            const QString childTmp = m_root.path() + QStringLiteral("/stress-child");
            QVERIFY(QDir().mkpath(childTmp));
            env.insert(QStringLiteral("SCOUT_STRESS_CHILD"), QStringLiteral("1"));
            // The child's own temporary folder, which is all it may write to.
            env.insert(QStringLiteral("TMPDIR"), childTmp);
            child.setProcessEnvironment(env);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("visualModeSurvivesRandomUse")});
            QVERIFY(child.waitForFinished(10 * 60 * 1000));
            const QString output = QString::fromLocal8Bit(child.readAll());
            for (const QString &line : output.split(u'\n'))
                if (line.contains(QLatin1String("visual-mode checks")) || line.contains(QLatin1String("SKIP")))
                    qInfo("child: %s", qPrintable(line.trimmed()));
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, qPrintable(output));
            if (output.contains(QLatin1String("SKIP   : ScoutTest::visualModeSurvivesRandomUse")))
                QSKIP("Landlock is unavailable");
            return;
        }
        if (!confineWritesTo(QDir::tempPath()))
            QSKIP("Landlock is unavailable, and this test only runs confined");
        const QString outside = QFileInfo(QDir::tempPath()).absolutePath() + QStringLiteral("/scout-landlock-probe");
        const int probe = ::open(QFile::encodeName(outside).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (probe >= 0) {
            ::close(probe);
            ::unlink(QFile::encodeName(outside).constData());
        }
        QVERIFY2(probe < 0 && errno == EACCES, "Landlock did not confine the test");

        const QString base = m_root.path() + QStringLiteral("/stress");
        for (int i = 0; i < 60; ++i)
            writeFile(base + QStringLiteral("/f%1.txt").arg(i));
        for (int d = 0; d < 4; ++d)
            for (int i = 0; i < 15; ++i)
                writeFile(base + QStringLiteral("/dir%1/g%2").arg(d).arg(i));
        MainWindow w(base, false);
        w.resize(900, 500);
        w.show();
        QTRY_VERIFY(w.isIdle());

        bool seeded = false;
        const int seed = qEnvironmentVariableIntValue("SCOUT_STRESS_SEED", &seeded);
        std::mt19937 rng(seeded ? seed : 20261028);
        auto pick = [&rng](int n) { return int(std::uniform_int_distribution<int>(0, n - 1)(rng)); };
        static const char *motions[] = {"j", "k", "G", "gg", "5j", "3k", "12j", "2G"};
        int created = 0;
        int visualChecks = 0;
        QSignalSpy refreshes(w.currentModel(), &DirModel::refreshed);
        auto touchFiles = [&] {
            const QString dir = base + (pick(2) ? QString() : QStringLiteral("/dir%1").arg(pick(4)));
            if (pick(2)) {
                writeFile(dir + QStringLiteral("/new%1").arg(++created));
                return;
            }
            QString err;
            const QVector<FileEntry> entries = listDirectory(dir, &err);
            const FileEntry *victim = entries.isEmpty() ? nullptr : &entries[pick(int(entries.size()))];
            if (victim && !victim->isDir)
                QFile::remove(victim->path);
        };
        // Things that end visual mode, kept rarer so most steps run inside it.
        auto leaveVisual = [&] {
            switch (pick(8)) {
            case 0: key(w, Qt::Key_Space); break;
            case 1: {
                // Never above `base`, and never `l` on a file, which would launch its app.
                const FileEntry *e = w.currentEntry();
                if (e && e->isDir && (pick(3) || w.cwd() == base))
                    keys(w, QStringLiteral("l"));
                else if (w.cwd() != base)
                    keys(w, QStringLiteral("h"));
                break;
            }
            case 2: keys(w, QStringLiteral("H")); break;
            case 3:
                if (w.tabCount() < 4 && pick(2))
                    w.runCommandLine(QStringLiteral("new-tab"));
                else if (w.tabCount() > 1 && pick(2))
                    w.runCommandLine(QStringLiteral("close-tab"));
                else
                    w.runCommandLine(QStringLiteral("next-tab"));
                break;
            case 4: w.runCommandLine(pick(2) ? QStringLiteral("invert-marks") : QStringLiteral("unmark-all")); break;
            case 5: key(w, Qt::Key_Escape); break;
            case 6: w.runCommandLine(QStringLiteral("reload")); break;
            default: keys(w, QStringLiteral("V")); break;
            }
        };
        for (int step = 0; step < 3000; ++step) {
            switch (pick(20)) {
            case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
                keys(w, QLatin1String(motions[pick(int(std::size(motions)))]));
                break;
            case 8:
                key(w, pick(2) ? Qt::Key_D : Qt::Key_U, Qt::ControlModifier);
                break;
            case 9: case 10:
                if (!w.isVisualMode())
                    keys(w, pick(2) ? QStringLiteral("v") : QStringLiteral("V"));
                break;
            case 11:
                w.currentModel()->setFilter(pick(2) ? QString() : QString::number(pick(10)));
                break;
            case 12:
                // What the folder watcher does when something changes.
                w.currentModel()->reload();
                break;
            case 13: case 14:
                touchFiles();
                break;
            case 15:
                leaveVisual();
                break;
            default:
                QTest::qWait(pick(25));
                break;
            }
            QCoreApplication::processEvents();
            QVERIFY2(w.cwd() == base || w.cwd().startsWith(base + u'/'), qPrintable(w.cwd()));
            QVERIFY(w.isVisible());
            QVERIFY(w.tabCount() >= 1);
            QVERIFY(w.cursorRow() < w.currentModel()->rowCount());
            DirModel *model = w.currentModel();
            if (w.isVisualMode() && model->isLoaded() && !model->isLoading() && model->path() == w.cwd()) {
                if (const FileEntry *e = w.currentEntry()) {
                    QVERIFY2(w.marks()->contains(e->path), qPrintable(QStringLiteral("step %1: %2").arg(step).arg(e->path)));
                    ++visualChecks;
                }
            }
        }
        qInfo("visual-mode checks: %d, listing refreshes: %d", visualChecks, int(refreshes.size()));
        QVERIFY(visualChecks > 300);
        QVERIFY(refreshes.size() > 100);
        QVERIFY(m_launches.isEmpty());
    }

    void undoTrashMoveRenameAndCopy() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());

        w.cd(base + QStringLiteral("/notes.md"));
        QTRY_COMPARE(currentName(w), QStringLiteral("notes.md"));
        keys(w, QStringLiteral("dD"));
        QTRY_VERIFY(!QFile::exists(base + QStringLiteral("/notes.md")));
        QTRY_COMPARE(w.undoDepth(), 1);
        const QString trashFiles = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME")) + QStringLiteral("/Trash/files");
        const QString trashInfo = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME")) + QStringLiteral("/Trash/info");
        const int infosBefore = int(QDir(trashInfo).entryList(QDir::Files).size());
        keys(w, QStringLiteral("uu"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/notes.md")));
        QTRY_COMPARE(int(QDir(trashInfo).entryList(QDir::Files).size()), infosBefore - 1);
        QCOMPARE(w.undoDepth(), 0);

        // Move, then undo puts it back.
        w.tasks()->start(Task::Move, {base + QStringLiteral("/file2.txt")}, base + QStringLiteral("/beta"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/beta/file2.txt")));
        QTRY_COMPARE(w.undoDepth(), 1);
        w.runCommandLine(QStringLiteral("undo"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/file2.txt")));
        QVERIFY(!QFile::exists(base + QStringLiteral("/beta/file2.txt")));

        // Rename, then undo.
        QTRY_VERIFY(w.isIdle());
        w.cd(base + QStringLiteral("/file10.txt"));
        QTRY_COMPARE(currentName(w), QStringLiteral("file10.txt"));
        w.runCommandLine(QStringLiteral("rename ten.txt"));
        QVERIFY(QFile::exists(base + QStringLiteral("/ten.txt")));
        keys(w, QStringLiteral("uu"));
        QVERIFY(QFile::exists(base + QStringLiteral("/file10.txt")));
        QVERIFY(!QFile::exists(base + QStringLiteral("/ten.txt")));

        // Undoing a copy moves the copy to the trash.
        w.tasks()->start(Task::Copy, {base + QStringLiteral("/file10.txt")}, base + QStringLiteral("/beta"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/beta/file10.txt")));
        QTRY_COMPARE(w.undoDepth(), 1);
        w.runCommandLine(QStringLiteral("undo"));
        QTRY_VERIFY(!QFile::exists(base + QStringLiteral("/beta/file10.txt")));
        QVERIFY(QFile::exists(base + QStringLiteral("/file10.txt")));
        QVERIFY(QFile::exists(trashFiles));
    }

    void undoDoesNotReplaceANewFile() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.tasks()->start(Task::Move, {base + QStringLiteral("/file2.txt")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(w.undoDepth(), 1);
        writeFile(base + QStringLiteral("/file2.txt"), "newer\n");
        QSignalSpy finished(w.tasks(), &TaskManager::taskFinished);
        w.runCommandLine(QStringLiteral("undo"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().value<Task *>()->progress().state, Task::Failed);
        QFile f(base + QStringLiteral("/file2.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("newer\n"));
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/file2.txt")));
    }

    void restoreFromTrashFolder() {
        const QString base = makeTree();
        QString inTrash;
        QVERIFY(QFile::moveToTrash(base + QStringLiteral("/file2.txt"), &inTrash));
        QCOMPARE(trashOriginalPath(inTrash), base + QStringLiteral("/file2.txt"));
        MainWindow w(inTrash, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(currentName(w), QFileInfo(inTrash).fileName());
        keys(w, QStringLiteral("ur"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/file2.txt")));
        QVERIFY(!QFile::exists(inTrash));
        QVERIFY(trashInfoPath(inTrash).isEmpty());
        // Undoing the restore trashes it again.
        QTRY_COMPARE(w.undoDepth(), 1);
        w.runCommandLine(QStringLiteral("undo"));
        QTRY_VERIFY(!QFile::exists(base + QStringLiteral("/file2.txt")));
    }

    void renameAllHandlesSwapsAndRefusesCollisions() {
        const QString base = makeTree();
        writeFile(base + QStringLiteral("/a"), "A");
        writeFile(base + QStringLiteral("/b"), "B");
        QStringList errors;
        auto done = renameAll({{base + QStringLiteral("/a"), base + QStringLiteral("/b")},
                               {base + QStringLiteral("/b"), base + QStringLiteral("/a")}},
                              &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u';')));
        QCOMPARE(done.size(), 2);
        QFile a(base + QStringLiteral("/a"));
        QVERIFY(a.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), QByteArray("B"));

        // A target that exists and isn't being renamed away stops everything.
        errors.clear();
        done = renameAll({{base + QStringLiteral("/a"), base + QStringLiteral("/c")},
                          {base + QStringLiteral("/b"), base + QStringLiteral("/notes.md")}},
                         &errors);
        QVERIFY(done.isEmpty());
        QCOMPARE(errors.size(), 1);
        QVERIFY(QFile::exists(base + QStringLiteral("/a")));
        QVERIFY(!QFile::exists(base + QStringLiteral("/c")));

        errors.clear();
        done = renameAll({{base + QStringLiteral("/a"), base + QStringLiteral("/same")},
                          {base + QStringLiteral("/b"), base + QStringLiteral("/same")}},
                         &errors);
        QVERIFY(done.isEmpty());
        QVERIFY(!errors.isEmpty());
        // Nothing is left behind under a staging name.
        for (const QString &name : QDir(base).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
            QVERIFY2(!name.contains(QStringLiteral("scout-rename")), qPrintable(name));
    }

    void bulkRenameThroughEditor() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.bulkRenameWith([](const QStringList &lines) {
            QStringList out = lines;
            for (QString &l : out)
                if (l == QLatin1String("file2.txt"))
                    l = QStringLiteral("file10.txt");
                else if (l == QLatin1String("file10.txt"))
                    l = QStringLiteral("file2.txt");
                else if (l == QLatin1String("notes.md"))
                    l = QStringLiteral("NOTES.md");
            return out;
        });
        QVERIFY(w.prompt()->isOpen());
        QCOMPARE(w.picker()->items().size(), 3);
        keys(w, QStringLiteral("y"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/NOTES.md")));
        QFile ten(base + QStringLiteral("/file10.txt"));
        QVERIFY(ten.open(QIODevice::ReadOnly));
        QCOMPARE(ten.size(), QFileInfo(base + QStringLiteral("/file2.txt")).size());
        QCOMPARE(w.undoDepth(), 1);
        w.runCommandLine(QStringLiteral("undo"));
        QVERIFY(QFile::exists(base + QStringLiteral("/notes.md")));
        QVERIFY(!QFile::exists(base + QStringLiteral("/NOTES.md")));

        // Wrong line counts and slashes rename nothing.
        w.bulkRenameWith([](const QStringList &lines) { return lines.mid(1); });
        QVERIFY(!w.prompt()->isOpen());
        w.bulkRenameWith([](const QStringList &lines) {
            QStringList out = lines;
            out.last() = QStringLiteral("x/y");
            return out;
        });
        QVERIFY(!w.prompt()->isOpen());
        QVERIFY(QFile::exists(base + QStringLiteral("/notes.md")));
    }

    void bulkRenameRunsEditorInTerminal() {
        const QString base = makeTree();
        // A stand-in terminal that runs its -e command line directly.
        const QString term = m_root.path() + QStringLiteral("/fake-term");
        writeFile(term, "#!/bin/sh\n[ \"$1\" = -e ] && shift\nexec \"$@\"\n");
        QVERIFY(chmod(QFile::encodeName(term).constData(), 0755) == 0);
        qputenv("TERMINAL", term.toLocal8Bit());
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.runCommandLine(QStringLiteral("set editor sed -i s/file/doc/"));
        w.runCommandLine(QStringLiteral("bulk-rename"));
        QTRY_VERIFY_WITH_TIMEOUT(w.prompt()->isOpen(), 10000);
        QCOMPARE(w.picker()->items().size(), 2);
        keys(w, QStringLiteral("y"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/doc2.txt")));
        QVERIFY(QFile::exists(base + QStringLiteral("/doc10.txt")));
        QVERIFY(!QFile::exists(base + QStringLiteral("/file2.txt")));

        // A failing editor renames nothing.
        w.runCommandLine(QStringLiteral("set editor false"));
        w.runCommandLine(QStringLiteral("bulk-rename"));
        QTRY_VERIFY_WITH_TIMEOUT(w.statusBar()->message().contains(QStringLiteral("status 1")), 10000);
        QVERIFY(!w.prompt()->isOpen());
        qunsetenv("TERMINAL");
    }

    void crossDeviceMoveKeepsLateArrivals() {
        struct stat shm {}, root {};
        if (stat("/dev/shm", &shm) != 0 || stat(QFile::encodeName(m_root.path()).constData(), &root) != 0 ||
            shm.st_dev == root.st_dev || access("/dev/shm", W_OK) != 0)
            QSKIP("needs /dev/shm on a separate, writable filesystem");
        struct statvfs vfs {};
        if (statvfs("/dev/shm", &vfs) != 0 || quint64(vfs.f_bavail) * vfs.f_frsize < (256ull << 20))
            QSKIP("needs 256 MiB free in /dev/shm");
        QTemporaryDir other(QStringLiteral("/dev/shm/scout-test-XXXXXX"));
        QVERIFY(other.isValid());
        const QByteArray chunk(4 << 20, 'x');
        for (int i = 0; i < 32; ++i)
            writeFile(other.path() + QStringLiteral("/pkg/f%1.bin").arg(i), chunk);
        QVERIFY(QDir().mkpath(other.path() + QStringLiteral("/pkg/dir")));
        const QString base = makeTree();
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        tasks.start(Task::Move, {other.path() + QStringLiteral("/pkg")}, base + QStringLiteral("/beta"));
        // Add a file to the source while the copy is under way.
        bool added = false;
        QElapsedTimer clock;
        clock.start();
        while (!added && finished.isEmpty() && clock.elapsed() < 10000) {
            const QStringList staging = QDir(base + QStringLiteral("/beta"))
                                            .entryList({QStringLiteral(".pkg.scout-partial-*")}, QDir::AllEntries | QDir::Hidden);
            if (!staging.isEmpty()) {
                writeFile(other.path() + QStringLiteral("/pkg/late.txt"), "late\n");
                // A folder replaced by a new one, which may reuse the old inode number.
                QVERIFY(QDir().rmdir(other.path() + QStringLiteral("/pkg/dir")));
                writeFile(other.path() + QStringLiteral("/pkg/dir/marker"), "new\n");
                added = true;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        if (!added)
            QSKIP("the copy finished before the test could add a file");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY(QFile::exists(other.path() + QStringLiteral("/pkg/late.txt")));
        QVERIFY(QFile::exists(other.path() + QStringLiteral("/pkg/dir/marker")) ||
                QFile::exists(base + QStringLiteral("/beta/pkg/dir/marker")));
        QVERIFY(!QFile::exists(other.path() + QStringLiteral("/pkg/f0.bin")));
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/pkg/f31.bin")));
    }

    void undoLeavesReplacedFilesAlone() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.tasks()->start(Task::Copy, {base + QStringLiteral("/notes.md")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(w.undoDepth(), 1);
        QVERIFY(QFile::remove(base + QStringLiteral("/beta/notes.md")));
        writeFile(base + QStringLiteral("/beta/notes.md"), "someone else's file\n");
        w.runCommandLine(QStringLiteral("undo"));
        QVERIFY(w.statusBar()->message().contains(QStringLiteral("replaced")));
        QCOMPARE(w.tasks()->activeCount(), 0);
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/notes.md")));

        // A renamed file that was renamed again by someone else stays put.
        w.cd(base + QStringLiteral("/file2.txt"));
        QTRY_COMPARE(currentName(w), QStringLiteral("file2.txt"));
        w.runCommandLine(QStringLiteral("rename two.txt"));
        QVERIFY(QFile::rename(base + QStringLiteral("/two.txt"), base + QStringLiteral("/elsewhere.txt")));
        writeFile(base + QStringLiteral("/two.txt"), "new\n");
        w.runCommandLine(QStringLiteral("undo"));
        QVERIFY(!QFile::exists(base + QStringLiteral("/file2.txt")));
        QVERIFY(QFile::exists(base + QStringLiteral("/two.txt")));
    }

    void bulkRenameKeepsCarriageReturnNames() {
        const QString base = makeTree();
        writeFile(base + QStringLiteral("/odd\r"), "odd");
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.bulkRenameWith([](const QStringList &lines) { return lines; });
        QVERIFY(!w.prompt()->isOpen());
        QVERIFY(QFile::exists(base + QStringLiteral("/odd\r")));
        // An editor that writes CRLF adds a \r to every line, odd\r included.
        w.bulkRenameWith([](const QStringList &lines) {
            QStringList out;
            for (const QString &l : lines)
                out << l + u'\r';
            return out;
        });
        QVERIFY(!w.prompt()->isOpen());
        // ...and an edited name loses only the editor's \r.
        w.bulkRenameWith([](const QStringList &lines) {
            QStringList out;
            for (const QString &l : lines)
                out << (l == QLatin1String("notes.md") ? QStringLiteral("notes2.md") : l) + u'\r';
            return out;
        });
        QVERIFY(w.prompt()->isOpen());
        QCOMPARE(w.picker()->items().size(), 1);
        keys(w, QStringLiteral("y"));
        QTRY_VERIFY(QFile::exists(base + QStringLiteral("/notes2.md")));
        QVERIFY(QFile::exists(base + QStringLiteral("/odd\r")));
    }

    void crossDeviceMoveJudgesHardLinksByName() {
        struct stat shm {}, root {};
        if (stat("/dev/shm", &shm) != 0 || stat(QFile::encodeName(m_root.path()).constData(), &root) != 0 ||
            shm.st_dev == root.st_dev || access("/dev/shm", W_OK) != 0)
            QSKIP("needs /dev/shm on a separate, writable filesystem");
        // Two names for one file both go, and land as two copies.
        QTemporaryDir other(QStringLiteral("/dev/shm/scout-test-XXXXXX"));
        writeFile(other.path() + QStringLiteral("/pkg/a"), "same\n");
        QVERIFY(link(QFile::encodeName(other.path() + QStringLiteral("/pkg/a")).constData(),
                     QFile::encodeName(other.path() + QStringLiteral("/pkg/b")).constData()) == 0);
        const QString base = makeTree();
        TaskManager tasks;
        QSignalSpy finished(&tasks, &TaskManager::taskFinished);
        Task *t = tasks.start(Task::Move, {other.path() + QStringLiteral("/pkg")}, base + QStringLiteral("/beta"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(t->progress().state, Task::Done);
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/pkg/a")));
        QVERIFY(QFile::exists(base + QStringLiteral("/beta/pkg/b")));
        QVERIFY(!QFileInfo::exists(other.path() + QStringLiteral("/pkg")));
    }

    void chooserOpenSaveAndFolders() {
        const QString base = makeTree();
        {
            ChooserRequest req;
            req.filters = {FileFilter{QStringLiteral("Markdown"), {{0u, QStringLiteral("*.md")}}},
                           FileFilter{QStringLiteral("Text"), {{1u, QStringLiteral("text/plain")}}}};
            req.currentFilter = 0;
            ChooserResult got;
            int calls = 0;
            MainWindow w(base, false);
            w.setChooser(req, [&](const ChooserResult &r) {
                got = r;
                ++calls;
            });
            w.show();
            QTRY_VERIFY(w.isIdle());
            QCOMPARE(w.currentModel()->rowOf(QStringLiteral("file2.txt")), -1);
            QVERIFY(w.currentModel()->rowOf(QStringLiteral("alpha")) >= 0);
            keys(w, QStringLiteral("G"));
            QCOMPARE(currentName(w), QStringLiteral("notes.md"));
            key(w, Qt::Key_Return);
            QCOMPARE(calls, 1);
            QVERIFY(got.accepted);
            QCOMPARE(got.paths, QStringList{base + QStringLiteral("/notes.md")});
            QCOMPARE(got.filter, 0);
            QTRY_VERIFY(!w.isVisible());
        }
        {
            ChooserRequest req;
            req.directory = true;
            ChooserResult got;
            MainWindow w(base, false);
            w.setChooser(req, [&](const ChooserResult &r) { got = r; });
            w.show();
            QTRY_VERIFY(w.isIdle());
            QCOMPARE(w.currentModel()->rowOf(QStringLiteral("notes.md")), -1);
            keys(w, QStringLiteral("l"));
            QTRY_VERIFY(w.isIdle());
            QCOMPARE(w.cwd(), base + QStringLiteral("/alpha"));
            key(w, Qt::Key_Return, Qt::ControlModifier);
            QVERIFY(got.accepted);
            QCOMPARE(got.paths, QStringList{base + QStringLiteral("/alpha")});
        }
        {
            ChooserRequest req;
            req.mode = ChooserRequest::Mode::Save;
            req.currentName = QStringLiteral("report.pdf");
            req.currentFolder = base + QStringLiteral("/beta");
            ChooserResult got;
            int calls = 0;
            MainWindow w(base, false);
            w.setChooser(req, [&](const ChooserResult &r) {
                got = r;
                ++calls;
            });
            w.show();
            QTRY_VERIFY(w.isIdle());
            QCOMPARE(w.cwd(), base + QStringLiteral("/beta"));
            QLineEdit *name = w.chooserBar()->nameEdit();
            QTRY_VERIFY(name->hasFocus());
            QCOMPARE(name->selectedText(), QStringLiteral("report"));
            // Saving over an existing file asks first.
            name->setText(QStringLiteral("../notes.md"));
            QTest::keyClick(name, Qt::Key_Return);
            QVERIFY(w.prompt()->isOpen());
            keys(w, QStringLiteral("n"));
            QCOMPARE(calls, 0);
            name->setText(QStringLiteral("fresh.txt"));
            QTest::keyClick(name, Qt::Key_Return);
            QCOMPARE(calls, 1);
            QCOMPARE(got.paths, QStringList{base + QStringLiteral("/beta/fresh.txt")});
        }
        {
            ChooserResult got;
            got.accepted = true;
            int calls = 0;
            auto *w = new MainWindow(base, false);
            w->setChooser(ChooserRequest{}, [&](const ChooserResult &r) {
                got = r;
                ++calls;
            });
            w->show();
            QTRY_VERIFY(w->isIdle());
            key(*w, Qt::Key_Escape);
            QCOMPARE(calls, 1);
            QVERIFY(!got.accepted);
            delete w;
            QCOMPARE(calls, 1);
        }
    }

    void portalOptionsAndResults() {
        service::registerTypes();
        QVariantMap options;
        options.insert(QStringLiteral("multiple"), true);
        options.insert(QStringLiteral("accept_label"), QStringLiteral("_Upload"));
        options.insert(QStringLiteral("current_folder"), QByteArray("/tmp\0", 5));
        const QList<FileFilter> filters = {FileFilter{QStringLiteral("Images"), {{1u, QStringLiteral("image/*")}}},
                                           FileFilter{QStringLiteral("All"), {{0u, QStringLiteral("*")}}}};
        options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
        options.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters[1]));
        const ChooserRequest r = service::requestFromOptions(ChooserRequest::Mode::Open, QStringLiteral("Pick"), options);
        QVERIFY(r.multiple);
        QCOMPARE(r.currentFolder, QStringLiteral("/tmp"));
        QCOMPARE(r.filters.size(), 2);
        QCOMPARE(r.currentFilter, 1);
        QCOMPARE(r.acceptLabel, QStringLiteral("_Upload"));

        // Filters are identified by their whole definition, not just the label.
        QVariantMap same;
        const QList<FileFilter> twins = {FileFilter{QStringLiteral("Images"), {{0u, QStringLiteral("*.png")}}},
                                         FileFilter{QStringLiteral("Images"), {{0u, QStringLiteral("*.jpg")}}}};
        same.insert(QStringLiteral("filters"), QVariant::fromValue(twins));
        same.insert(QStringLiteral("current_filter"), QVariant::fromValue(twins[0]));
        QCOMPARE(service::requestFromOptions(ChooserRequest::Mode::Open, {}, same).currentFilter, 0);

        FileEntry png;
        png.name = QStringLiteral("cat.PNG");
        QVERIFY(r.filters[0].accepts(png));
        FileEntry txt;
        txt.name = QStringLiteral("notes.txt");
        QVERIFY(!r.filters[0].accepts(txt));

        ChooserResult result;
        result.accepted = true;
        result.paths = {QStringLiteral("/tmp/a b.png")};
        result.filter = 0;
        const QVariantMap out = service::resultsFromChooser(r, result);
        QCOMPARE(out.value(QStringLiteral("uris")).toStringList(), QStringList{QStringLiteral("file:///tmp/a%20b.png")});
        QVERIFY(out.contains(QStringLiteral("current_filter")));
        QVERIFY(service::resultsFromChooser(r, ChooserResult{}).isEmpty());
    }

    void goKeysAndRemovedBindings() {
        const QString downloads = m_root.path() + QStringLiteral("/my downloads");
        QVERIFY(QDir().mkpath(downloads));
        writeFile(qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/user-dirs.dirs"),
                  "XDG_DOWNLOAD_DIR=\"" + downloads.toUtf8() + "\"\n");
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("gd"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), downloads);

        for (const char *gone : {"!", "gT", "gD", "gv", "gM", "gm"}) {
            const QStringList seq = KeyMap::parseSequence(QLatin1String(gone));
            const QList<Binding> all = w.keymap()->bindings();
            QVERIFY2(std::none_of(all.begin(), all.end(), [&](const Binding &b) { return b.keys == seq; }), gone);
        }
        QVERIFY(!w.runCommandLine(QStringLiteral("shell true")));

        w.runCommandLine(QStringLiteral("cd ") + base);
        w.runCommandLine(QStringLiteral("new-tab ") + base + QStringLiteral("/alpha"));
        w.runCommandLine(QStringLiteral("new-tab ") + base + QStringLiteral("/beta"));
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.currentTab(), 2);
        keys(w, QStringLiteral("gp"));
        QCOMPARE(w.currentTab(), 1);
        keys(w, QStringLiteral("gp"));
        QCOMPARE(w.currentTab(), 0);
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base);

        const double size = Theme::instance().font().pointSizeF();
        key(w, Qt::Key_Equal, Qt::ControlModifier);
        key(w, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(Theme::instance().font().pointSizeF(), size + 2);
        key(w, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(Theme::instance().font().pointSizeF(), Theme::instance().defaultFontSize());
        QFile::remove(qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/user-dirs.dirs"));
    }

    void zoomIsRememberedAndReset() {
        Theme &theme = Theme::instance();
        const double systemSize = theme.defaultFontSize();
        theme.resetZoom();
        QVERIFY(!state::fontSize());
        const QString conf = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/scout/scout.conf");
        writeFile(conf, "set font-size 9\n");
        const QString base = makeTree();
        {
            MainWindow w(base, true);
            QCOMPARE(theme.font().pointSizeF(), 9.0);
            w.runCommandLine(QStringLiteral("zoom-in"));
            w.runCommandLine(QStringLiteral("zoom-in"));
            QCOMPARE(theme.font().pointSizeF(), 11.0);
            QCOMPARE(state::fontSize(), std::optional<double>(11.0));
        }
        // The next start: the remembered size beats the config's default.
        theme.setFontSize(systemSize);
        theme.restoreZoom();
        {
            MainWindow w(base, true);
            QCOMPARE(theme.font().pointSizeF(), 11.0);
            QCOMPARE(theme.defaultFontSize(), 9.0);
            key(w, Qt::Key_0, Qt::ControlModifier);
            QCOMPARE(theme.font().pointSizeF(), 9.0);
            QVERIFY(!state::fontSize());
            // Typing `set font-size` replaces a remembered zoom.
            w.runCommandLine(QStringLiteral("zoom-out"));
            QVERIFY(state::fontSize());
            w.runCommandLine(QStringLiteral("set font-size 14"));
            QCOMPARE(theme.font().pointSizeF(), 14.0);
            QVERIFY(!state::fontSize());
        }
        QVERIFY(QFile::remove(conf));
        theme.setFontSize(systemSize, true);
    }

    void tabsRestoredOnlyWithoutAPath() {
        const QString base = makeTree();
        const QString alpha = base + QStringLiteral("/alpha"), beta = base + QStringLiteral("/beta");
        state::setSession({{alpha, base + QStringLiteral("/gone"), beta}, 2});
        {
            const auto w = startup::openWindow({}, false);
            QCOMPARE(w->tabCount(), 2);
            QCOMPARE(w->currentTab(), 1);
            QCOMPARE(w->cwd(), beta);
            w->runCommandLine(QStringLiteral("tab 1"));
            QCOMPARE(w->cwd(), alpha);
            w->runCommandLine(QStringLiteral("new-tab ") + base);
            QCOMPARE(w->currentTab(), 1);
            w->show();
            w->close();
        }
        // Closing saved the window's tabs.
        QCOMPARE(state::session()->tabs, (QStringList{alpha, base, beta}));
        QCOMPARE(state::session()->active, 1);
        {
            const auto w = startup::openWindow({beta}, false);
            QCOMPARE(w->tabCount(), 1);
            QCOMPARE(w->cwd(), beta);
        }
        // FileManager1 windows open just their folder.
        const QList<MainWindow *> shown = service::showItems({alpha}, true);
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown[0]->tabCount(), 1);
        QCOMPARE(shown[0]->cwd(), alpha);
        qDeleteAll(shown);
        // A chooser leaves the saved tabs alone.
        {
            MainWindow chooser(base, false);
            ChooserRequest request;
            request.currentFolder = alpha;
            chooser.setChooser(request, [](const ChooserResult &) {});
            chooser.show();
            chooser.close();
        }
        QCOMPARE(state::session()->tabs, (QStringList{alpha, base, beta}));

        state::setSession({{base + QStringLiteral("/gone")}, 0});
        const auto w = startup::openWindow({}, false);
        QCOMPARE(w->tabCount(), 1);
        QCOMPARE(w->cwd(), QDir::homePath());
    }

    void defaultBindingsNeverShadowEachOther() {
        MainWindow w(m_root.path(), false);
        const QList<Binding> all = w.defaultKeys().bindings();
        for (const Binding &a : all)
            for (const Binding &b : all)
                QVERIFY2(a.keys == b.keys || b.keys.mid(0, a.keys.size()) != a.keys,
                         qPrintable(a.keys.join(QString()) + QStringLiteral(" / ") + b.keys.join(QString())));
        QVERIFY(std::none_of(all.begin(), all.end(), [](const Binding &b) { return b.fromConfig; }));
        QVERIFY(config::analyze(QString(), w.commands(), w.defaultKeys()).errors.isEmpty());
    }

    void configCheckFindsErrorsAndOverrides() {
        MainWindow w(m_root.path(), false);
        const ConfigReport r = config::analyze(QStringLiteral("# a comment\n"
                                                              "set hidden yes\n"
                                                              "set hidden maybe\n"
                                                              "set colour red\n"
                                                              "bogus\n"
                                                              "map gd cd /tmp\n"
                                                              "map g cd ~\n"
                                                              "map ggx top\n"
                                                              "map zz nope\n"
                                                              "map <Foo> top\n"
                                                              "unmap q\n"
                                                              "unmap zq\n"
                                                              "map <C-y> 'set theme pink'\n"
                                                              // One error per pair, at the later line.
                                                              "map ab top\n"
                                                              "map a down\n"
                                                              "mkdir\n"),
                                               w.commands(), w.defaultKeys());
        const QStringList expected = {
            QStringLiteral("line 3: hidden takes true, false or toggle, not maybe"),
            QStringLiteral("line 4: unknown option: colour"),
            QStringLiteral("line 5: unknown command bogus"),
            QStringLiteral("line 7: map g is a prefix of other bindings (g/, gb, gc, gd, gf, gg and 6 more), so it only runs after a pause"),
            QStringLiteral("line 8: map ggx starts with g, which is bound on its own, so g only runs after a pause"),
            QStringLiteral("line 8: map ggx starts with gg, which is bound on its own, so gg only runs after a pause"),
            QStringLiteral("line 9: map zz: unknown command nope"),
            QStringLiteral("line 10: map <Foo>: unknown key <Foo>"),
            QStringLiteral("line 13: map <C-y>: theme takes auto, dark or light"),
            QStringLiteral("line 15: map a is a prefix of other bindings (ab), so it only runs after a pause"),
            QStringLiteral("line 16: mkdir needs <name>"),
        };
        QCOMPARE(r.errors, expected);
        QCOMPARE(r.notes, (QStringList{
                              QStringLiteral("line 6: map gd overrides the default gd (%1)").arg(w.defaultKeys().binding(QStringLiteral("gd"))->command),
                              QStringLiteral("line 11: unmap q removes the default q (close-tab)"),
                              QStringLiteral("line 12: unmap zq does nothing; zq isn't bound"),
                              QStringLiteral("line 15: map a overrides the default a (create)"),
                          }));
        QCOMPARE(r.lines.size(), 15);
        QVERIFY(r.lines[0].run);
        QVERIFY(!r.lines[1].run);
        QVERIFY(r.lines[6].run); // shadowing still binds

        const QString good = m_root.path() + QStringLiteral("/good.conf"), bad = m_root.path() + QStringLiteral("/bad.conf");
        writeFile(good, "set sort modified reverse\nmap gw cd '~/my work'\nunmap gr\n");
        writeFile(bad, "set sort sideways\n");
        auto check = [](const QStringList &args, int *code) {
            QProcess p;
            p.start(QCoreApplication::applicationDirPath() + QStringLiteral("/scout"), QStringList{QStringLiteral("--check-config")} + args);
            p.waitForFinished(60000);
            *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
            return QString::fromUtf8(p.readAllStandardOutput());
        };
        int code = 0;
        QString out = check({good}, &code);
        QCOMPARE(code, 0);
        QCOMPARE(out, QStringLiteral("scout.conf: %1\ninfo:\n  line 3: unmap gr removes the default gr (cd ~/repos)\nno errors\n").arg(good));
        out = check({bad}, &code);
        QCOMPARE(code, 1);
        QCOMPARE(out, QStringLiteral("scout.conf: %1\nerrors:\n  line 1: sort takes name, size, modified or extension, then optionally reverse\n1 error\n").arg(bad));
        out = check({m_root.path() + QStringLiteral("/missing.conf")}, &code);
        QCOMPARE(code, 1);
        // Without a file it checks the usual config, which these tests don't have.
        out = check({}, &code);
        QCOMPARE(code, 0);
        QVERIFY2(out.startsWith(QStringLiteral("no config at ")), qPrintable(out));
    }

    void configErrorsReachStatusBarAndLog() {
        const QString log = m_root.path() + QStringLiteral("/log-config/scout.log");
        QVERIFY(logging::open(log));
        const QString conf = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/scout/scout.conf");
        writeFile(conf, "bogus\nmap gw cd /tmp\nset preview nah\n");
        MainWindow w(m_root.path(), true);
        const QString message = QStringLiteral("scout.conf: line 1: unknown command bogus; line 3: preview takes true, false or toggle, not nah");
        QCOMPARE(w.statusBar()->message(), message);
        QVERIFY(readAll(log).contains(QStringLiteral("] E ") + message));
        QVERIFY(w.keymap()->isFromConfig(QStringLiteral("gw")));
        QVERIFY(!w.keymap()->isFromConfig(QStringLiteral("j")));
        w.runCommandLine(QStringLiteral("map gq cd /"));
        QVERIFY(w.keymap()->binding(QStringLiteral("gq")));
        QVERIFY(!w.keymap()->isFromConfig(QStringLiteral("gq")));
        logging::close();
        QVERIFY(QFile::remove(conf));
    }

    void logWritesLinesAndRotates() {
        const QString path = m_root.path() + QStringLiteral("/log-rotate/scout.log");
        QVERIFY(logging::open(path, 4096));
        logging::info(QStringLiteral("first line"));
        logging::warning(QStringLiteral("two\nlines"));
        const QString first = readAll(path);
        const QString pid = QString::number(QCoreApplication::applicationPid());
        QVERIFY2(first.contains(QRegularExpression(QStringLiteral("^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{3} \\[%1\\] I first line\n").arg(pid))),
                 qPrintable(first));
        QVERIFY(first.contains(QStringLiteral("] W two\n    lines\n")));
        for (int i = 0; i < 100; ++i)
            logging::info(QStringLiteral("filler %1 ").arg(i) + QString(60, u'x'));
        logging::info(QStringLiteral("last line"));
        logging::close();
        QVERIFY(QFile::exists(path + QStringLiteral(".1")));
        QVERIFY(!QFile::exists(path + QStringLiteral(".2")));
        QVERIFY(QFileInfo(path).size() <= 4096 + 200);
        QVERIFY(QFileInfo(path + QStringLiteral(".1")).size() >= 4096);
        QVERIFY(readAll(path).contains(QStringLiteral("last line")));
        QVERIFY(!readAll(path).contains(QStringLiteral("first line")));
        // Nothing is written once closed.
        logging::info(QStringLiteral("after close"));
        QVERIFY(!readAll(path).contains(QStringLiteral("after close")));
    }

    void statusErrorsTasksAndClosesAreLogged() {
        const QString path = m_root.path() + QStringLiteral("/log-window/scout.log");
        QVERIFY(logging::open(path));
        const QString base = makeTree();
        auto *w = new MainWindow(base, false);
        w->show();
        QTRY_VERIFY(w->isIdle());
        w->runCommandLine(QStringLiteral("cd /no/such/folder"));
        QVERIFY(readAll(path).contains(QStringLiteral("] E no such directory: /no/such/folder")));
        w->tasks()->start(Task::Copy, {base + QStringLiteral("/missing")}, base + QStringLiteral("/beta"));
        QTRY_VERIFY(readAll(path).contains(QStringLiteral("] E task failed: Copying missing to beta")));

        w->runCommandLine(QStringLiteral("new-tab ") + base + QStringLiteral("/alpha"));
        QTRY_VERIFY(w->isIdle());
        keys(*w, QStringLiteral("q"));
        QVERIFY(readAll(path).contains(QStringLiteral("tab 2 of 2 closed by close-tab (key q): ") + base + QStringLiteral("/alpha")));
        key(*w, Qt::Key_W, Qt::ControlModifier);
        QVERIFY(!w->isVisible());
        QVERIFY(readAll(path).contains(QStringLiteral("window closed by close-tab on the last tab (key <C-w>): ") + base));
        w->show();
        w->runCommandLine(QStringLiteral("quit"));
        QVERIFY(readAll(path).contains(QStringLiteral("window closed by quit (command)")));
        w->show();
        w->close();
        QVERIFY(readAll(path).contains(QStringLiteral("window closed by the window manager")));
        delete w;
        logging::close();
    }

    void crashHandlerLogsSignalAndBacktrace() {
        char line[160];
        // 2026-10-28 23:20:01.234 UTC, shown at UTC-4.
        const size_t n = logging::formatSignal(line, sizeof line, SIGSEGV, 4242, 1793229601234LL, -4 * 3600);
        QCOMPARE(QByteArray(line, int(n)), QByteArray("2026-10-28 19:20:01.234 [4242] F crash: signal 11 (SIGSEGV), backtrace follows\n"));
        QVERIFY(logging::formatSignal(line, 12, SIGTERM, 1, 0, 0) == 11);

        const QString path = m_root.path() + QStringLiteral("/log-crash/scout.log");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--crash-child"), path});
        QVERIFY(child.waitForFinished(30000));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        const QString log = readAll(path);
        QVERIFY2(log.contains(QStringLiteral("I about to crash")), qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("] F crash: signal 11 (SIGSEGV), backtrace follows")), qPrintable(log));
        // backtrace_symbols_fd lines: "binary(symbol+0x..) [0x...]".
        QVERIFY2(log.contains(QStringLiteral("scout_tests(")) && log.contains(QStringLiteral(") [0x")), qPrintable(log));
    }

    void showItemsGroupsByFolder() {
        const QString base = makeTree();
        const QList<MainWindow *> windows = service::showItems(
            {QUrl::fromLocalFile(base + QStringLiteral("/notes.md")).toString(), base + QStringLiteral("/file2.txt"),
             base + QStringLiteral("/alpha/one.md")},
            false);
        QCOMPARE(windows.size(), 2);
        windows[0]->show();
        QTRY_VERIFY(windows[0]->isIdle());
        QCOMPARE(windows[0]->cwd(), base);
        QCOMPARE(currentName(*windows[0]), QStringLiteral("notes.md"));
        QCOMPARE(windows[0]->marks()->size(), 2);
        QCOMPARE(windows[1]->cwd(), base + QStringLiteral("/alpha"));
        qDeleteAll(windows);
    }
};

int main(int argc, char *argv[]) {
    if (argc == 3 && std::strcmp(argv[1], "--crash-child") == 0)
        crashChild(argv[2]);
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    ScoutTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_scout.moc"
