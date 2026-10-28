#pragma once

#include <QColor>
#include <QDBusVariant>
#include <QJsonObject>
#include <QFileSystemWatcher>
#include <QFont>
#include <QList>
#include <QObject>
#include <QString>

struct Palette {
    QString name;
    bool dark = true;
    QColor base;      // window background
    QColor mantle;    // side columns
    QColor crust;     // deepest background
    QColor surface;   // selection fill
    QColor overlay;   // borders and rules; never text
    QColor text;
    QColor subtext;   // secondary text, readable on base, mantle and surface
    QColor muted;     // tertiary text, readable on base and mantle only
    QColor accent;    // directories, focus
    QColor red;
    QColor green;
    QColor yellow;
    QColor orange;
    QColor magenta;
    QColor cyan;

    static Palette mocha();
    static Palette latte();
    static Palette fromBase16(const QJsonObject &scheme, const QString &name);

    // Backgrounds behind highlighted search matches and mode badges.
    QColor searchTint(const QColor &bg) const { return mix(bg, yellow, 0.16); }
    QColor badgeTint(const QColor &bg, const QColor &color) const { return mix(bg, color, 0.16); }

    // Every color that is drawn as text, paired with each background it is drawn on.
    struct TextRole {
        QString name;
        QColor fg;
        QColor bg;
    };
    QList<TextRole> textRoles() const;

    static QColor mix(const QColor &a, const QColor &b, double t);
    static double contrast(const QColor &a, const QColor &b);
    // `fg` moved toward `toward` just far enough to reach `ratio` against every background.
    static QColor readable(const QColor &fg, const QList<QColor> &backgrounds, const QColor &toward, double ratio = 4.6);

private:
    void ensureContrast();
};

enum class ThemeMode { Auto, Dark, Light };

// Process-wide theme. Follows, in order: an explicit `set theme` override, the
// Stylix base16 palette in ~/.config/stylix/palette.json, and the portal's
// light/dark preference.
class Theme : public QObject {
    Q_OBJECT
public:
    static Theme &instance();
    static const Palette &p() { return instance().m_palette; }

    void init();
    void setMode(ThemeMode mode);
    ThemeMode mode() const { return m_mode; }

    QFont font() const { return m_font; }
    void setFontFamily(const QString &family);
    void setFontSize(double points);
    void zoom(double deltaPoints);
    int rowHeight() const;

signals:
    void changed();

private slots:
    void onPortalSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value);

private:
    Theme() = default;
    void rebuild();
    void applyToApplication();
    void watchStylix();

    Palette m_palette = Palette::mocha();
    ThemeMode m_mode = ThemeMode::Auto;
    int m_portalScheme = 0; // 0 no preference, 1 dark, 2 light
    QFont m_font;
    QFileSystemWatcher m_watcher;
    QString m_stylixPath;
};

QString cssColor(const QColor &c);
