#ifndef OMMESH_BELLE_BACKUP_JSON_HPP
#define OMMESH_BELLE_BACKUP_JSON_HPP

#include <QtCore/QRegExp>
#include <QtCore/QVariant>
#include <cmath>

namespace BackupJson {

constexpr int MaximumBytes = 2 * 1024 * 1024;

namespace Detail {

class Reader {
public:
    explicit Reader(const QString& text) : text_(text) {}

    bool read(QVariantMap& object) {
        QVariant result;
        if (!value(result, 0) || result.type() != QVariant::Map)
            return false;
        spaces();
        if (position_ != text_.size())
            return false;
        object = result.toMap();
        return true;
    }

private:
    QChar peek() const { return position_ < text_.size() ? text_.at(position_) : QChar(); }

    static bool space(QChar c) {
        return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r') || c == QLatin1Char('\n');
    }

    void spaces() {
        while (space(peek()))
            ++position_;
    }

    bool consume(char character) {
        spaces();
        if (peek() != QLatin1Char(character))
            return false;
        ++position_;
        return true;
    }

    bool codeUnit(QChar& output) {
        unsigned code = 0;
        for (int index = 0; index < 4; ++index) {
            if (position_ >= text_.size())
                return false;
            const ushort c = text_.at(position_++).unicode();
            const int digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0)
                return false;
            code = code * 16 + digit;
        }
        output = QChar(static_cast<ushort>(code));
        return true;
    }

    bool string(QString& result) {
        if (!consume('"'))
            return false;
        bool needLow = false;
        while (position_ < text_.size() && result.size() <= 65536) {
            QChar c = text_.at(position_++);
            if (c == QLatin1Char('"'))
                return !needLow;
            if (c.unicode() < 32)
                return false;
            if (c == QLatin1Char('\\')) {
                if (position_ == text_.size())
                    return false;
                c = text_.at(position_++);
                switch (c.unicode()) {
                case '"': case '/': case '\\': break;
                case 'b': c = QLatin1Char('\b'); break;
                case 'f': c = QLatin1Char('\f'); break;
                case 'n': c = QLatin1Char('\n'); break;
                case 'r': c = QLatin1Char('\r'); break;
                case 't': c = QLatin1Char('\t'); break;
                case 'u': if (!codeUnit(c)) return false; break;
                default: return false;
                }
            }
            const ushort unit = c.unicode();
            if (needLow != (unit >= 0xdc00 && unit <= 0xdfff))
                return false;
            needLow = unit >= 0xd800 && unit <= 0xdbff;
            result += c;
        }
        return false;
    }

    bool object(QVariant& result, int depth) {
        QVariantMap fields;
        if (!consume('}')) {
            do {
                QString key;
                QVariant child;
                if (fields.size() >= 64 || !string(key) || key.size() > 64 || fields.contains(key) ||
                        !consume(':') || !value(child, depth + 1))
                    return false;
                fields.insert(key, child);
            } while (consume(','));
            if (!consume('}'))
                return false;
        }
        result = fields;
        return true;
    }

    bool list(QVariant& result, int depth) {
        QVariantList entries;
        if (!consume(']')) {
            do {
                QVariant child;
                if (entries.size() >= 8192 || !value(child, depth + 1))
                    return false;
                entries.append(child);
            } while (consume(','));
            if (!consume(']'))
                return false;
        }
        result = entries;
        return true;
    }

    bool scalar(QVariant& result) {
        const int start = position_;
        while (position_ < text_.size() && position_ - start < 64 && !space(peek()) &&
                peek() != QLatin1Char(',') && peek() != QLatin1Char(']') && peek() != QLatin1Char('}'))
            ++position_;
        const QString token = text_.mid(start, position_ - start);
        if (token == QLatin1String("null"))
            return true;
        if (token == QLatin1String("true") || token == QLatin1String("false")) {
            result = token == QLatin1String("true");
            return true;
        }
        if (!number_.exactMatch(token))
            return false;
        bool valid = false;
        const double number = token.toDouble(&valid);
        if (!valid || !std::isfinite(number))
            return false;
        result = number;
        return true;
    }

    bool value(QVariant& result, int depth) {
        spaces();
        if (depth > 12 || position_ == text_.size() || ++values_ > 48000)
            return false;
        if (consume('{'))
            return object(result, depth);
        if (consume('['))
            return list(result, depth);
        if (peek() == QLatin1Char('"')) {
            QString text;
            if (!string(text))
                return false;
            result = text;
            return true;
        }
        return scalar(result);
    }

    const QString text_;
    QRegExp number_{QLatin1String("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?")};
    int position_ = 0;
    int values_ = 0;
};

inline void appendString(QByteArray& output, const QString& string) {
    const QByteArray bytes = string.toUtf8();
    constexpr char digits[] = "0123456789abcdef";
    output += '"';
    for (char c : bytes) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            output += '\\';
            output += c;
        } else if (byte < 32) {
            output += "\\u00";
            output += digits[byte >> 4];
            output += digits[byte & 15];
        } else {
            output += c;
        }
    }
    output += '"';
}

inline bool appendValue(QByteArray& output, const QVariant& value, int depth) {
    if (depth > 12 || output.size() > MaximumBytes)
        return false;
    switch (value.type()) {
    case QVariant::Invalid: output += "null"; return true;
    case QVariant::Bool: output += value.toBool() ? "true" : "false"; return true;
    case QVariant::Int: case QVariant::LongLong: output += QByteArray::number(value.toLongLong()); return true;
    case QVariant::UInt: case QVariant::ULongLong: output += QByteArray::number(value.toULongLong()); return true;
    case QVariant::Double:
        if (!std::isfinite(value.toDouble()))
            return false;
        output += QByteArray::number(value.toDouble(), 'g', 17);
        return true;
    case QVariant::String: appendString(output, value.toString()); return true;
    case QVariant::Map: {
        const QVariantMap fields = value.toMap();
        output += '{';
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
            if (it != fields.constBegin())
                output += ',';
            appendString(output, it.key());
            output += ':';
            if (!appendValue(output, it.value(), depth + 1))
                return false;
        }
        output += '}';
        return true;
    }
    case QVariant::List: case QVariant::StringList: {
        const QVariantList entries = value.toList();
        output += '[';
        for (int index = 0; index < entries.size(); ++index) {
            if (index != 0)
                output += ',';
            if (!appendValue(output, entries.at(index), depth + 1))
                return false;
        }
        output += ']';
        return true;
    }
    default: return false;
    }
}

}  // namespace Detail

inline bool readObject(const QByteArray& input, QVariantMap& object) {
    if (input.isEmpty() || input.size() > MaximumBytes)
        return false;
    const QByteArray bytes = input.startsWith("\xef\xbb\xbf") ? input.mid(3) : input;
    const QString text = QString::fromUtf8(bytes.constData(), bytes.size());
    return text.toUtf8() == bytes && Detail::Reader(text).read(object);
}

inline QByteArray writeObject(const QVariantMap& object) {
    QByteArray output;
    if (!Detail::appendValue(output, object, 0) || output.size() >= MaximumBytes)
        return QByteArray();
    output += '\n';
    return output;
}

}  // namespace BackupJson

#endif
