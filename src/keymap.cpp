#include "keymap.h"

#include <QHash>
#include <QKeyEvent>
#include <QSet>

namespace {

const QHash<QString, QString> &namedKeys() {
    static const QHash<QString, QString> names = {
        {QStringLiteral("cr"), QStringLiteral("CR")},       {QStringLiteral("enter"), QStringLiteral("CR")},
        {QStringLiteral("return"), QStringLiteral("CR")},   {QStringLiteral("esc"), QStringLiteral("Esc")},
        {QStringLiteral("escape"), QStringLiteral("Esc")},  {QStringLiteral("space"), QStringLiteral("Space")},
        {QStringLiteral("tab"), QStringLiteral("Tab")},     {QStringLiteral("bs"), QStringLiteral("BS")},
        {QStringLiteral("backspace"), QStringLiteral("BS")}, {QStringLiteral("del"), QStringLiteral("Del")},
        {QStringLiteral("delete"), QStringLiteral("Del")},  {QStringLiteral("up"), QStringLiteral("Up")},
        {QStringLiteral("down"), QStringLiteral("Down")},   {QStringLiteral("left"), QStringLiteral("Left")},
        {QStringLiteral("right"), QStringLiteral("Right")}, {QStringLiteral("home"), QStringLiteral("Home")},
        {QStringLiteral("end"), QStringLiteral("End")},     {QStringLiteral("pageup"), QStringLiteral("PageUp")},
        {QStringLiteral("pagedown"), QStringLiteral("PageDown")}, {QStringLiteral("insert"), QStringLiteral("Insert")},
        {QStringLiteral("lt"), QStringLiteral("lt")},
    };
    return names;
}

QString keyName(int key) {
    switch (key) {
    case Qt::Key_Escape: return QStringLiteral("Esc");
    case Qt::Key_Return:
    case Qt::Key_Enter: return QStringLiteral("CR");
    case Qt::Key_Tab: return QStringLiteral("Tab");
    case Qt::Key_Backtab: return QStringLiteral("S-Tab");
    case Qt::Key_Backspace: return QStringLiteral("BS");
    case Qt::Key_Delete: return QStringLiteral("Del");
    case Qt::Key_Insert: return QStringLiteral("Insert");
    case Qt::Key_Space: return QStringLiteral("Space");
    case Qt::Key_Up: return QStringLiteral("Up");
    case Qt::Key_Down: return QStringLiteral("Down");
    case Qt::Key_Left: return QStringLiteral("Left");
    case Qt::Key_Right: return QStringLiteral("Right");
    case Qt::Key_Home: return QStringLiteral("Home");
    case Qt::Key_End: return QStringLiteral("End");
    case Qt::Key_PageUp: return QStringLiteral("PageUp");
    case Qt::Key_PageDown: return QStringLiteral("PageDown");
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    if (key >= 0x20 && key < 0x7f)
        return QString(QChar(key)).toLower();
    return {};
}

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta ||
           key == Qt::Key_Super_L || key == Qt::Key_Super_R || key == Qt::Key_AltGr || key == Qt::Key_CapsLock ||
           key == Qt::Key_NumLock;
}

} // namespace

QString KeyMap::normalizeToken(const QString &token) {
    if (token.size() < 3 || !token.startsWith(u'<') || !token.endsWith(u'>'))
        return token;
    QString inner = token.mid(1, token.size() - 2);
    QStringList mods;
    while (inner.size() > 2 && inner[1] == u'-') {
        const QChar m = inner[0].toUpper();
        if (m == u'C' || m == u'A' || m == u'S' || m == u'M')
            mods << QString(m == u'M' ? QChar(u'A') : m);
        else
            break;
        inner = inner.mid(2);
    }
    QString key = namedKeys().value(inner.toLower(), inner);
    if (key == QLatin1String("lt"))
        key = QStringLiteral("<");
    // <S-x> is just X.
    if (mods == QStringList{QStringLiteral("S")} && key.size() == 1)
        return key.toUpper();
    if (mods.isEmpty())
        return key.size() == 1 ? key : QStringLiteral("<%1>").arg(key);
    if (key.size() == 1 && key[0].isUpper()) {
        key = key.toLower();
        mods << QStringLiteral("S");
    }
    QStringList ordered;
    for (const auto &m : {QStringLiteral("C"), QStringLiteral("A"), QStringLiteral("S")})
        if (mods.contains(m))
            ordered << m;
    return QStringLiteral("<%1-%2>").arg(ordered.join(u'-'), key);
}

QStringList KeyMap::parseSequence(const QString &sequence) {
    QStringList tokens;
    for (int i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == u'<') {
            const int close = sequence.indexOf(u'>', i + 1);
            if (close > i + 1) {
                tokens << normalizeToken(sequence.mid(i, close - i + 1));
                i = close;
                continue;
            }
        }
        tokens << QString(sequence[i]);
    }
    return tokens;
}

