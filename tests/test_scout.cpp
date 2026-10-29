#include "clipboard.h"
#include "commands.h"
#include "desktopentry.h"
#include "dirmodel.h"
#include "fileops.h"
#include "fuzzy.h"
#include "keymap.h"
#include "mainwindow.h"
#include "service.h"
#include "theme.h"

#include <QApplication>
#include <QDBusArgument>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <cerrno>
#include <sys/stat.h>
#include <sys/statvfs.h>
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

        key(w, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(w.prompt()->isOpen());
        keys(w, QStringLiteral("deep"));
        QTRY_VERIFY(!w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->items().first().text, QStringLiteral("alpha/inner/deep.txt"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.cwd(), base + QStringLiteral("/alpha/inner"));
        QTRY_COMPARE(currentName(w), QStringLiteral("deep.txt"));

        key(w, Qt::Key_K, Qt::ControlModifier);
        keys(w, QStringLiteral("mkdir made"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(QFileInfo(base + QStringLiteral("/alpha/inner/made")).isDir());

        key(w, Qt::Key_K, Qt::ControlModifier);
        keys(w, QStringLiteral("hidd"));
        QTRY_VERIFY(!w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->items().first().description, QStringLiteral("toggle-hidden"));
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

    void textMeetsContrastInEveryTheme() {
        const QJsonObject stylix = QJsonDocument::fromJson(R"({
            "base00": "1e1e2e", "base01": "181825", "base02": "313244", "base03": "45475a",
            "base04": "585b70", "base05": "cdd6f4", "base06": "f5e0dc", "base07": "b4befe",
            "base08": "f38ba8", "base09": "fab387", "base0A": "f9e2af", "base0B": "a6e3a1",
            "base0C": "94e2d5", "base0D": "89b4fa", "base0E": "cba6f7", "base0F": "f2cdcd"})").object();
        QVERIFY(!stylix.isEmpty());
        for (const Palette &p : {Palette::fromBase16(stylix, QStringLiteral("stylix")), Palette::mocha(), Palette::latte()}) {
            for (const Palette::TextRole &role : p.textRoles()) {
                const double ratio = Palette::contrast(role.fg, role.bg);
                QVERIFY2(ratio >= 4.5, qPrintable(QStringLiteral("%1: %2 %3 on %4 is %5:1")
                                                      .arg(p.name, role.name, role.fg.name(), role.bg.name())
                                                      .arg(ratio, 0, 'f', 2)));
            }
        }
        // Decorations keep the scheme's own colors.
        QCOMPARE(Palette::fromBase16(stylix, {}).overlay, QColor(QStringLiteral("#45475a")));
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

    void nameMatcherTakesSubstringsAndGlobs() {
        QVERIFY(NameMatcher(QStringLiteral("not")).matches(QStringLiteral("Notes.md")));
        QVERIFY(!NameMatcher(QStringLiteral("Not")).matches(QStringLiteral("notes.md")));
        QCOMPARE(NameMatcher(QStringLiteral("te")).span(QStringLiteral("notes.md")), (QPair<int, int>{2, 2}));
        const NameMatcher png(QStringLiteral("*.png"));
        QVERIFY(png.isGlob());
        QVERIFY(png.matches(QStringLiteral("cat.png")));
        QVERIFY(png.matches(QStringLiteral("CAT.PNG")));
        QVERIFY(!png.matches(QStringLiteral("cat.png.txt")));
        QCOMPARE(png.span(QStringLiteral("cat.png")), (QPair<int, int>{0, 7}));
        QVERIFY(!NameMatcher(QStringLiteral("*.PNG")).matches(QStringLiteral("cat.png")));
        QVERIFY(NameMatcher(QStringLiteral("file?.txt")).matches(QStringLiteral("file2.txt")));
        QVERIFY(!NameMatcher(QStringLiteral("file?.txt")).matches(QStringLiteral("file10.txt")));
        QVERIFY(NameMatcher(QStringLiteral("[ab]*")).matches(QStringLiteral("beta")));
        QVERIFY(!NameMatcher(QStringLiteral("[ab")).matches(QStringLiteral("[ab")));
    }

    void filterTakesGlobs() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("f*.txt"));
        QTRY_COMPARE(w.currentModel()->rowCount(), 2);
        QCOMPARE(w.prompt()->style(), PromptSpec::Style::Bar);
        QCOMPARE(w.prompt()->info(), QStringLiteral("2 matches"));
        key(w, Qt::Key_Return);
        QCOMPARE(w.currentModel()->filter(), QStringLiteral("*.txt"));
        key(w, Qt::Key_Escape);
        QTRY_COMPARE(w.currentModel()->rowCount(), 5);
        keys(w, QStringLiteral("ffile?.txt"));
        QTRY_COMPARE(w.currentModel()->rowCount(), 1);
        QCOMPARE(currentName(w), QStringLiteral("file2.txt"));
        key(w, Qt::Key_Escape);
        QTRY_COMPARE(w.currentModel()->rowCount(), 5);
    }

    void searchBarCountsMatchesAndTakesGlobs() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        keys(w, QStringLiteral("/"));
        QVERIFY(w.prompt()->isOpen());
        QCOMPARE(w.prompt()->style(), PromptSpec::Style::Bar);
        // The bar sits above the status bar, which stays in view.
        QVERIFY(w.statusBar()->isVisible());
        QVERIFY(w.prompt()->geometry().bottom() < w.statusBar()->mapTo(&w, QPoint(0, 0)).y());
        keys(w, QStringLiteral("file"));
        QCOMPARE(currentName(w), QStringLiteral("file2.txt"));
        QCOMPARE(w.prompt()->info(), QStringLiteral("1 of 2"));
        QCOMPARE(w.currentView()->highlight().pattern(), QStringLiteral("file"));
        key(w, Qt::Key_Return);
        keys(w, QStringLiteral("n"));
        QCOMPARE(currentName(w), QStringLiteral("file10.txt"));
        keys(w, QStringLiteral("N"));
        QCOMPARE(currentName(w), QStringLiteral("file2.txt"));

        // A glob matches whole names; Esc puts the cursor and the old search back.
        keys(w, QStringLiteral("/*.md"));
        QCOMPARE(currentName(w), QStringLiteral("notes.md"));
        QCOMPARE(w.prompt()->info(), QStringLiteral("1 of 1"));
        key(w, Qt::Key_Escape);
        QCOMPARE(currentName(w), QStringLiteral("file2.txt"));
        QCOMPARE(w.currentView()->highlight().pattern(), QStringLiteral("file"));
        keys(w, QStringLiteral("/*.MD"));
        QCOMPARE(w.prompt()->info(), QStringLiteral("no matches"));
        key(w, Qt::Key_Escape);
        key(w, Qt::Key_Escape);
        QVERIFY(w.currentView()->highlight().isEmpty());
    }

    void paletteMergesHelpAndRunsArguments() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.keymap()->keysFor(QStringLiteral("command")), QStringLiteral("<C-k>"));
        QCOMPARE(w.keymap()->keysFor(QStringLiteral("find")), QStringLiteral("<C-f>"));
        QVERIFY(w.keymap()->keysFor(QStringLiteral("help")).isEmpty());
        for (const Binding &b : w.keymap()->bindings()) {
            const QString keys = b.keys.join(u' ');
            QVERIFY2(!QStringList({QStringLiteral(":"), QStringLiteral("?"), QStringLiteral("<C-p>"), QStringLiteral("g f"),
                                   QStringLiteral("<C-b>")})
                          .contains(keys),
                     qPrintable(keys));
            if (keys == QLatin1String("<C-f>"))
                QCOMPARE(b.command, QStringLiteral("find"));
        }

        w.runCommandLine(QStringLiteral("map <A-q> leave"));
        key(w, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(w.prompt()->isOpen());
        QCOMPARE(w.prompt()->style(), PromptSpec::Style::Palette);
        QVERIFY(w.picker()->isVisible());
        // One row per command with all of its keys, and one per binding that has arguments.
        auto row = [&](const QString &command) -> PickerItem {
            for (const PickerItem &item : w.picker()->items())
                if (item.description == command)
                    return item;
            return {};
        };
        QVERIFY(row(QStringLiteral("toggle-hidden")).keys.contains(QStringLiteral("zh")));
        QVERIFY(row(QStringLiteral("toggle-hidden")).keys.contains(QStringLiteral("Ctrl-h")));
        QCOMPARE(row(QStringLiteral("sort name")).text, QStringLiteral("Sort by name"));
        QCOMPARE(row(QStringLiteral("sort name")).keys, QStringLiteral("sn"));
        QVERIFY(row(QStringLiteral("command")).description.isEmpty());
        // Keys and several words are searchable.
        keys(w, QStringLiteral("zh"));
        QCOMPARE(w.picker()->items().first().description, QStringLiteral("toggle-hidden"));
        // Every key counts, however many a command has.
        w.prompt()->edit()->clear();
        keys(w, QStringLiteral("Alt-q"));
        QVERIFY(!w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->items().first().description, QStringLiteral("leave"));
        w.prompt()->edit()->clear();
        keys(w, QStringLiteral("copy path"));
        QCOMPARE(w.picker()->items().first().description, QStringLiteral("copy-path"));
        // A command line with arguments runs as typed.
        w.prompt()->edit()->clear();
        keys(w, QStringLiteral("sort size"));
        key(w, Qt::Key_Return);
        QVERIFY(!w.prompt()->isOpen());
        QCOMPARE(w.options().sort, ViewOptions::Size);
        // Rows that need arguments fill in the name.
        key(w, Qt::Key_K, Qt::ControlModifier);
        keys(w, QStringLiteral("mkdir"));
        QCOMPARE(w.picker()->items().first().description, QStringLiteral("mkdir"));
        key(w, Qt::Key_Return);
        QTRY_VERIFY(w.prompt()->isOpen());
        QCOMPARE(w.prompt()->text(), QStringLiteral("mkdir "));
        key(w, Qt::Key_Escape);
        // Nothing found says so in a sentence.
        key(w, Qt::Key_K, Qt::ControlModifier);
        keys(w, QStringLiteral("qqxqz"));
        QVERIFY(w.picker()->items().isEmpty());
        QCOMPARE(w.picker()->emptyTitle(), QStringLiteral("No commands match “qqxqz”"));
        key(w, Qt::Key_Escape);
        // The old command names open the same palette.
        for (const char *name : {"help", "command"}) {
            w.runCommandLine(QLatin1String(name));
            QVERIFY(w.prompt()->isOpen());
            QCOMPARE(w.picker()->header(), QStringLiteral("Commands"));
            key(w, Qt::Key_Escape);
            QVERIFY(!w.prompt()->isOpen());
        }
    }

    void keyMapSheetFollowsTheBindings() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        w.runCommandLine(QStringLiteral("map <C-y> copy"));
        w.runCommandLine(QStringLiteral("unmap yy"));
        w.runCommandLine(QStringLiteral("map <A-q> leave"));
        key(w, Qt::Key_K, Qt::ControlModifier);
        keys(w, QStringLiteral("keymap"));
        QCOMPARE(w.picker()->items().first().text, QStringLiteral("Show key map"));
        key(w, Qt::Key_Return);
        QVERIFY(w.keyMapSheet()->isVisible());
        QStringList titles;
        QString copyKeys, leaveKeys;
        for (const KeyMapSheet::Group &g : w.keyMapSheet()->groups()) {
            titles << g.title;
            for (const KeyMapSheet::Entry &e : g.entries) {
                if (e.description == QLatin1String("Copy to the clipboard"))
                    copyKeys = e.keys;
                if (e.description == QLatin1String("Go to the parent folder"))
                    leaveKeys = e.keys;
            }
        }
        // A fifth key for one command still shows.
        QVERIFY2(leaveKeys.contains(QStringLiteral("Alt-q")), qPrintable(leaveKeys));
        auto tabEntries = [&] {
            QStringList out;
            for (const KeyMapSheet::Group &g : w.keyMapSheet()->groups())
                for (const KeyMapSheet::Entry &e : g.entries)
                    if (e.description.startsWith(QLatin1String("Go to tab")))
                        out << e.keys + u' ' + e.description;
            return out;
        };
        QCOMPARE(tabEntries(), QStringList{QStringLiteral("Alt-1…Alt-9 Go to tab N")});
        key(w, Qt::Key_Escape);
        // A remapped tab key breaks the range up rather than hiding behind it.
        w.runCommandLine(QStringLiteral("unmap <A-5>"));
        w.runCommandLine(QStringLiteral("map X tab 5"));
        w.runCommandLine(QStringLiteral("keymap"));
        QVERIFY(tabEntries().contains(QStringLiteral("X Go to tab 5")));
        QVERIFY(!tabEntries().join(u' ').contains(QStringLiteral("Alt-5")));
        QCOMPARE(titles.mid(0, 9), (QStringList{QStringLiteral("Move"), QStringLiteral("Go"), QStringLiteral("Select"),
                                               QStringLiteral("Copy & move"), QStringLiteral("Remove & undo"),
                                               QStringLiteral("Change"), QStringLiteral("Find"), QStringLiteral("View"),
                                               QStringLiteral("Tabs")}));
        QVERIFY2(copyKeys.contains(QStringLiteral("Ctrl-y")) && !copyKeys.contains(QStringLiteral("yy")), qPrintable(copyKeys));
        QVERIFY(w.rect().contains(w.keyMapSheet()->geometry()));
        key(w, Qt::Key_Escape);
        QVERIFY(!w.keyMapSheet()->isVisible());
    }

    void propertiesShelfFollowsTheCursor() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.resize(1000, 600);
        w.show();
        QTRY_VERIFY(w.isIdle());
        const int fullHeight = w.preview()->height();
        key(w, Qt::Key_Return, Qt::AltModifier);
        PropertiesShelf *shelf = w.propertiesShelf();
        QTRY_VERIFY(shelf->isVisible());
        // The preview gives up the room rather than being covered.
        QTRY_VERIFY(w.preview()->height() < fullHeight);
        QVERIFY(w.preview()->geometry().bottom() < shelf->geometry().top());
        QCOMPARE(shelf->title(), QStringLiteral("alpha"));
        QTRY_VERIFY2(shelf->value(QStringLiteral("Total size")).contains(QStringLiteral("12 bytes")),
                     qPrintable(shelf->value(QStringLiteral("Total size"))));
        QCOMPARE(shelf->value(QStringLiteral("Contains")), QStringLiteral("2 items"));
        keys(w, QStringLiteral("G"));
        QTRY_COMPARE(shelf->title(), QStringLiteral("notes.md"));
        QVERIFY(shelf->value(QStringLiteral("Size")).contains(QStringLiteral("19 bytes")));
        QVERIFY(shelf->value(QStringLiteral("Access")).contains(QStringLiteral("-rw")));
        writeFile(base + QStringLiteral("/notes.md"), "# Notes, now longer\n");
        w.runCommandLine(QStringLiteral("reload"));
        QTRY_VERIFY(shelf->value(QStringLiteral("Size")).contains(QStringLiteral("20 bytes")));
        // Marks turn it into a summary.
        key(w, Qt::Key_Space);
        keys(w, QStringLiteral("gg"));
        key(w, Qt::Key_Space);
        QTRY_COMPARE(shelf->title(), QStringLiteral("2 items"));
        QTRY_VERIFY(shelf->value(QStringLiteral("Total size")).contains(QStringLiteral("32 bytes")));
        key(w, Qt::Key_Escape);
        QVERIFY(shelf->isVisible());
        key(w, Qt::Key_Return, Qt::AltModifier);
        QVERIFY(!shelf->isVisible());
        QTRY_COMPARE(w.preview()->height(), fullHeight);
        // Esc closes it once nothing else is left to clear.
        key(w, Qt::Key_Return, Qt::AltModifier);
        QVERIFY(shelf->isVisible());
        key(w, Qt::Key_Escape);
        QVERIFY(!shelf->isVisible());
        // An empty folder shows nothing rather than the last item.
        w.cd(base + QStringLiteral("/beta"));
        QTRY_VERIFY(w.isIdle());
        key(w, Qt::Key_Return, Qt::AltModifier);
        QCOMPARE(shelf->title(), QStringLiteral("Nothing selected"));
    }

    void tabsShowEvenWhenThereIsOne() {
        const QString base = makeTree();
        MainWindow w(base, false);
        w.show();
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.headerBar()->tabRects().size(), 1);
        w.runCommandLine(QStringLiteral("new-tab ") + base + QStringLiteral("/alpha"));
        QTRY_VERIFY(w.isIdle());
        const QList<QRect> tabs = w.headerBar()->tabRects();
        QCOMPARE(tabs.size(), 2);
        QVERIFY(tabs[0].right() < tabs[1].left());
        QTest::mouseClick(w.headerBar(), Qt::LeftButton, {}, tabs[0].center());
        QTRY_VERIFY(w.isIdle());
        QCOMPARE(w.currentTab(), 0);
        QCOMPARE(w.cwd(), base);
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

QTEST_MAIN(ScoutTest)
#include "test_scout.moc"
