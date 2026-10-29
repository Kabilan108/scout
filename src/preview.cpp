#include "preview.h"

#include "theme.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTextBlock>
#include <QtConcurrent>

#include <archive.h>
#include <archive_entry.h>

namespace {

constexpr qint64 kTextBytes = 128 * 1024;
constexpr int kTextLines = 3000;
constexpr int kHexBytes = 4096;
constexpr int kArchiveEntries = 2000;

QString formatTime(qint64 secs) {
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

bool inherits(const QMimeType &mime, const QStringList &names) {
    for (const QString &n : names)
        if (mime.inherits(n))
            return true;
    return false;
}

bool isArchiveMime(const QMimeType &mime) {
    static const QStringList names = {
        QStringLiteral("application/zip"),          QStringLiteral("application/x-tar"),
        QStringLiteral("application/x-compressed-tar"), QStringLiteral("application/x-bzip-compressed-tar"),
        QStringLiteral("application/x-xz-compressed-tar"), QStringLiteral("application/x-zstd-compressed-tar"),
        QStringLiteral("application/x-7z-compressed"), QStringLiteral("application/vnd.rar"),
        QStringLiteral("application/x-rar"),        QStringLiteral("application/x-cpio"),
        QStringLiteral("application/vnd.debian.binary-package"), QStringLiteral("application/x-rpm"),
        QStringLiteral("application/x-iso9660-image"), QStringLiteral("application/x-archive"),
        QStringLiteral("application/x-lzma-compressed-tar"), QStringLiteral("application/x-tarz"),
    };
    return inherits(mime, names);
}

bool isTextMime(const QMimeType &mime) {
    static const QStringList names = {
        QStringLiteral("text/plain"),          QStringLiteral("application/json"),
        QStringLiteral("application/xml"),     QStringLiteral("application/javascript"),
        QStringLiteral("application/x-shellscript"), QStringLiteral("application/toml"),
        QStringLiteral("application/x-yaml"),  QStringLiteral("application/yaml"),
        QStringLiteral("application/x-desktop"), QStringLiteral("application/sql"),
        QStringLiteral("application/x-wine-extension-ini"), QStringLiteral("application/x-zerosize"),
    };
    return inherits(mime, names);
}

bool looksBinary(const QByteArray &data) { return data.left(8192).contains('\0'); }

QString hexDump(const QByteArray &data) {
    QString out;
    out.reserve(data.size() * 4);
    for (int off = 0; off < data.size(); off += 16) {
        out += QStringLiteral("%1  ").arg(off, 8, 16, QLatin1Char('0'));
        QString ascii;
        for (int i = 0; i < 16; ++i) {
            if (off + i < data.size()) {
                const auto b = static_cast<unsigned char>(data[off + i]);
                out += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QChar(u'.');
            } else {
                out += QStringLiteral("   ");
            }
            if (i == 7)
                out += u' ';
        }
        out += QStringLiteral(" |") + ascii + QStringLiteral("|\n");
    }
    return out;
}

QString listArchive(const QString &path) {
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    const QByteArray enc = QFile::encodeName(path);
    if (archive_read_open_filename(a, enc.constData(), 1 << 16) != ARCHIVE_OK) {
        const QString err = QString::fromLocal8Bit(archive_error_string(a));
        archive_read_free(a);
        return QStringLiteral("could not read archive: ") + err;
    }
    QStringList lines;
    qint64 total = 0;
    int count = 0;
    bool truncated = false;
    archive_entry *entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (count >= kArchiveEntries) {
            truncated = true;
            break;
        }
        const char *utf8 = archive_entry_pathname_utf8(entry);
        const QString name = utf8 ? QString::fromUtf8(utf8) : QFile::decodeName(archive_entry_pathname(entry));
        const bool isDir = archive_entry_filetype(entry) == AE_IFDIR;
        const qint64 size = archive_entry_size(entry);
        total += size;
        lines << QStringLiteral("%1  %2").arg(isDir ? QString() : humanSize(size), 8).arg(name);
        ++count;
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    QString header = QStringLiteral("%1%2 entries · %3 unpacked\n\n")
                         .arg(truncated ? QStringLiteral("first ") : QString())
                         .arg(count)
                         .arg(humanSize(total));
    return header + lines.join(u'\n');
}

struct LoadedImage {
    QImage image;
    QSize original;
    QString error;
};

LoadedImage loadImage(const QString &path, QSize target) {
    LoadedImage out;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    out.original = reader.size();
    if (out.original.isValid() && target.isValid() &&
        (out.original.width() > target.width() || out.original.height() > target.height())) {
        QSize scaled = out.original;
        // Orientation may swap axes; fit against the larger box to be safe.
        scaled.scale(target.expandedTo(target.transposed()), Qt::KeepAspectRatio);
        reader.setScaledSize(scaled);
    }
    out.image = reader.read();
    if (out.image.isNull())
        out.error = reader.errorString();
    return out;
}

} // namespace

void ImageView::setImage(const QImage &image, const QString &path, const QString &caption) {
    m_image = image;
    m_path = path;
    m_caption = caption;
    update();
}

void ImageView::clear() {
    m_image = {};
    m_path.clear();
    m_caption.clear();
    update();
}

void ImageView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    const QFontMetrics fm(font());
    const QRect area = rect().adjusted(10, 10, -10, -(fm.height() + 20));
    if (!m_image.isNull()) {
        const qreal dpr = devicePixelRatioF();
        QSizeF size = QSizeF(m_image.size()) / dpr;
        if (size.width() > area.width() || size.height() > area.height())
            size.scale(area.size(), Qt::KeepAspectRatio);
        const QRectF target(area.x() + (area.width() - size.width()) / 2,
                            area.y() + (area.height() - size.height()) / 2, size.width(), size.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, m_image);
    }
    p.setPen(pal.subtext);
    p.drawText(QRect(10, height() - fm.height() - 12, width() - 20, fm.height() + 4), Qt::AlignCenter,
               fm.elidedText(m_caption, Qt::ElideMiddle, width() - 20));
}

