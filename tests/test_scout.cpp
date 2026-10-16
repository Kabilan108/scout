#include "clipboard.h"
#include "commands.h"
#include "desktopentry.h"
#include "dirmodel.h"
#include "fileops.h"
#include "fuzzy.h"
#include "keymap.h"
#include "mainwindow.h"
#include "theme.h"

#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, const QByteArray &content = "hello\n") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
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

private slots:
    void initTestCase() {
        QVERIFY(m_root.isValid());
        qputenv("XDG_DATA_HOME", (m_root.path() + QStringLiteral("/xdg-data")).toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", (m_root.path() + QStringLiteral("/xdg-config")).toLocal8Bit());
        QDir().mkpath(m_root.path() + QStringLiteral("/xdg-data"));
        Theme::instance().init();
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
};

QTEST_MAIN(ScoutTest)
#include "test_scout.moc"
