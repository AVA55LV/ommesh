#ifndef OMMESH_BELLE_EMOJI_LINE_HPP
#define OMMESH_BELLE_EMOJI_LINE_HPP

#include "chat_features.hpp"

#include <QtCore/QVector>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPainter>

// Single-line labels in painted lists and maps use the same sprites as chat.
class EmojiLine {
public:
    EmojiLine(const QString& text, const QFontMetrics& metrics, int maximumWidth) {
        int plainStart = 0;
        const int iconSize = qMax(1, metrics.ascent());
        for (int position = 0; position < text.size();) {
            int next;
            const QImage image = ChatFeatures::emojiSpriteAt(text, position, &next);
            if (!image.isNull()) {
                appendText(text.mid(plainStart, position - plainStart), metrics);
                parts_.append({QString(), image, iconSize});
                width_ += iconSize;
                plainStart = next;
            }
            position = next;
        }
        if (parts_.isEmpty()) {
            appendText(metrics.elidedText(text, Qt::ElideRight, maximumWidth), metrics);
            return;
        }
        appendText(text.mid(plainStart), metrics);
        if (width_ > maximumWidth)
            elide(metrics, maximumWidth);
    }

    int width() const { return width_; }

    void draw(QPainter& painter, const QRect& bounds, Qt::Alignment horizontal = Qt::AlignLeft) const {
        painter.save();
        painter.setClipRect(bounds, Qt::IntersectClip);
        int x = bounds.left();
        if (horizontal == Qt::AlignHCenter)
            x += (bounds.width() - width_) / 2;
        for (const Part& part : parts_) {
            if (part.image.isNull()) {
                painter.drawText(QRect(x, bounds.y(), part.width, bounds.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, part.text);
            } else {
                const int y = bounds.y() + (bounds.height() - part.width) / 2;
                painter.drawImage(QRect(x, y, part.width, part.width), part.image);
            }
            x += part.width;
        }
        painter.restore();
    }

private:
    struct Part {
        QString text;
        QImage image;
        int width;
    };

    void appendText(const QString& text, const QFontMetrics& metrics) {
        if (text.isEmpty())
            return;
        const int width = metrics.width(text);
        parts_.append({text, QImage(), width});
        width_ += width;
    }

    void elide(const QFontMetrics& metrics, int maximumWidth) {
        const QString ellipsis(QChar(0x2026));
        const int textLimit = maximumWidth - metrics.width(ellipsis);
        QVector<Part> full;
        parts_.swap(full);
        width_ = 0;
        if (textLimit < 0)
            return;
        for (const Part& part : full) {
            if (width_ + part.width <= textLimit) {
                parts_.append(part);
                width_ += part.width;
                continue;
            }
            if (part.image.isNull()) {
                int length = part.text.size();
                while (length > 0 && width_ + metrics.width(part.text.left(length)) > textLimit) {
                    --length;
                    if (length > 0 && part.text.at(length).isLowSurrogate())
                        --length;
                }
                appendText(part.text.left(length), metrics);
            }
            break;
        }
        appendText(ellipsis, metrics);
    }

    QVector<Part> parts_;
    int width_ = 0;
};

#endif