void ImageView::mousePressEvent(QMouseEvent *event) {
    m_pressed = event->button() == Qt::LeftButton && !m_path.isEmpty();
    m_pressPos = event->position().toPoint();
}

void ImageView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_pressed || (event->position().toPoint() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
        return;
    m_pressed = false;
    startFileDrag(this, {m_path});
}

void InfoView::setEntry(const FileEntry &entry, const QString &mimeDescription, const QString &note) {
    m_entry = entry;
    m_mime = mimeDescription;
    m_note = note;
    update();
}

void InfoView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const Palette &pal = Theme::p();
    p.fillRect(rect(), pal.base);
    if (m_entry.path.isEmpty())
        return;
    const QFontMetrics fm(font());
    const int iconSize = std::min(72, width() / 4);
    int y = std::max(24, height() / 5);
    paintEntryIcon(&p, QRectF((width() - iconSize) / 2.0, y, iconSize, iconSize), entryKind(m_entry));
    y += iconSize + 16;
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(pal.text);
    p.drawText(QRect(10, y, width() - 20, fm.height() + 4), Qt::AlignHCenter,
               QFontMetrics(bold).elidedText(m_entry.name, Qt::ElideMiddle, width() - 20));
    y += fm.height() + 10;
    p.setFont(font());
    QStringList lines;
    if (!m_mime.isEmpty())
        lines << m_mime;
    if (!m_entry.isDir)
        lines << humanSize(m_entry.size);
    lines << formatTime(m_entry.mtime) << permissionString(m_entry);
    if (m_entry.isSymlink)
        lines << QStringLiteral("→ ") + m_entry.linkTarget;
    if (!m_note.isEmpty())
        lines << QString() << m_note;
    p.setPen(pal.subtext);
    for (const QString &line : lines) {
        p.drawText(QRect(10, y, width() - 20, fm.height() + 2), Qt::AlignHCenter,
                   fm.elidedText(line, Qt::ElideMiddle, width() - 20));
        y += fm.height() + 4;
    }
}

