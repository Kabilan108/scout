#include "chooser.h"

#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRegularExpression>

bool FileFilter::accepts(const FileEntry &entry) const {
    if (rules.isEmpty())
        return true;
    static const QMimeDatabase db;
    QMimeType mime;
    for (const auto &[kind, pattern] : rules) {
        if (kind == 0) {
            if (QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive).match(entry.name).hasMatch())
                return true;
        } else {
            if (!mime.isValid())
                mime = db.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension);
            if (pattern == QLatin1String("*/*") || pattern == QLatin1String("*"))
                return true;
            if (pattern.endsWith(QLatin1String("/*"))) {
                const QString group = pattern.chopped(1);
                if (mime.name().startsWith(group))
                    return true;
                for (const QString &parent : mime.allAncestors())
                    if (parent.startsWith(group))
                        return true;
            } else if (mime.inherits(pattern)) {
                return true;
            }
        }
    }
    return false;
}

ChooserBar::ChooserBar(const ChooserRequest &request, QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(8);

    m_hint = new QLabel(this);
    layout->addWidget(m_hint);

    if (request.mode == ChooserRequest::Mode::Save) {
        layout->addSpacing(16);
        auto *label = new QLabel(QStringLiteral("Name"), this);
        layout->addWidget(label);
        m_name = new QLineEdit(this);
        m_name->setPlaceholderText(QStringLiteral("file name"));
        layout->addWidget(m_name, 1);
    } else {
        layout->addStretch(1);
    }

    if (!request.filters.isEmpty()) {
        m_filter = new QComboBox(this);
        m_filter->setFocusPolicy(Qt::ClickFocus);
        for (const FileFilter &f : request.filters)
            m_filter->addItem(f.name);
        m_filter->setCurrentIndex(std::clamp(request.currentFilter, 0, int(request.filters.size()) - 1));
        connect(m_filter, &QComboBox::currentIndexChanged, this, &ChooserBar::filterChanged);
        layout->addWidget(m_filter);
    }

    for (const ChooserChoice &c : request.choices) {
        if (c.options.isEmpty()) {
            auto *box = new QCheckBox(c.label, this);
            box->setFocusPolicy(Qt::ClickFocus);
            box->setChecked(c.selected == QLatin1String("true"));
            layout->addWidget(box);
            m_choiceWidgets.append({c.id, box});
        } else {
            if (!c.label.isEmpty())
                layout->addWidget(new QLabel(c.label, this));
            auto *combo = new QComboBox(this);
            combo->setFocusPolicy(Qt::ClickFocus);
            for (const auto &[id, label] : c.options)
                combo->addItem(label, id);
            const int index = combo->findData(c.selected);
            if (index >= 0)
                combo->setCurrentIndex(index);
            layout->addWidget(combo);
            m_choiceWidgets.append({c.id, combo});
        }
    }

    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_accept = new QPushButton(this);
    for (QPushButton *b : {m_cancel, m_accept}) {
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        layout->addWidget(b);
    }
    connect(m_cancel, &QPushButton::clicked, this, &ChooserBar::cancelClicked);
    connect(m_accept, &QPushButton::clicked, this, &ChooserBar::acceptClicked);
    applyTheme();
}

QString ChooserBar::name() const { return m_name ? m_name->text() : QString(); }

void ChooserBar::setName(const QString &name) {
    if (!m_name)
        return;
    m_name->setText(name);
    const int dot = int(name.lastIndexOf(u'.'));
    m_name->setSelection(0, dot > 0 ? dot : int(name.size()));
}

int ChooserBar::filterIndex() const { return m_filter ? m_filter->currentIndex() : -1; }

void ChooserBar::cycleFilter() {
    if (m_filter && m_filter->count() > 1)
        m_filter->setCurrentIndex((m_filter->currentIndex() + 1) % m_filter->count());
}

QList<QPair<QString, QString>> ChooserBar::choices() const {
    QList<QPair<QString, QString>> out;
    for (const auto &[id, widget] : m_choiceWidgets) {
        if (auto *box = qobject_cast<QCheckBox *>(widget))
            out.append({id, box->isChecked() ? QStringLiteral("true") : QStringLiteral("false")});
        else if (auto *combo = qobject_cast<QComboBox *>(widget))
            out.append({id, combo->currentData().toString()});
    }
    return out;
}

void ChooserBar::setHint(const QString &hint) { m_hint->setText(hint); }

void ChooserBar::setAcceptText(const QString &text) { m_accept->setText(text); }

void ChooserBar::applyTheme() {
    const Palette &p = Theme::p();
    setFont(Theme::instance().font());
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, p.mantle);
    setPalette(pal);
    m_hint->setStyleSheet(QStringLiteral("color: %1;").arg(p.subtext.name()));
    const QString button = QStringLiteral(
        "QPushButton { background: %1; color: %2; border: none; border-radius: 4px; padding: 4px 14px; }"
        "QPushButton:hover { background: %3; }");
    if (m_name)
        m_name->setStyleSheet(QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 4px;"
                                             " padding: 3px 8px; selection-background-color: %4; selection-color: %1; }"
                                             "QLineEdit:focus { border-color: %4; }")
                                  .arg(p.base.name(), p.text.name(), p.overlay.name(), p.accent.name()));
    m_cancel->setStyleSheet(button.arg(p.surface.name(), p.text.name(), p.overlay.name()));
    m_accept->setStyleSheet(button.arg(p.accent.name(), p.crust.name(), p.accent.lighter(115).name()) +
                            QStringLiteral("QPushButton { font-weight: bold; }"));
}
