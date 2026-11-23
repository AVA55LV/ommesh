#ifndef OMMESH_BELLE_APPEARANCE_HPP
#define OMMESH_BELLE_APPEARANCE_HPP

#include <QtCore/QSettings>
#include <QtGui/QApplication>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtGui/QWidget>

namespace Appearance {

enum Theme {
    Graphite, Apricot, Grape, Pebble, Lagoon, Mint, Olive, Onyx,
    Twilight, Berry, Catppuccin, OneDark, OneLight, System, ThemeCount
};

enum { MinimumChatTextSize = 12, DefaultChatTextSize = 15, MaximumChatTextSize = 22 };

struct Palette {
    QColor canvas;
    QColor chrome;
    QColor surface;
    QColor border;
    QColor text;
    QColor muted;
    QColor accent;
    QColor onAccent;
    QColor selection;
    QColor danger;
    bool dark;
};

struct Preferences {
    Theme theme;
    int chatTextSize;
};

inline Theme checkedTheme(int theme) {
    return theme >= Graphite && theme < ThemeCount ? static_cast<Theme>(theme) : Graphite;
}

inline int checkedChatTextSize(int size) {
    return qBound(int(MinimumChatTextSize), size, int(MaximumChatTextSize));
}

inline Preferences preferences(const QSettings& settings) {
    bool validSize = false;
    const int size = settings.value(QLatin1String("ui/chatTextSize"), DefaultChatTextSize).toInt(&validSize);
    return {checkedTheme(settings.value(QLatin1String("ui/theme"), Graphite).toInt()),
            validSize ? checkedChatTextSize(size) : int(DefaultChatTextSize)};
}

inline Preferences preferences() { return preferences(QSettings()); }

inline QString themeName(Theme theme) {
    static const char* const names[] = {
        "Графит", "Абрикос", "Виноград", "Галька", "Лагуна", "Мята",
        "Олива", "Оникс", "Сумерки", "Ягода", "Catppuccin", "One Dark", "One Light",
        "Как в системе"
    };
    return QString::fromUtf8(names[checkedTheme(theme)]);
}

inline Palette palette(Theme theme) {
    if (theme == System) {
        const QColor background = QApplication::palette().color(QPalette::Window);
        theme = qGray(background.rgb()) < 128 ? Graphite : OneLight;
    }
    static const QRgb colors[][9] = {
        {0x151c22, 0x1b262d, 0x233239, 0x3a5158, 0xeaf1f4, 0xafbec0, 0x62d5be, 0x142721, 0x26413f}, // Graphite
        {0xfff4e8, 0xf7e9d9, 0xf9e4d0, 0xb69784, 0x3d2e26, 0x624b3c, 0x984526, 0xffffff, 0xf3d9c4}, // Apricot
        {0x211b2b, 0x2b2237, 0x362a46, 0x695780, 0xf4ebfa, 0xc6b4d6, 0xc5a3f2, 0x2a183d, 0x463157}, // Grape
        {0xf1f2f0, 0xe6e9e7, 0xfbfdfb, 0xabbab8, 0x283837, 0x4e6260, 0x326d69, 0xffffff, 0xd5e7e4}, // Pebble
        {0x10232c, 0x172c36, 0x203b49, 0x426576, 0xe0f1f3, 0xa6c8ce, 0x66cbe3, 0x173140, 0x284f5f}, // Lagoon
        {0xe6f7ef, 0xd7f0e5, 0xf4fcf7, 0x91b5a5, 0x203f35, 0x4a675b, 0x126751, 0xffffff, 0xcfeade}, // Mint
        {0x20251d, 0x292f25, 0x3a4133, 0x636d57, 0xf3f5e8, 0xc5cbb4, 0xc8d48d, 0x2b3319, 0x465438}, // Olive
        {0x131518, 0x1b1e22, 0x25292f, 0x47515b, 0xf1f2f5, 0xb6c1c9, 0xa3b9de, 0x202c42, 0x363f50}, // Onyx
        {0x181d33, 0x232842, 0x2b3452, 0x555f85, 0xeceefd, 0xb9c2de, 0xa7b5ff, 0x23284c, 0x3b4570}, // Twilight
        {0x291c25, 0x362630, 0x452f3d, 0x745367, 0xffedf5, 0xd4b7c8, 0xf3a5c5, 0x432236, 0x5c3b50}, // Berry
        {0x221e2b, 0x2b2635, 0x383143, 0x625774, 0xf2eefd, 0xc8bcdb, 0xe4b8ea, 0x3c2a46, 0x4c3d61}, // Catppuccin
        {0x222831, 0x2b333e, 0x333d4b, 0x57697b, 0xeff3fa, 0xc0cbdb, 0xa4c7f0, 0x203750, 0x374b60}, // One Dark
        {0xf3f5f9, 0xe6ebf3, 0xfbfcff, 0xa6b7cd, 0x263751, 0x465e77, 0x315e93, 0xffffff, 0xdce7f6}  // One Light
    };
    const QRgb* entry = colors[checkedTheme(theme)];
    const bool dark = qGray(entry[0]) < 128;
    return {QColor::fromRgb(entry[0]), QColor::fromRgb(entry[1]), QColor::fromRgb(entry[2]),
            QColor::fromRgb(entry[3]), QColor::fromRgb(entry[4]), QColor::fromRgb(entry[5]),
            QColor::fromRgb(entry[6]), QColor::fromRgb(entry[7]), QColor::fromRgb(entry[8]),
            QColor::fromRgb(dark ? 0xffb2a6 : 0xa32a33), dark};
}

inline QColor mix(const QColor& base, const QColor& detail, int amount) {
    return QColor((base.red() * (100 - amount) + detail.red() * amount) / 100,
                  (base.green() * (100 - amount) + detail.green() * amount) / 100,
                  (base.blue() * (100 - amount) + detail.blue() * amount) / 100);
}

inline Palette paletteFor(const QWidget* widget) {
    for (const QWidget* parent = widget; parent; parent = parent->parentWidget()) {
        const QVariant choice = parent->property("ommeshTheme");
        if (choice.isValid())
            return palette(checkedTheme(choice.toInt()));
    }
    return palette(Graphite);
}

}  // namespace Appearance

#endif