Preview::Preview(Marks *marks, QWidget *parent) : QStackedWidget(parent) {
    m_dirModel = new DirModel(marks, this);
    m_dirView = new ColumnView(m_dirModel, ColumnView::Role::Preview, this);

    m_text = new CodeView(this);
    m_text->setReadOnly(true);
    m_text->setFocusPolicy(Qt::NoFocus);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setTextInteractionFlags(Qt::NoTextInteraction);
    m_text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_text->viewport()->setCursor(Qt::ArrowCursor);

    m_image = new ImageView(this);
    m_info = new InfoView(this);
    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_text->document());

    addWidget(m_info);
    addWidget(m_dirView);
    addWidget(m_text);
    addWidget(m_image);

    connect(m_dirModel, &DirModel::refreshed, this, [this] {
        if (m_dirCursorHint.isEmpty())
            return;
        const int row = m_dirModel->rowOf(m_dirCursorHint);
        m_dirView->setShowCursor(row >= 0);
        if (row >= 0)
            m_dirView->setCurrentRow(row);
    });
    applyTheme();
}

void Preview::applyTheme() {
    const Palette &pal = Theme::p();
    QPalette tp = m_text->palette();
    tp.setColor(QPalette::Base, pal.base);
    tp.setColor(QPalette::Text, pal.text);
    m_text->setPalette(tp);
    m_text->viewport()->setPalette(tp);
    m_text->setFont(Theme::instance().font());
    m_text->setTabStopDistance(QFontMetricsF(Theme::instance().font()).horizontalAdvance(u' ') * 4);

    const bool catppuccin = pal.name.contains(QLatin1String("atppuccin"), Qt::CaseInsensitive);
    KSyntaxHighlighting::Theme theme;
    if (catppuccin)
        theme = m_repo.theme(pal.dark ? QStringLiteral("Catppuccin Mocha") : QStringLiteral("Catppuccin Latte"));
    if (!theme.isValid())
        theme = m_repo.defaultTheme(pal.dark ? KSyntaxHighlighting::Repository::DarkTheme
                                             : KSyntaxHighlighting::Repository::LightTheme);
    m_highlighter->setTheme(theme);
    m_highlighter->rehighlight();
    m_info->setFont(Theme::instance().font());
    m_image->setFont(Theme::instance().font());
    update();
}

void Preview::setOptions(const ViewOptions &options) { m_dirModel->setOptions(options); }

void Preview::setLineNumbers(bool on) {
    m_lineNumbers = on;
    m_text->setLineNumbers(m_textFile && on);
}

QString Preview::text() const { return m_kind == Kind::Text ? m_text->toPlainText() : QString(); }

QSize Preview::imageTargetSize() const {
    const qreal dpr = devicePixelRatioF();
    QSize s = size() * dpr;
    if (s.width() < 400 || s.height() < 300)
        s = QSize(1600, 1200);
    return s.boundedTo(QSize(2560, 2560));
}

void Preview::clear() {
    ++m_generation;
    m_kind = Kind::None;
    m_path.clear();
    if (m_process)
        m_process->kill();
    m_dirModel->clearPath();
    m_image->clear();
    m_info->setEntry({}, {});
    setCurrentWidget(m_info);
}

