#include "theme.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace {

QColor hex(const char *s) { return QColor(QString::fromLatin1(s)); }

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kPortalSettings = "org.freedesktop.portal.Settings";

int unwrapScheme(QVariant v) {
    while (v.canConvert<QDBusVariant>() && v.userType() == qMetaTypeId<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v.toInt();
}

} // namespace

QString cssColor(const QColor &c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF());
}

Palette Palette::mocha() {
    Palette p;
    p.name = QStringLiteral("catppuccin-mocha");
    p.dark = true;
    p.base = hex("#1e1e2e");
    p.mantle = hex("#181825");
    p.crust = hex("#11111b");
    p.surface = hex("#313244");
    p.overlay = hex("#45475a");
    p.text = hex("#cdd6f4");
    p.subtext = hex("#a6adc8");
    p.muted = hex("#6c7086");
    p.accent = hex("#89b4fa");
    p.red = hex("#f38ba8");
    p.green = hex("#a6e3a1");
    p.yellow = hex("#f9e2af");
    p.orange = hex("#fab387");
    p.magenta = hex("#cba6f7");
    p.cyan = hex("#94e2d5");
    return p;
}

Palette Palette::latte() {
    Palette p;
    p.name = QStringLiteral("catppuccin-latte");
    p.dark = false;
    p.base = hex("#eff1f5");
    p.mantle = hex("#e6e9ef");
    p.crust = hex("#dce0e8");
    p.surface = hex("#ccd0da");
    p.overlay = hex("#bcc0cc");
    p.text = hex("#4c4f69");
    p.subtext = hex("#6c6f85");
    p.muted = hex("#9ca0b0");
    p.accent = hex("#1e66f5");
    p.red = hex("#d20f39");
    p.green = hex("#40a02b");
    p.yellow = hex("#df8e1d");
    p.orange = hex("#fe640b");
    p.magenta = hex("#8839ef");
    p.cyan = hex("#179299");
    return p;
}

Palette Palette::fromBase16(const QJsonObject &scheme, const QString &name) {
    auto c = [&](const char *key) {
        QString v = scheme.value(QLatin1String(key)).toString();
        if (!v.startsWith(u'#'))
            v.prepend(u'#');
        return QColor(v);
    };
    Palette p;
    p.name = name;
    p.base = c("base00");
    p.dark = p.base.lightnessF() < 0.5;
    p.mantle = c("base01");
    p.crust = p.dark ? p.mantle.darker(115) : p.mantle.darker(105);
    p.surface = c("base02");
    p.overlay = c("base03");
    p.text = c("base05");
    p.subtext = c("base04");
    p.muted = c("base03");
    p.red = c("base08");
    p.orange = c("base09");
    p.yellow = c("base0A");
    p.green = c("base0B");
    p.cyan = c("base0C");
    p.accent = c("base0D");
    p.magenta = c("base0E");
    // base04 is often too close to base03 in light schemes; nudge it toward text.
    if (!p.dark)
        p.subtext = QColor::fromRgbF((p.subtext.redF() + p.text.redF()) / 2,
                                     (p.subtext.greenF() + p.text.greenF()) / 2,
                                     (p.subtext.blueF() + p.text.blueF()) / 2);
    return p;
}

Theme &Theme::instance() {
    static Theme theme;
    return theme;
}

void Theme::init() {
    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (m_font.pointSizeF() < 9)
        m_font.setPointSizeF(10.5);

    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        // Asynchronous on purpose: as a portal backend, scout starts while
        // xdg-desktop-portal is blocked waiting for it, so a blocking call
        // (or QDBusInterface's introspection) would deadlock until timeout.
        auto read = [this, bus](const QString &method, auto &&self) -> void {
            QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kPortalService), QLatin1String(kPortalPath),
                                                               QLatin1String(kPortalSettings), method);
            call << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("color-scheme");
            auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 2000), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method, self](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusPendingReply<QDBusVariant> reply = *w;
                if (reply.isValid()) {
                    m_portalScheme = unwrapScheme(reply.value().variant());
                    rebuild();
                } else if (method == QLatin1String("ReadOne")) {
                    self(QStringLiteral("Read"), self);
                }
            });
        };
        read(QStringLiteral("ReadOne"), read);
        bus.connect(QLatin1String(kPortalService), QLatin1String(kPortalPath), QLatin1String(kPortalSettings),
                    QStringLiteral("SettingChanged"), this,
                    SLOT(onPortalSettingChanged(QString, QString, QDBusVariant)));
    }

    m_stylixPath = QDir::homePath() + QStringLiteral("/.config/stylix/palette.json");
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { watchStylix(); rebuild(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { watchStylix(); rebuild(); });
    watchStylix();
    rebuild();
}

