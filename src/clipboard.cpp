#include "clipboard.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QUrl>

namespace {
const QString kGnomeCopied = QStringLiteral("x-special/gnome-copied-files");
const QString kKdeCut = QStringLiteral("application/x-kde-cutselection");
} // namespace

QMimeData *makeFileMimeData(const QStringList &paths, bool cut) {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString &p : paths) {
        const QUrl url = QUrl::fromLocalFile(p);
        urls << url;
        gnome += '\n' + url.toEncoded();
    }
    data->setUrls(urls);
    data->setData(kGnomeCopied, gnome);
    if (cut)
        data->setData(kKdeCut, "1");
    data->setText(paths.join(u'\n'));
    return data;
}

ClipboardFiles parseFileMimeData(const QMimeData *data) {
    ClipboardFiles out;
    if (!data)
        return out;
    if (data->hasFormat(kGnomeCopied)) {
        const QList<QByteArray> lines = data->data(kGnomeCopied).split('\n');
        if (!lines.isEmpty()) {
            out.cut = lines.first().trimmed() == "cut";
            for (int i = 1; i < lines.size(); ++i) {
                const QUrl url = QUrl::fromEncoded(lines[i].trimmed());
                if (url.isLocalFile())
                    out.paths << url.toLocalFile();
            }
        }
        if (!out.paths.isEmpty())
            return out;
    }
    if (data->hasUrls()) {
        for (const QUrl &url : data->urls())
            if (url.isLocalFile())
                out.paths << url.toLocalFile();
        out.cut = data->data(kKdeCut) == "1";
        if (!out.paths.isEmpty())
            return out;
    }
    if (data->hasText()) {
        for (const QString &line : data->text().split(u'\n', Qt::SkipEmptyParts)) {
            const QString trimmed = line.trimmed();
            QString path;
            if (trimmed.startsWith(QLatin1String("file://")))
                path = QUrl(trimmed).toLocalFile();
            else if (trimmed.startsWith(u'/'))
                path = trimmed;
            if (path.isEmpty() || !QFileInfo::exists(path))
                return {};
            out.paths << path;
        }
    }
    return out;
}

void setClipboardFiles(const QStringList &paths, bool cut) {
    QGuiApplication::clipboard()->setMimeData(makeFileMimeData(paths, cut));
}

ClipboardFiles clipboardFiles() { return parseFileMimeData(QGuiApplication::clipboard()->mimeData()); }

void setClipboardText(const QString &text) { QGuiApplication::clipboard()->setText(text); }
