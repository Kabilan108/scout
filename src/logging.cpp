#include "logging.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct LogState {
    std::mutex mutex;
    QByteArray path;
    qint64 maxBytes = 0;
    bool enabled = false;
};

LogState &state() {
    static LogState s;
    return s;
}

// Read by the signal handler, so outside the mutex.
std::atomic<int> g_fd{-1};
std::atomic<long> g_utcOffset{0};
std::atomic<bool> g_exitLogged{false};
char g_signalPath[4096]; // the log's path, for the signal handler

// True if `fd` no longer is the file at `path`: another process rotated it.
bool isStale(int fd, const char *path) {
    struct stat ours {}, onDisk {};
    return fstat(fd, &ours) != 0 || stat(path, &onDisk) != 0 || onDisk.st_ino != ours.st_ino || onDisk.st_dev != ours.st_dev;
}

void writeAll(int fd, const char *data, size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        data += n;
        size -= size_t(n);
    }
}

int openFile(const QByteArray &path) { return ::open(path.constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); }

// Several scout processes share the file; whichever notices it is full moves
// it aside, and the others follow once they see their descriptor is stale.
void rotateIfNeeded(LogState &s) {
    const int fd = g_fd.load();
    struct stat ours {};
    if (fd < 0 || fstat(fd, &ours) != 0 || (!isStale(fd, s.path.constData()) && ours.st_size < s.maxBytes))
        return;
    // Everyone still writing to this file locks the same inode, so only the
    // first to get here renames it; the rest see it replaced and reopen.
    flock(fd, LOCK_EX);
    if (!isStale(fd, s.path.constData()))
        ::rename(s.path.constData(), (s.path + ".1").constData());
    const int next = openFile(s.path);
    g_fd.store(next);
    ::close(fd);
}

char levelLetter(logging::Level level) {
    switch (level) {
    case logging::Level::Debug: return 'D';
    case logging::Level::Info: return 'I';
    case logging::Level::Warning: return 'W';
    case logging::Level::Error: return 'E';
    case logging::Level::Fatal: return 'F';
    }
    return '?';
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    logging::Level level = logging::Level::Info;
    switch (type) {
    case QtDebugMsg: level = logging::Level::Debug; break;
    case QtInfoMsg: level = logging::Level::Info; break;
    case QtWarningMsg: level = logging::Level::Warning; break;
    case QtCriticalMsg: level = logging::Level::Error; break;
    case QtFatalMsg: level = logging::Level::Fatal; break;
    }
    const char *category = context.category;
    const bool named = category && std::strcmp(category, "default") != 0;
    logging::write(level, named ? QStringLiteral("[%1] %2").arg(QLatin1String(category), message) : message);
}

const char *signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGTERM: return "SIGTERM";
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    default: return "signal";
    }
}

bool isCrash(int sig) { return sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS || sig == SIGFPE || sig == SIGILL; }

// Tiny async-signal-safe formatter.
struct Out {
    char *buf;
    size_t size;
    size_t len = 0;
    void put(char c) {
        if (len + 1 < size)
            buf[len++] = c;
    }
    void str(const char *s) {
        while (*s)
            put(*s++);
    }
    void num(long long v, int width = 0) {
        char digits[24];
        int n = 0;
        const bool negative = v < 0;
        unsigned long long u = negative ? 0ULL - (unsigned long long)v : (unsigned long long)v;
        do {
            digits[n++] = char('0' + u % 10);
            u /= 10;
        } while (u > 0 && n < 24);
        if (negative)
            put('-');
        for (int i = n; i < width; ++i)
            put('0');
        while (n > 0)
            put(digits[--n]);
    }
};

void onSignal(int sig) {
    const int saved = errno;
    struct timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    char line[160];
    const size_t n = logging::formatSignal(line, sizeof line, sig, long(getpid()),
                                           (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000, g_utcOffset.load());
    int fd = g_fd.load();
    // Another process may have rotated the file away; open() and stat() are signal-safe.
    if (fd >= 0 && g_signalPath[0] && isStale(fd, g_signalPath))
        if (const int fresh = ::open(g_signalPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); fresh >= 0)
            fd = fresh;
    writeAll(STDERR_FILENO, line, n);
    if (fd >= 0)
        writeAll(fd, line, n);
    if (isCrash(sig)) {
        void *frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        if (fd >= 0)
            backtrace_symbols_fd(frames, count, fd);
    }
    errno = saved;
    // Blocked until we return, then delivered with the default action (a core dump for crashes).
    signal(sig, SIG_DFL);
    raise(sig);
}

void onWaylandLog(const char *format, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, format, args);
    logging::warning(QStringLiteral("[wayland] %1").arg(QString::fromLocal8Bit(buf).trimmed()));
}