void Preview::showEntry(const FileEntry &entry) {
    if (m_path == entry.path && m_kind != Kind::None) {
        if (m_kind == Kind::Directory)
            m_dirModel->setPath(entry.path);
        return;
    }
    ++m_generation;
    if (m_process)
        m_process->kill();
    m_path = entry.path;

    if (entry.isDir) {
        m_kind = Kind::Directory;
        m_dirView->setShowCursor(false);
        m_dirModel->setPath(entry.path);
        setCurrentWidget(m_dirView);
        return;
    }
    if (m_dirModel->isLoaded() || m_dirModel->isLoading())
        m_dirModel->clearPath();

    if (entry.isBrokenLink) {
        showInfo(entry, QStringLiteral("broken symbolic link"));
        return;
    }
    // Reading a FIFO or device would block the UI thread.
    if (!entry.isRegular) {
        showInfo(entry, QStringLiteral("special file"));
        return;
    }
    QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(entry.path);
    if (entry.size == 0) {
        showInfo(entry, QStringLiteral("empty file"));
        return;
    }
    static const QList<QByteArray> imageMimes = QImageReader::supportedMimeTypes();
    if (imageMimes.contains(mime.name().toLatin1()) || (mime.name().startsWith(QLatin1String("image/")) &&
                                                       !QImageReader::imageFormat(entry.path).isEmpty())) {
        showImageAsync(entry);
        return;
    }
    if (mime.inherits(QStringLiteral("application/pdf")) && !QStandardPaths::findExecutable(QStringLiteral("pdftoppm")).isEmpty()) {
        const QSize t = imageTargetSize();
        showProcessImage(entry, QStringLiteral("pdftoppm"),
                         {QStringLiteral("-png"), QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
                          QStringLiteral("1"), QStringLiteral("-scale-to"), QString::number(std::max(t.width(), t.height())),
                          QStringLiteral("-singlefile"), entry.path, QStringLiteral("-")});
        return;
    }
    if (mime.name().startsWith(QLatin1String("video/")) &&
        !QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer")).isEmpty()) {
        showProcessImage(entry, QStringLiteral("ffmpegthumbnailer"),
                         {QStringLiteral("-i"), entry.path, QStringLiteral("-o"), QStringLiteral("-"),
                          QStringLiteral("-c"), QStringLiteral("png"), QStringLiteral("-s"),
                          QString::number(imageTargetSize().width())});
        return;
    }
    if (isArchiveMime(mime)) {
        showArchiveAsync(entry);
        return;
    }
    if (mime.name().startsWith(QLatin1String("audio/")) || mime.name().startsWith(QLatin1String("font/"))) {
        showInfo(entry, mime.comment());
        return;
    }

    QFile f(entry.path);
    if (!f.open(QIODevice::ReadOnly)) {
        showInfo(entry, f.errorString());
        return;
    }
    const QByteArray data = f.read(kTextBytes);
    if (!looksBinary(data)) {
        QStringDecoder decoder(QStringDecoder::Utf8);
        QString text = decoder.decode(data);
        // A chunk boundary can split one multibyte character; tolerate that.
        if (!decoder.hasError() || data.size() == kTextBytes || isTextMime(mime)) {
            int lines = 0;
            qsizetype cut = -1;
            for (qsizetype i = 0; i < text.size(); ++i) {
                if (text[i] == u'\n' && ++lines >= kTextLines) {
                    cut = i;
                    break;
                }
            }
            if (cut >= 0)
                text.truncate(cut);
            m_kind = Kind::Text;
            showText(text, entry.path, true);
            return;
        }
    }
    m_kind = Kind::Text;
    showText(hexDump(data.left(kHexBytes)), entry.path, false);
}

void Preview::showText(const QString &text, const QString &path, bool highlight) {
    KSyntaxHighlighting::Definition def;
    if (highlight) {
        def = m_repo.definitionForFileName(path);
        if (!def.isValid())
            def = m_repo.definitionForMimeType(QMimeDatabase().mimeTypeForFile(path).name());
    }
    m_textFile = highlight;
    m_text->setLineNumbers(highlight && m_lineNumbers);
    replaceText(text, def);
    m_text->verticalScrollBar()->setValue(0);
    setCurrentWidget(m_text);
}

void Preview::replaceText(const QString &text, const KSyntaxHighlighting::Definition &def) {
    // KSyntaxHighlighting queues rehighlightBlock() calls that hold blocks of the
    // current text; they must not outlive it. Emptying the document before the
    // definition changes also keeps that change from re-highlighting old text.
    QCoreApplication::removePostedEvents(m_highlighter, QEvent::MetaCall);
    m_text->clear();
    m_highlighter->setDefinition(def);
    m_text->setPlainText(text);
}

void Preview::showInfo(const FileEntry &entry, const QString &note) {
    m_kind = Kind::Info;
    QMimeDatabase db;
    m_info->setEntry(entry, entry.isBrokenLink ? QString() : db.mimeTypeForFile(entry.path).comment(), note);
    setCurrentWidget(m_info);
}

