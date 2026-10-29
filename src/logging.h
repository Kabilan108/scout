#pragma once

#include <QString>

#include <cstddef>

// scout.log: one file shared by every scout process, with one rotated copy.
// Nothing is written until open() is called; tests that don't open it stay quiet.
namespace logging {

enum class Level { Debug, Info, Warning, Error, Fatal };

// $XDG_STATE_HOME/scout/scout.log
QString defaultPath();
// Appends to `path` (creating its folder) and to stderr. Past `maxBytes` the
// file moves to `path`.1 and a new one starts.
bool open(const QString &path, qint64 maxBytes = 1024 * 1024);
void close();
bool isOpen();
QString path();

void write(Level level, const QString &text);
inline void info(const QString &text) { write(Level::Info, text); }
inline void warning(const QString &text) { write(Level::Warning, text); }
inline void error(const QString &text) { write(Level::Error, text); }

// Routes qDebug/qInfo/qWarning/qCritical/qFatal through write().
void installMessageHandler();
// SIGSEGV, SIGABRT, SIGBUS, SIGFPE and SIGILL write a backtrace to the log and
// stderr, then re-raise with the default action so a core dump still happens.
// SIGTERM, SIGHUP and SIGINT are noted without a backtrace.
void installCrashHandler();
// Sends libwayland's own messages, such as the protocol error that comes
// right before Qt exits without a core dump, to the log. Call after the
// QApplication exists; does nothing when libwayland isn't loaded.
void captureWaylandLog();
// Logs main()'s exit status; an exit() from anywhere else is logged as such.
void logExit(int status);

// Async-signal-safe: "<local time> [<pid>] F crash: signal 11 (SIGSEGV)\n".
// Returns the length written to `buf`, never more than `size` - 1.
size_t formatSignal(char *buf, size_t size, int sig, long pid, long long epochMs, long utcOffsetSeconds);

const char *commit();
QString versionString();

} // namespace logging