void onExit() {
    if (!g_exitLogged.load())
        logging::warning(QStringLiteral("exit() called before main() returned"));
}

} // namespace

QString logging::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/scout/scout.log");
}

bool logging::open(const QString &path, qint64 maxBytes) {
    LogState &s = state();
    std::lock_guard lock(s.mutex);
    if (const int old = g_fd.exchange(-1); old >= 0)
        ::close(old);
    QDir().mkpath(QFileInfo(path).absolutePath());
    s.path = QFile::encodeName(path);
    qstrncpy(g_signalPath, s.path.constData(), sizeof g_signalPath);
    s.maxBytes = maxBytes;
    s.enabled = true;
    g_fd.store(openFile(s.path));
    rotateIfNeeded(s);
    g_utcOffset.store(QDateTime::currentDateTime().offsetFromUtc());
    static const bool registered = std::atexit(onExit) == 0;
    Q_UNUSED(registered);
    return g_fd.load() >= 0;
}

void logging::close() {
    LogState &s = state();
    std::lock_guard lock(s.mutex);
    if (const int fd = g_fd.exchange(-1); fd >= 0)
        ::close(fd);
    g_signalPath[0] = '\0';
    s.enabled = false;
}

bool logging::isOpen() { return g_fd.load() >= 0; }

QString logging::path() {
    LogState &s = state();
    std::lock_guard lock(s.mutex);
    return QFile::decodeName(s.path);
}

void logging::write(Level level, const QString &text) {
    LogState &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.enabled)
        return;
    QString body = text;
    body.replace(u'\n', QStringLiteral("\n    "));
    const QByteArray line = QStringLiteral("%1 [%2] %3 %4\n")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                                .arg(QCoreApplication::applicationPid())
                                .arg(QLatin1Char(levelLetter(level)))
                                .arg(body)
                                .toUtf8();
    writeAll(STDERR_FILENO, line.constData(), size_t(line.size()));
    // Before writing: an idle process's descriptor may point at a rotated, even deleted, file.
    rotateIfNeeded(s);
    if (g_fd.load() >= 0)
        writeAll(g_fd.load(), line.constData(), size_t(line.size()));
}

void logging::installMessageHandler() { qInstallMessageHandler(messageHandler); }

void logging::installCrashHandler() {
    // backtrace() loads libgcc on first use, which isn't safe inside a handler.
    void *warmup[1];
    backtrace(warmup, 1);
    // A stack overflow leaves no stack to run the handler on.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTERM, SIGHUP, SIGINT})
        sigaction(sig, &sa, nullptr);
}

void logging::captureWaylandLog() {
    using Handler = void (*)(const char *, va_list);
    auto setHandler = reinterpret_cast<void (*)(Handler)>(dlsym(RTLD_DEFAULT, "wl_log_set_handler_client"));
    if (setHandler)
        setHandler(onWaylandLog);
}

void logging::logExit(int status) {
    g_exitLogged.store(true);
    info(QStringLiteral("exit status %1").arg(status));
}

size_t logging::formatSignal(char *buf, size_t size, int sig, long pid, long long epochMs, long utcOffsetSeconds) {
    if (size == 0)
        return 0;
    Out out{buf, size};
    // Civil date from days since the epoch (Howard Hinnant's algorithm); localtime() isn't signal-safe.
    const long long local = epochMs / 1000 + utcOffsetSeconds;
    const long long days = (local >= 0 ? local : local - 86399) / 86400;
    const long long secs = local - days * 86400;
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2);
    out.num(year, 4);
    out.put('-');
    out.num(month, 2);
    out.put('-');
    out.num(day, 2);
    out.put(' ');
    out.num(secs / 3600, 2);
    out.put(':');
    out.num(secs / 60 % 60, 2);
    out.put(':');
    out.num(secs % 60, 2);
    out.put('.');
    out.num(epochMs % 1000, 3);
    out.str(" [");
    out.num(pid);
    out.str(isCrash(sig) ? "] F crash: signal " : "] W terminated by signal ");
    out.num(sig);
    out.str(" (");
    out.str(signalName(sig));
    out.str(isCrash(sig) ? "), backtrace follows\n" : ")\n");
    buf[out.len] = '\0';
    return out.len;
}

QString logging::versionString() {
    return QStringLiteral("%1 (%2)").arg(QStringLiteral(SCOUT_VERSION), QLatin1String(commit()));
}