void Preview::showImageAsync(const FileEntry &entry) {
    m_kind = Kind::Image;
    const quint64 generation = m_generation;
    const QString path = entry.path;
    const QString captionBase = QStringLiteral("%1 · %2 · %3").arg(entry.name, humanSize(entry.size), formatTime(entry.mtime));
    m_image->setImage({}, path, captionBase);
    setCurrentWidget(m_image);
    QPointer<Preview> self(this);
    QtConcurrent::run([path, target = imageTargetSize()] { return loadImage(path, target); })
        .then(this, [self, generation, path, captionBase, entry](LoadedImage loaded) {
            if (!self || generation != self->m_generation)
                return;
            if (loaded.image.isNull()) {
                self->showInfo(entry, loaded.error);
                return;
            }
            const QString caption = loaded.original.isValid()
                                        ? QStringLiteral("%1 · %2×%3").arg(captionBase).arg(loaded.original.width()).arg(loaded.original.height())
                                        : captionBase;
            self->m_image->setImage(loaded.image, path, caption);
        });
}

void Preview::showArchiveAsync(const FileEntry &entry) {
    m_kind = Kind::Text;
    const quint64 generation = m_generation;
    m_textFile = false;
    m_text->setLineNumbers(false);
    replaceText(QStringLiteral("reading archive…"), {});
    setCurrentWidget(m_text);
    QPointer<Preview> self(this);
    QtConcurrent::run([path = entry.path] { return listArchive(path); }).then(this, [self, generation](QString listing) {
        if (!self || generation != self->m_generation)
            return;
        self->replaceText(listing, {});
    });
}

void Preview::showProcessImage(const FileEntry &entry, const QString &program, const QStringList &args) {
    m_kind = Kind::Image;
    const quint64 generation = m_generation;
    const QString caption = QStringLiteral("%1 · %2 · %3").arg(entry.name, humanSize(entry.size), formatTime(entry.mtime));
    m_image->setImage({}, entry.path, caption);
    setCurrentWidget(m_image);
    auto *proc = new QProcess(this);
    m_process = proc;
    connect(proc, &QProcess::finished, this, [this, proc, generation, entry, caption](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (generation != m_generation)
            return;
        QImage image;
        if (status == QProcess::NormalExit && code == 0)
            image.loadFromData(proc->readAllStandardOutput());
        if (image.isNull()) {
            showInfo(entry, QStringLiteral("no preview available"));
            return;
        }
        m_image->setImage(image, entry.path, caption);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, generation, entry](QProcess::ProcessError) {
        proc->deleteLater();
        if (generation == m_generation)
            showInfo(entry, QStringLiteral("no preview available"));
    });
    proc->start(program, args);
}

namespace {

class LineGutter : public QWidget {
public:
    explicit LineGutter(CodeView *view) : QWidget(view), m_view(view) {}

protected:
    void paintEvent(QPaintEvent *event) override { m_view->paintGutter(event); }

private:
    CodeView *m_view;
};

} // namespace

CodeView::CodeView(QWidget *parent) : QPlainTextEdit(parent), m_gutter(new LineGutter(this)) {
    m_gutter->hide();
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeView::updateMargins);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (!m_lineNumbers)
            return;
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    });
}

void CodeView::setLineNumbers(bool on) {
    if (on == m_lineNumbers)
        return;
    m_lineNumbers = on;
    m_gutter->setVisible(on);
    updateMargins();
}

int CodeView::gutterWidth() const {
    if (!m_lineNumbers)
        return 0;
    const int digits = std::max(3, int(QString::number(std::max(1, blockCount())).size()));
    const QFontMetrics fm(font());
    return fm.horizontalAdvance(u'9') * digits + fm.horizontalAdvance(u' ') * 2;
}

void CodeView::updateMargins() {
    setViewportMargins(gutterWidth(), 0, 0, 0);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(cr.left(), cr.top(), gutterWidth(), cr.height());
}

void CodeView::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    updateMargins();
}

void CodeView::changeEvent(QEvent *event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateMargins();
}

void CodeView::paintGutter(QPaintEvent *event) {
    QPainter p(m_gutter);
    const Palette &pal = Theme::p();
    p.fillRect(event->rect(), pal.base);
    p.setFont(font());
    p.setPen(pal.muted);
    const int right = m_gutter->width() - QFontMetrics(font()).horizontalAdvance(u' ');
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top())
            p.drawText(0, top, right, height, Qt::AlignRight | Qt::AlignTop, QString::number(block.blockNumber() + 1));
        block = block.next();
        top += height;
    }
}