void Theme::watchStylix() {
    const QFileInfo info(m_stylixPath);
    if (info.dir().exists() && !m_watcher.directories().contains(info.absolutePath()))
        m_watcher.addPath(info.absolutePath());
    if (info.exists() && !m_watcher.files().contains(m_stylixPath))
        m_watcher.addPath(m_stylixPath);
}

void Theme::onPortalSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value) {
    if (ns != QLatin1String("org.freedesktop.appearance") || key != QLatin1String("color-scheme"))
        return;
    m_portalScheme = unwrapScheme(value.variant());
    rebuild();
}

void Theme::setMode(ThemeMode mode) {
    m_mode = mode;
    rebuild();
}

void Theme::rebuild() {
    Palette next;
    if (m_mode == ThemeMode::Dark) {
        next = Palette::mocha();
    } else if (m_mode == ThemeMode::Light) {
        next = Palette::latte();
    } else {
        QFile f(m_stylixPath);
        QJsonObject scheme;
        if (f.open(QIODevice::ReadOnly))
            scheme = QJsonDocument::fromJson(f.readAll()).object();
        if (scheme.contains(QLatin1String("base00")) && scheme.contains(QLatin1String("base0D")))
            next = Palette::fromBase16(scheme, scheme.value(QLatin1String("scheme")).toString());
        else
            next = m_portalScheme == 2 ? Palette::latte() : Palette::mocha();
    }
    m_palette = next;
    applyToApplication();
    emit changed();
}

void Theme::setFontFamily(const QString &family) {
    m_font.setFamily(family);
    applyToApplication();
    emit changed();
}

void Theme::setFontSize(double points) {
    m_font.setPointSizeF(std::clamp(points, 6.0, 32.0));
    applyToApplication();
    emit changed();
}

void Theme::zoom(double deltaPoints) { setFontSize(m_font.pointSizeF() + deltaPoints); }

int Theme::rowHeight() const { return QFontMetrics(m_font).height() + 6; }

void Theme::applyToApplication() {
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return;
    if (!app->style() || app->style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
        app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    const Palette &p = m_palette;
    QPalette pal;
    pal.setColor(QPalette::Window, p.base);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.base);
    pal.setColor(QPalette::AlternateBase, p.mantle);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::PlaceholderText, p.muted);
    pal.setColor(QPalette::Button, p.surface);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, p.base);
    pal.setColor(QPalette::ToolTipBase, p.mantle);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::Mid, p.overlay);
    pal.setColor(QPalette::Dark, p.crust);
    pal.setColor(QPalette::Light, p.surface);
    pal.setColor(QPalette::Link, p.accent);
    pal.setColor(QPalette::Disabled, QPalette::Text, p.muted);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.muted);
    app->setPalette(pal);
    app->setFont(m_font);

    app->setStyleSheet(QStringLiteral(R"(
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
QScrollBar::handle:vertical { background: %1; border-radius: 3px; min-height: 24px; margin: 1px; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }
QScrollBar::handle:horizontal { background: %1; border-radius: 3px; min-width: 24px; margin: 1px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QLineEdit { background: transparent; border: none; color: %2; selection-background-color: %3; selection-color: %4; }
QToolTip { background: %5; color: %2; border: 1px solid %1; padding: 4px; }
QMenu { background: %5; color: %2; border: 1px solid %1; padding: 4px; }
QMenu::item { padding: 4px 18px; border-radius: 4px; }
QMenu::item:selected { background: %6; }
QMenu::separator { height: 1px; background: %1; margin: 4px 6px; }
QSplitter::handle { background: %1; }
)")
                           .arg(cssColor(p.overlay), cssColor(p.text), cssColor(p.accent), cssColor(p.base),
                                cssColor(p.mantle), cssColor(p.surface)));
}
