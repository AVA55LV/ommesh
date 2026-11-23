#ifndef OMMESH_BELLE_MESSAGE_TEXT_HPP
#define OMMESH_BELLE_MESSAGE_TEXT_HPP

#include <QtCore/QRegExp>
#include <QtCore/QSettings>
#include <QtCore/QString>

namespace MessageText {

struct Preferences {
    bool compact = false;
    bool near = false;
    bool cursive = false;
    bool restore = false;
};

inline Preferences preferences() {
    QSettings settings;
    return {settings.value(QLatin1String("ui/compactCyrillic"), true).toBool(),
            settings.value(QLatin1String("ui/compactNear"), false).toBool(),
            settings.value(QLatin1String("ui/compactCursive"), false).toBool(),
            settings.value(QLatin1String("ui/restoreCyrillic"), true).toBool()};
}

inline QString russianTwins(bool near, bool cursive) {
    QString result = QString::fromUtf8("аеорсхіАВЕКМНОРСТХІ");
    if (near)
        result += QString::fromUtf8("уУёЁЗЬ");
    if (cursive)
        result += QString::fromUtf8("тпи");
    return result;
}

inline QString latinTwins(bool near, bool cursive) {
    QString result = QLatin1String("aeopcxiABEKMHOPCTXI");
    if (near)
        result += QLatin1String("yYeE3b");
    if (cursive)
        result += QLatin1String("mnu");
    return result;
}

// Usernames, URLs and inline formulas have their own syntax and retain their original bytes.
template<class Rewrite>
QString rewriteProse(const QString& text, Rewrite rewrite) {
    QRegExp literal(QLatin1String("@\\[[^\\]\\n]{1,32}\\]|https?://\\S+|www\\.\\S+|\\$\\$[^$]*\\$\\$"),
                    Qt::CaseInsensitive);
    QString output;
    output.reserve(text.size());
    int start = 0;
    for (int match = literal.indexIn(text); match >= 0; match = literal.indexIn(text, start)) {
        output += rewrite(text.mid(start, match - start));
        const int size = literal.matchedLength();
        output += text.mid(match, size);
        start = match + size;
    }
    output += rewrite(text.mid(start));
    return output;
}

inline QString pack(const QString& text, const Preferences& prefs) {
    if (!prefs.compact)
        return text;
    const QString from = russianTwins(prefs.near, prefs.cursive);
    const QString to = latinTwins(prefs.near, prefs.cursive);
    return rewriteProse(text, [&](QString segment) {
        for (int index = 0; index < segment.size(); ++index) {
            const int twin = from.indexOf(segment.at(index));
            if (twin >= 0)
                segment[index] = to.at(twin);
        }
        return segment;
    });
}

inline QString restore(const QString& text, const Preferences& prefs) {
    if (!prefs.restore)
        return text;
    const QString from = latinTwins(true, true);
    const QString to = russianTwins(true, true);
    return rewriteProse(text, [&](QString segment) {
        int end = 0;
        while (end < segment.size()) {
            if (!segment.at(end).isLetterOrNumber()) {
                ++end;
                continue;
            }
            const int start = end;
            bool hasCyrillic = false;
            bool hasOtherLetters = false;
            while (end < segment.size() && segment.at(end).isLetterOrNumber()) {
                const QChar letter = segment.at(end++);
                if (letter.unicode() >= 0x0400 && letter.unicode() <= 0x052f)
                    hasCyrillic = true;
                else if (letter.isLetter() && !from.contains(letter))
                    hasOtherLetters = true;
            }
            if (hasCyrillic && !hasOtherLetters) {
                for (int index = start; index < end; ++index) {
                    const int twin = from.indexOf(segment.at(index));
                    if (twin >= 0)
                        segment[index] = to.at(twin);
                }
            }
        }
        return segment;
    });
}

}  // namespace MessageText

#endif