QString KeyMap::tokenFor(const QKeyEvent *event) {
    const int key = event->key();
    if (isModifierKey(key) || key == 0 || key == Qt::Key_unknown)
        return {};
    const auto mods = event->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const QString text = event->text();
    const bool printable = !text.isEmpty() && text[0].isPrint() && !text[0].isSpace();

    if (!ctrl && !alt && printable)
        return text;

    QString name = keyName(key);
    if (name.isEmpty())
        return {};
    bool shifted = shift;
    if (name == QLatin1String("S-Tab")) {
        name = QStringLiteral("Tab");
        shifted = true;
    }
    QStringList parts;
    if (ctrl)
        parts << QStringLiteral("C");
    if (alt)
        parts << QStringLiteral("A");
    // Shift on a printable key is already part of the character.
    const bool namedKey = name.size() > 1;
    if (shifted && (namedKey || ctrl || alt))
        parts << QStringLiteral("S");
    if (parts.isEmpty())
        return name.size() == 1 ? name : QStringLiteral("<%1>").arg(name);
    return QStringLiteral("<%1-%2>").arg(parts.join(u'-'), name);
}

QString KeyMap::display(const QStringList &keys) {
    QString out;
    for (const QString &k : keys)
        out += k == QLatin1String("<Space>") ? QStringLiteral("␣") : k;
    return out;
}

void KeyMap::bind(const QString &sequence, const QString &command, const QString &description) {
    const QStringList keys = parseSequence(sequence);
    if (keys.isEmpty())
        return;
    m_bindings.insert(join(keys), Binding{keys, command, description});
}

void KeyMap::unbind(const QString &sequence) { m_bindings.remove(join(parseSequence(sequence))); }

void KeyMap::clear() {
    m_bindings.clear();
    reset();
}

void KeyMap::reset() {
    m_pending.clear();
    m_count = 0;
}

const Binding *KeyMap::exact(const QStringList &keys) const {
    auto it = m_bindings.constFind(join(keys));
    return it == m_bindings.constEnd() ? nullptr : &it.value();
}

bool KeyMap::hasLonger(const QStringList &keys) const {
    for (const Binding &b : m_bindings) {
        if (b.keys.size() > keys.size() && b.keys.mid(0, keys.size()) == keys)
            return true;
    }
    return false;
}

bool KeyMap::hasAmbiguousExact() const { return !m_pending.isEmpty() && exact(m_pending) && hasLonger(m_pending); }

KeyMap::Result KeyMap::feed(const QString &rawToken) {
    const QString token = normalizeToken(rawToken);
    if (token.isEmpty())
        return {Result::Pending, {}, m_count};

    if (m_pending.isEmpty() && token.size() == 1 && token[0].isDigit() && (m_count > 0 || token[0] != u'0') &&
        !exact({token}) && !hasLonger({token})) {
        m_count = std::min(m_count * 10 + token[0].digitValue(), 99999);
        return {Result::Pending, {}, m_count};
    }

    QStringList keys = m_pending;
    keys << token;
    const Binding *hit = exact(keys);
    const bool longer = hasLonger(keys);
    if (hit && !longer) {
        Result r{Result::Command, hit->command, m_count};
        reset();
        return r;
    }
    if (longer) {
        m_pending = keys;
        return {Result::Pending, {}, m_count};
    }
    reset();
    return {};
}

KeyMap::Result KeyMap::flush() {
    if (const Binding *hit = m_pending.isEmpty() ? nullptr : exact(m_pending)) {
        Result r{Result::Command, hit->command, m_count};
        reset();
        return r;
    }
    reset();
    return {};
}

QString KeyMap::pendingDisplay() const {
    QString out = m_count > 0 ? QString::number(m_count) : QString();
    return out + display(m_pending);
}

QList<Continuation> KeyMap::continuations() const {
    QMap<QString, Continuation> next;
    QHash<QString, int> prefixCounts;
    const int depth = int(m_pending.size());
    for (const Binding &b : m_bindings) {
        if (b.keys.size() <= depth || b.keys.mid(0, depth) != m_pending)
            continue;
        const QString key = b.keys[depth];
        if (b.keys.size() == depth + 1) {
            next[key] = Continuation{key, b.description.isEmpty() ? b.command : b.description, false};
        } else {
            ++prefixCounts[key];
            if (!next.contains(key))
                next[key] = Continuation{key, {}, true};
        }
    }
    QList<Continuation> out;
    for (auto it = next.begin(); it != next.end(); ++it) {
        Continuation c = it.value();
        if (c.isPrefix && c.label.isEmpty())
            c.label = QStringLiteral("+%1 more").arg(prefixCounts.value(c.key));
        out << c;
    }
    std::sort(out.begin(), out.end(), [](const Continuation &a, const Continuation &b) {
        const bool aLetter = a.key.size() == 1 && a.key[0].isLetter();
        const bool bLetter = b.key.size() == 1 && b.key[0].isLetter();
        if (aLetter != bLetter)
            return aLetter;
        if (aLetter && a.key.compare(b.key, Qt::CaseInsensitive) != 0)
            return a.key.compare(b.key, Qt::CaseInsensitive) < 0;
        return a.key < b.key;
    });
    return out;
}

QString KeyMap::keysFor(const QString &command) const {
    QString best;
    for (const Binding &b : m_bindings) {
        if (b.command != command)
            continue;
        const QString shown = display(b.keys);
        if (best.isEmpty() || shown.size() < best.size())
            best = shown;
    }
    return best;
}
