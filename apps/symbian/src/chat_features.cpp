#include "chat_features.hpp"
#include "link_preview.hpp"
#include "message_text.hpp"
#include "native_keyboard.hpp"
#include "node_chat_options.hpp"
#include "settings_attachments.hpp"
#include "settings_icons.hpp"
#include "window.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QTimerEvent>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/qmath.h>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QAction>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QDesktopWidget>
#include <QtGui/QDesktopServices>
#include <QtGui/QDialog>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QGridLayout>
#include <QtGui/QHBoxLayout>
#include <QtGui/QImage>
#include <QtGui/QInputContext>
#include <QtGui/QKeyEvent>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QMenu>
#include <QtGui/QMessageBox>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>
#include <QtGui/QPushButton>
#include <QtGui/QResizeEvent>
#include <QtGui/QScrollBar>
#include <QtGui/QShortcut>
#include <QtGui/QStackedWidget>
#include <QtGui/QTabBar>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextEdit>
#include <QtGui/QTextFragment>
#include <QtGui/QTextFrame>
#include <QtGui/QTextLayout>
#include <QtGui/QTextTable>
#include <QtGui/QVBoxLayout>
#include <QtCore/QSignalMapper>

#include <algorithm>

namespace {

#include "chat_emoji_data.inc"

const int emojiTotal = sizeof(emojiCodes) / sizeof(emojiCodes[0]);
const int spriteSide = 24;
const int originalEmoji = QTextFormat::UserProperty + 6;
const int messageIndex = QTextFormat::UserProperty + 7;
const int blockedRunMarker = QTextFormat::UserProperty + 31;
const int reactionBadgeCount = QTextFormat::UserProperty + 32;
const int nameLimit = 32;
const int excerptCharacters = 15;
const int excerptBytes = 30;
const int tapSlop = 9;
const int messageHoldMs = 550;
const int quickReactions[] = {9, 11, 2, 21, 7, 15};
const int moreReactions = sizeof(quickReactions) / sizeof(quickReactions[0]);
const int receiptWidth = 21;
const int receiptHeight = 14;
const int receiptAnimationMs = 260;
const int receiptTickMs = 70;
const int relayBadgeLimit = 20;
const int receiptTextSize = 14;
const int reactionTextSize = 13;
const int reactionBadgePadding = 4;
const int chatBottomMargin = 6;
const int keyboardWatchMs = 150;

enum ReceiptKind { NoReceipt, SendingReceipt, WaitingReceipt, AcceptedReceipt, ConfirmedReceipt };
enum MessageMenuAction { RemoveReaction, Reply, Resend, ReactionPeople, MessageDetails, BlockSender,
                         FirstQuickReaction };

struct Receipt {
    int position = -1;
    quint32 timestamp = 0;
    ReceiptKind kind = NoReceipt;
    qint64 startedAt = -1;
};

QString ui(const char* utf8) {
    return QString::fromUtf8(utf8);
}

int scalarEnd(const QString& text, int start) {
    return start + (text.at(start).isHighSurrogate() && start + 1 < text.size() &&
                    text.at(start + 1).isLowSurrogate() ? 2 : 1);
}

uint scalarAt(const QString& text, int start) {
    const ushort first = text.at(start).unicode();
    if (scalarEnd(text, start) > start + 1)
        return 0x10000 + ((first - 0xd800) << 10) + text.at(start + 1).unicode() - 0xdc00;
    return first;
}

int basicEmoji(uint code) {
    if (code < 0x2300)
        return -1;
    for (int i = 0; i < emojiTotal; ++i) {
        if (emojiCodes[i] == code)
            return i;
    }
    return code >= 0x1f000 && code <= 0x1faff ? emojiTotal : -1;
}

bool regional(uint code) {
    return code >= 0x1f1e6 && code <= 0x1f1ff;
}

int emojiSuffix(const QString& text, int position) {
    while (position < text.size()) {
        const uint code = scalarAt(text, position);
        if (code != 0xfe0e && code != 0xfe0f && !(code >= 0x1f3fb && code <= 0x1f3ff) &&
            !(code >= 0xe0020 && code <= 0xe007f))
            break;
        position = scalarEnd(text, position);
    }
    return position;
}

int characterEnd(const QString& text, int position, int* image = 0) {
    const uint code = scalarAt(text, position);
    int end = scalarEnd(text, position);
    int kind = basicEmoji(code);
    const bool keycap = code == '#' || code == '*' || (code >= '0' && code <= '9');
    const bool variation = end < text.size() && text.at(end).unicode() == 0xfe0f;
    if (keycap) {
        const int keyEnd = end + (variation ? 1 : 0);
        if (keyEnd < text.size() && text.at(keyEnd).unicode() == 0x20e3) {
            kind = emojiTotal;
            end = keyEnd + 1;
        }
    } else if (kind < 0 && variation && code >= 0x2300 && code <= 0x2bff) {
        kind = emojiTotal;
    }
    if (kind >= 0) {
        if (regional(code) && end < text.size() && regional(scalarAt(text, end)))
            end = scalarEnd(text, end);
        end = emojiSuffix(text, end);
        while (end + 1 < text.size() && text.at(end).unicode() == 0x200d &&
               basicEmoji(scalarAt(text, end + 1)) >= 0) {
            kind = emojiTotal;
            end = emojiSuffix(text, scalarEnd(text, end + 1));
        }
    }
    if (image)
        *image = kind;
    return end;
}

QImage emojiImage(int index) {
    QImage result(spriteSide, spriteSide, QImage::Format_ARGB32);
    if (result.isNull())
        return result;
    QRgb* pixel = reinterpret_cast<QRgb*>(result.bits());
    const int begin = emojiOffsets[index];
    const int end = emojiOffsets[index + 1];
    for (int run = begin; run < end; run += 2) {
        const int count = emojiRuns[run];
        const QRgb color = emojiPalette[emojiRuns[run + 1]];
        for (int i = 0; i < count; ++i)
            *pixel++ = color;
    }
    return result;
}

void insertEmoji(QTextCursor& cursor, int kind, const QString& original, int size,
                 const QTextCharFormat& textFormat = QTextCharFormat()) {
    const QString name = QLatin1String("ommesh-emoji:/") + QString::number(kind);
    const QUrl url(name);
    if (!cursor.document()->resource(QTextDocument::ImageResource, url).isValid()) {
        const QImage sprite = emojiImage(kind);
        if (sprite.isNull()) {
            cursor.insertText(original, textFormat);
            return;
        }
        cursor.document()->addResource(QTextDocument::ImageResource, url, sprite);
    }
    QTextImageFormat format;
    format.setName(name);
    format.setWidth(size);
    format.setHeight(size);
    if (textFormat.background().style() != Qt::NoBrush)
        format.setBackground(textFormat.background());
    format.setProperty(originalEmoji, original);
    cursor.insertImage(format);
}

void insertText(QTextCursor& cursor, const QString& text, const QTextCharFormat& format,
                int emojiSize = 18) {
    int plainStart = 0;
    for (int i = 0; i < text.size();) {
        int kind = -1;
        const int end = characterEnd(text, i, &kind);
        if (kind >= 0) {
            if (i > plainStart)
                cursor.insertText(text.mid(plainStart, i - plainStart), format);
            insertEmoji(cursor, kind, text.mid(i, end - i), emojiSize, format);
            plainStart = end;
        }
        i = end;
    }
    if (plainStart < text.size())
        cursor.insertText(text.mid(plainStart), format);
}

QTextCharFormat textColor(const QColor& color, bool strong = false, int pixels = 0) {
    QTextCharFormat format;
    format.setForeground(color);
    if (strong)
        format.setFontWeight(QFont::DemiBold);
    if (pixels > 0)
        format.setProperty(QTextFormat::FontPixelSize, pixels);
    return format;
}

QString paletteStyle(const char* style, const Appearance::Palette& palette, int textSize) {
    return QString::fromLatin1(style)
        .replace(QLatin1String("@canvas"), palette.canvas.name())
        .replace(QLatin1String("@chrome"), palette.chrome.name())
        .replace(QLatin1String("@surface"), palette.surface.name())
        .replace(QLatin1String("@border"), palette.border.name())
        .replace(QLatin1String("@text"), palette.text.name())
        .replace(QLatin1String("@muted"), palette.muted.name())
        .replace(QLatin1String("@accent"), palette.accent.name())
        .replace(QLatin1String("@onAccent"), palette.onAccent.name())
        .replace(QLatin1String("@selection"), palette.selection.name())
        .replace(QLatin1String("@danger"), palette.danger.name())
        .replace(QLatin1String("@size"), QString::number(textSize));
}

int mentionEnd(const QString& text) {
    if (!text.startsWith(QLatin1String("@[")))
        return -1;
    const int close = text.indexOf(QLatin1Char(']'), 2);
    if (close < 3 || close - 2 > nameLimit)
        return -1;
    const QString name = text.mid(2, close - 2);
    return name.contains(QLatin1Char('\n')) || name.contains(QLatin1Char('\r')) ? -1 : close;
}

QString quoteSource(const QString& text) {
    const ChatFeatures::ReplyQuote quote = ChatFeatures::splitQuote(text);
    QString content = quote.body;
    int close;
    while ((close = mentionEnd(content)) >= 0)
        content = content.mid(close + 1).trimmed();
    content = content.simplified();
    return content.isEmpty() ? quote.excerpt : content;
}

int fitPrefix(const QString& text, int bytes, int characters) {
    int end = 0;
    while (end < text.size()) {
        const int next = characterEnd(text, end);
        if (next > characters || text.left(next).toUtf8().size() > bytes)
            break;
        end = next;
    }
    return end;
}

int previewEnd(const QString& text) {
    int end = 0;
    for (int count = 0; count < excerptCharacters && end < text.size(); ++count)
        end = characterEnd(text, end);
    return end;
}

int distinguishEnd(const QString& plain, const QStringList& newerTexts) {
    int distinguish = 0;
    for (int index = 0; index < newerTexts.size(); ++index) {
        const QString newer = quoteSource(newerTexts.at(index));
        int matched = 0;
        while (matched < plain.size()) {
            const int next = characterEnd(plain, matched);
            const bool shared = next <= newer.size() &&
                                plain.mid(matched, next - matched) == newer.mid(matched, next - matched);
            matched = next;
            if (!shared)
                break;
        }
        distinguish = qMax(distinguish, matched);
    }
    return distinguish;
}

QLabel* makeLabel(const QString& text, QWidget* parent, const char* name) {
    QLabel* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName(QLatin1String(name));
    return result;
}

QPushButton* makeButton(const QString& text, QWidget* parent, const char* name) {
    QPushButton* result = new QPushButton(text, parent);
    result->setObjectName(QLatin1String(name));
    result->setMinimumHeight(40);
    return result;
}

QString routeText(const ChatMessage& message) {
    QString route = message.routeDetails;
    const int newline = route.indexOf(QLatin1Char('\n'));
    if (message.outgoing && route.startsWith(ui("Вас услышали · ")))
        return newline < 0 ? QString() : route.mid(newline + 1);
    if (message.outgoing || (!route.startsWith(ui("Переходов: ")) && !route.startsWith(ui("Хопов "))))
        return route;
    const int divider = route.indexOf(ui(" · "));
    if (divider >= 0 && (newline < 0 || divider < newline))
        return route.mid(divider + 3);
    return newline < 0 ? QString() : route.mid(newline + 1);
}

bool sameReactions(const QVector<ChatReaction>& left, const QVector<ChatReaction>& right) {
    if (left.size() != right.size())
        return false;
    for (int i = 0; i < left.size(); ++i) {
        const ChatReaction& a = left.at(i);
        const ChatReaction& b = right.at(i);
        if (a.emoji != b.emoji || a.count != b.count || a.own != b.own || a.authors != b.authors)
            return false;
    }
    return true;
}

bool ownsReaction(const ChatMessage& message, const QString& emoji = QString()) {
    QString wanted = emoji;
    wanted.remove(QChar(0xfe0f));
    for (int i = 0; i < message.reactions.size(); ++i) {
        const ChatReaction& reaction = message.reactions.at(i);
        if (!reaction.own)
            continue;
        QString actual = reaction.emoji;
        actual.remove(QChar(0xfe0f));
        if (wanted.isEmpty() || wanted == actual)
            return true;
    }
    return false;
}

bool sameMessage(const ChatMessage& a, const ChatMessage& b) {
    return a.author == b.author && a.text == b.text && a.outgoing == b.outgoing &&
           a.timestamp == b.timestamp && a.delivery == b.delivery && a.hops == b.hops &&
           a.relayCount == b.relayCount && a.relayCountPartial == b.relayCountPartial &&
           a.routeDetails == b.routeDetails && a.blockId == b.blockId && a.blocked == b.blocked &&
           a.canReact == b.canReact && sameReactions(a.reactions, b.reactions);
}

bool sameMessageIdentity(const ChatMessage& a, const ChatMessage& b) {
    return a.timestamp == b.timestamp && a.outgoing == b.outgoing && a.author == b.author &&
           a.roomSigner == b.roomSigner && a.text == b.text;
}

QTextCursor afterFrame(QTextFrame* frame) {
    QTextCursor cursor = frame->lastCursorPosition();
    cursor.movePosition(QTextCursor::NextCharacter);
    return cursor;
}

QRectF bubbleBounds(const QAbstractTextDocumentLayout* layout, QTextFrame* frame) {
    const QTextFrameFormat format = frame->frameFormat();
    return layout->frameBoundingRect(frame).adjusted(format.leftMargin(), format.topMargin(),
                                                     -format.rightMargin(), -format.bottomMargin());
}

void maskBubbleCorners(QPainter& painter, const QRectF& bubble, const QBrush& backdrop) {
    const qreal radius = qMin<qreal>(12, qMin(bubble.width(), bubble.height()) / 2);
    if (radius <= 0)
        return;
    // Keep Qt's text and frame layout. Only remove the square corners from the painted background.
    QPainterPath corners;
    corners.setFillRule(Qt::OddEvenFill);
    corners.addRect(bubble);
    corners.addRoundedRect(bubble, radius, radius);
    painter.fillPath(corners, backdrop);
}

class MessageDetailsText : public QTextEdit {
public:
    MessageDetailsText(const QColor& backdrop, QWidget* parent)
        : QTextEdit(parent), backdrop_(backdrop) {}

    void setBubble(QTextFrame* frame) {
        bubble_ = frame;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QTextEdit::paintEvent(event);
        if (!bubble_)
            return;
        const QRectF bounds = bubbleBounds(document()->documentLayout(), bubble_)
            .translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        if (!bounds.intersects(event->rect()))
            return;
        QPainter painter(viewport());
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        maskBubbleCorners(painter, bounds, QBrush(backdrop_));
    }

private:
    QColor backdrop_;
    QPointer<QTextFrame> bubble_;
};

QString blockedRunKey(const ChatMessage& first) {
    return QString::number(first.timestamp) + QLatin1Char('/') + first.blockId + QLatin1Char('/') + first.text;
}

QTextFrame* insertBlockedRun(QTextCursor& cursor, int index, const QString& label,
                            const Appearance::Palette& palette) {
    QTextFrameFormat format;
    format.setProperty(messageIndex, index);
    format.setProperty(blockedRunMarker, true);
    format.setBackground(palette.chrome);
    format.setTopMargin(3);
    format.setBottomMargin(3);
    format.setPadding(8);
    QTextFrame* frame = cursor.insertFrame(format);
    QTextCursor inside = frame->firstCursorPosition();
    insertText(inside, label, textColor(palette.muted, false, 13), 15);
    cursor = afterFrame(frame);
    return frame;
}

void insertQuote(QTextCursor& cursor, const ChatFeatures::ReplyQuote& quote,
                 const MessageText::Preferences& textPreferences, const Appearance::Palette& palette,
                 int textSize) {
    if (!quote.valid())
        return;
    QTextFrameFormat background;
    background.setPadding(6);
    background.setTopMargin(3);
    background.setBottomMargin(5);
    background.setBackground(palette.chrome);
    QTextFrame* frame = cursor.insertFrame(background);
    QTextCursor inside = frame->firstCursorPosition();
    const int quoteSize = qMax(12, textSize - 1);
    insertText(inside, quote.author.isEmpty() ? ui("Цитата") : quote.author,
               textColor(palette.accent, true, quoteSize), quoteSize + 2);
    inside.insertBlock();
    insertText(inside, MessageText::restore(quote.excerpt, textPreferences),
               textColor(palette.text, false, quoteSize), quoteSize + 2);
    cursor = afterFrame(frame);
}

void insertReactions(QTextCursor& cursor, const QVector<ChatReaction>& reactions,
                     const Appearance::Palette& palette) {
    const int padding = reactionBadgePadding;
    const int iconSize = 15;
    for (int i = 0; i < reactions.size(); ++i) {
        const ChatReaction& reaction = reactions.at(i);
        if (reaction.emoji.isEmpty() || reaction.count <= 0)
            continue;
        const QColor ink = reaction.own ? palette.accent : palette.text;
        const QColor background = reaction.own ? palette.selection : palette.chrome;
        const QString number = QString::number(reaction.count);
        int sprite = -1;
        if (characterEnd(reaction.emoji, 0, &sprite) != reaction.emoji.size())
            sprite = -1;
        const QString iconKey = sprite >= 0 ? QString::fromLatin1("sprite%1").arg(sprite)
            : QString::fromLatin1(reaction.emoji.toUtf8().toHex());
        const QString name = QString::fromLatin1("ommesh-reaction:/%1/%2/%3/%4/%5")
            .arg(iconKey, number, ink.name().mid(1), background.name().mid(1))
            .arg(int(reaction.own));
        const QUrl url(name);
        QTextDocument* document = cursor.document();
        QImage badge = qvariant_cast<QImage>(document->resource(QTextDocument::ImageResource, url));
        if (badge.isNull()) {
            QFont font = document->defaultFont();
            font.setPixelSize(reactionTextSize);
            font.setWeight(reaction.own ? QFont::DemiBold : QFont::Normal);
            const QFontMetrics metrics(font);
            const QImage icon = sprite >= 0 ? emojiImage(sprite) : QImage();
            const int iconWidth = icon.isNull() ? metrics.width(reaction.emoji) : iconSize;
            const int width = 3 * padding + iconWidth + metrics.width(number);
            const int height = qMax(iconSize + 3, metrics.height() + 2);
            badge = QImage(width + padding, height, QImage::Format_ARGB32_Premultiplied);
            if (!badge.isNull()) {
                badge.fill(0);
                QPainter painter(&badge);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(background);
                painter.drawRoundedRect(QRect(0, 0, width, height), 5, 5);
                painter.setFont(font);
                painter.setPen(ink);
                const int baseline = height - metrics.descent() - 1;
                if (icon.isNull())
                    painter.drawText(QPoint(padding, baseline), reaction.emoji);
                else
                    painter.drawImage(QRect(padding, (height - iconSize) / 2, iconSize, iconSize), icon);
                painter.drawText(QPoint(2 * padding + iconWidth, baseline), number);
                painter.end();
                document->addResource(QTextDocument::ImageResource, url, badge);
            }
        }
        if (badge.isNull()) {
            cursor.insertText(reaction.emoji + QLatin1Char(' ') + number + QLatin1Char(' '),
                              textColor(ink, reaction.own, reactionTextSize));
        } else {
            // One inline object keeps each emoji and its count together at a line break.
            QTextImageFormat chip;
            chip.setName(name);
            chip.setWidth(badge.width());
            chip.setHeight(badge.height());
            chip.setProperty(originalEmoji, reaction.emoji);
            chip.setProperty(reactionBadgeCount, reaction.count);
            cursor.insertImage(chip);
        }
    }
}

bool visibleReaction(const ChatReaction& reaction) {
    return !reaction.emoji.isEmpty() && reaction.count > 0;
}

void insertReactionAuthors(QTextCursor& cursor, const QVector<ChatReaction>& reactions,
                            const Appearance::Palette& palette, const QString& emoji, bool heading) {
    const QTextCharFormat names = textColor(palette.text);
    bool first = true;
    for (int i = 0; i < reactions.size(); ++i) {
        const ChatReaction& reaction = reactions.at(i);
        if (!visibleReaction(reaction) || (!emoji.isEmpty() && reaction.emoji != emoji))
            continue;
        if (first && heading) {
            cursor.insertBlock();
            cursor.insertText(ui("Реакции"), textColor(palette.accent, true));
        }
        if (!first || heading)
            cursor.insertBlock();
        first = false;

        QStringList people;
        if (reaction.own)
            people.append(ui("Вы"));
        for (int index = 0; index < reaction.authors.size() && people.size() < reaction.count; ++index) {
            const QString name = reaction.authors.at(index).simplified();
            if (!name.isEmpty())
                people.append(name);
        }
        if (people.size() < reaction.count)
            people.append(ui("Без имени (%1)").arg(reaction.count - people.size()));

        insertText(cursor, reaction.emoji, names, 18);
        cursor.insertText(QLatin1String("  "), names);
        insertText(cursor, people.join(QLatin1String(", ")), names);
    }
}

ReceiptKind receiptKind(const ChatMessage& message) {
    if (!message.outgoing)
        return NoReceipt;
    if (message.delivery == ui("Отправка…") || message.delivery == ui("Повторная отправка"))
        return SendingReceipt;
    if (message.delivery == ui("Ждёт подтверждения"))
        return WaitingReceipt;
    if (message.delivery == ui("Принято рацией"))
        return message.relayCount > 0 ? ConfirmedReceipt : AcceptedReceipt;
    return message.delivery == ui("Доставлено") ? ConfirmedReceipt : NoReceipt;
}

void drawReceipt(QPainter& painter, const QRect& box, const Receipt& receipt, qint64 time,
                 const Appearance::Palette& palette);

bool insertReceiptSlot(QTextCursor& cursor, Receipt& receipt, const QTextCharFormat& textFormat,
                       bool animate, const Appearance::Palette& palette) {
    const QString name = animate ? QString::fromLatin1("ommesh-status:/receipt-space")
                                 : QLatin1String("ommesh-status:/receipt-") + QString::number(receipt.kind);
    const QUrl url(name);
    if (!cursor.document()->resource(QTextDocument::ImageResource, url).isValid()) {
        QImage image(receiptWidth, receiptHeight, QImage::Format_ARGB32);
        if (image.isNull())
            return false;
        image.fill(0);
        if (!animate) {
            QPainter painter(&image);
            drawReceipt(painter, image.rect(), receipt, 0, palette);
        }
        cursor.document()->addResource(QTextDocument::ImageResource, url, image);
    }
    QTextImageFormat slot;
    slot.setName(name);
    slot.setWidth(receiptWidth);
    slot.setHeight(receiptHeight);
    receipt.position = cursor.position();
    cursor.insertImage(slot);
    cursor.setCharFormat(textFormat);
    return true;
}

void insertMessageFooter(QTextCursor& cursor, const ChatMessage& message, Receipt* receipt,
                         const Appearance::Palette& palette) {
    const ReceiptKind kind = receiptKind(message);
    QString delivery = message.outgoing ? message.delivery : QString();
    const QString time = message.timestamp == 0 ? QString() : QDateTime::fromTime_t(message.timestamp)
        .toString(QLatin1String(receipt ? "HH:mm" : "dd.MM HH:mm"));
    const bool hasReactions = std::any_of(message.reactions.constBegin(), message.reactions.constEnd(), visibleReaction);
    const bool sharedRow = hasReactions && (!time.isEmpty() || kind != NoReceipt);
    const QChar gap(0xa0);
    const QTextCharFormat timeFormat = textColor(palette.muted, false,
        message.outgoing ? receiptTextSize : sharedRow ? reactionTextSize : 12);
    const QColor statusColor = delivery == ui("Нет подтверждения") ? palette.danger : palette.muted;
    const QTextCharFormat statusFormat = textColor(statusColor, false, 12);
    const QTextCharFormat countFormat = textColor(palette.accent, true, receiptTextSize);
    const QString count = kind == ConfirmedReceipt && message.relayCount > 0
        ? gap + QString::number(qMin(message.relayCount, relayBadgeLimit)) : QString();
    QTextBlockFormat placement;
    placement.setTopMargin(3);
    placement.setAlignment(Qt::AlignRight);
    if (sharedRow) {
        // Let long delivery errors use the bubble width instead of squeezing the reactions.
        if (kind == NoReceipt && !delivery.isEmpty()) {
            cursor.insertBlock(placement);
            cursor.insertText(delivery, statusFormat);
            delivery.clear();
        }
        const QFont baseFont = cursor.document()->defaultFont();
        const QFontMetricsF timeMetrics(timeFormat.font().resolve(baseFont));
        const QFontMetricsF countMetrics(countFormat.font().resolve(baseFont));
        const qreal width = timeMetrics.width(time) + (kind == NoReceipt ? 0 : receiptWidth) +
                            (time.isEmpty() || kind == NoReceipt ? 0 : timeMetrics.width(gap)) +
                            countMetrics.width(count);
        QTextTableFormat rowFormat;
        rowFormat.setWidth(QTextLength(QTextLength::PercentageLength, 100));
        rowFormat.setColumnWidthConstraints(QVector<QTextLength>()
            << QTextLength(QTextLength::VariableLength, 0) << QTextLength(QTextLength::FixedLength, qCeil(width) + 2));
        rowFormat.setTopMargin(3);
        rowFormat.setCellPadding(0);
        rowFormat.setCellSpacing(0);
        rowFormat.setBorder(0);
        QTextTable* row = cursor.insertTable(1, 2, rowFormat);
        QTextCursor reactions = row->cellAt(0, 0).firstCursorPosition();
        QTextBlockFormat badges;
        badges.setRightMargin(6);
        reactions.setBlockFormat(badges);
        insertReactions(reactions, message.reactions, palette);
        QTextTableCell statusCell = row->cellAt(0, 1);
        QTextCharFormat bottom = statusCell.format();
        bottom.setVerticalAlignment(QTextCharFormat::AlignBottom);
        statusCell.setFormat(bottom);
        cursor = statusCell.firstCursorPosition();
        placement.setTopMargin(0);
        cursor.setBlockFormat(placement);
    } else {
        if (hasReactions) {
            cursor.insertBlock();
            insertReactions(cursor, message.reactions, palette);
        }
        if (kind == NoReceipt && delivery.isEmpty() && time.isEmpty())
            return;
        cursor.insertBlock(placement);
    }
    if (!time.isEmpty()) {
        cursor.insertText(time, timeFormat);
        if (kind != NoReceipt || !delivery.isEmpty())
            cursor.insertText(gap, timeFormat);
    }
    if (kind == NoReceipt) {
        cursor.insertText(delivery, statusFormat);
        return;
    }

    Receipt still;
    Receipt& marker = receipt ? *receipt : still;
    marker.kind = kind;
    marker.timestamp = message.timestamp;
    if (!insertReceiptSlot(cursor, marker, statusFormat, receipt != 0, palette)) {
        cursor.insertText(delivery, statusFormat);
        return;
    }
    if (!count.isEmpty())
        cursor.insertText(count, countFormat);
}

struct PreviewLinkPosition {
    int start;
    int end;
    QUrl address;
};

struct MessageLinks {
    const LinkPreview::Store& previews;
    const QSet<QString>& opened;
    const QString& chat;
    QVector<PreviewLinkPosition>& markers;
};

QString linkSelection(const ChatMessage& message, const QString& chat, const QUrl& address) {
    return chat + QLatin1Char('\n') + QString::number(message.timestamp) + QLatin1Char('\n') +
        message.author + QLatin1Char('\n') + message.text + QLatin1Char('\n') + QString::fromLatin1(address.toEncoded());
}

QTextCharFormat linkText(QTextCharFormat format, const QString& anchor) {
    format.setAnchor(true);
    format.setAnchorHref(anchor);
    return format;
}

void insertLinkCard(QTextCursor& cursor, const LinkPreview::Card& card, const QUrl& address,
                    const Appearance::Palette& palette, int textSize) {
    QTextFrameFormat frameFormat;
    frameFormat.setBackground(palette.chrome);
    frameFormat.setPadding(7);
    frameFormat.setTopMargin(4);
    QTextFrame* frame = cursor.insertFrame(frameFormat);
    QTextCursor inside = frame->firstCursorPosition();
    const QString href = QString::fromLatin1(address.toEncoded());
    const int small = qMax(12, textSize - 2);
    inside.insertText(card.host, linkText(textColor(palette.accent, false, small), href));
    if (!card.title.isEmpty()) {
        inside.insertBlock();
        inside.insertText(card.title, linkText(textColor(palette.text, true, textSize), href));
    }
    if (!card.description.isEmpty()) {
        inside.insertBlock();
        inside.insertText(card.description, linkText(textColor(palette.muted, false, small), href));
    }
    if (!card.image.isNull()) {
        const QByteArray key = QCryptographicHash::hash(address.toEncoded(), QCryptographicHash::Sha1).toHex();
        const QUrl resource(QLatin1String("ommesh-preview-image:/") + QString::fromLatin1(key));
        cursor.document()->addResource(QTextDocument::ImageResource, resource, card.image);
        QTextImageFormat image;
        image.setName(resource.toString());
        image.setWidth(card.image.width());
        image.setHeight(card.image.height());
        image.setAnchor(true);
        image.setAnchorHref(href);
        inside.insertBlock();
        inside.insertImage(image);
    }
    cursor = afterFrame(frame);
}

void insertMessageBody(QTextCursor& cursor, const ChatMessage& message, const QString& text,
                       const MessageText::Preferences& textPreferences, const Appearance::Palette& palette,
                       int textSize, const MessageLinks* display) {
    const QTextCharFormat body = textColor(message.blocked ? palette.muted : palette.text, false, textSize);
    if (!display || message.blocked) {
        insertText(cursor, MessageText::restore(text, textPreferences), body, textSize + 3);
        return;
    }
    const QList<LinkPreview::Link> links = LinkPreview::linksIn(text);
    const LinkPreview::Mode mode = display->previews.mode();
    QList<QUrl> cards;
    int from = 0;
    for (const LinkPreview::Link& item : links) {
        insertText(cursor, MessageText::restore(text.mid(from, item.start - from), textPreferences), body, textSize + 3);
        const int start = cursor.position();
        QTextCharFormat addressFormat = linkText(textColor(palette.accent, false, textSize),
                                                  QString::fromLatin1(item.address.toEncoded()));
        addressFormat.setFontUnderline(true);
        cursor.insertText(text.mid(item.start, item.length), addressFormat);
        from = item.start + item.length;
        if (mode == LinkPreview::Off || !item.previewable)
            continue;

        const QUrl address = LinkPreview::previewAddress(item.address);
        const LinkPreview::State state = display->previews.card(address).state;
        const bool opened = mode == LinkPreview::Automatic ||
            display->opened.contains(linkSelection(message, display->chat, address));
        if (opened && state == LinkPreview::Ready && !cards.contains(address))
            cards.append(address);
        if (mode == LinkPreview::Automatic)
            display->markers.append(PreviewLinkPosition{start, cursor.position(), address});
        if (state != LinkPreview::Empty && (mode == LinkPreview::OnTap || state == LinkPreview::Failed)) {
            const QString marker = state == LinkPreview::Loading ? ui("[…]") :
                opened && state == LinkPreview::Ready ? ui("[-]") : ui("[+]");
            const QString action = QLatin1String("ommesh-preview:") + QString::fromLatin1(address.toEncoded());
            cursor.insertText(QChar(0xa0) + marker, linkText(textColor(palette.accent, true, textSize), action));
        }
    }
    insertText(cursor, MessageText::restore(text.mid(from), textPreferences), body, textSize + 3);
    for (const QUrl& address : cards)
        insertLinkCard(cursor, display->previews.card(address), address, palette, textSize);
}

QTextFrame* insertMessage(QTextCursor& cursor, const ChatMessage& message, int index,
                          const MessageText::Preferences& textPreferences, const Appearance::Palette& palette,
                          int textSize, Receipt* receipt = 0, const MessageLinks* display = 0) {
    QTextFrameFormat background;
    background.setBackground(message.outgoing ? palette.selection : palette.surface);
    background.setTopMargin(3);
    background.setBottomMargin(3);
    background.setLeftMargin(message.outgoing ? 24 : 0);
    background.setRightMargin(message.outgoing ? 0 : 24);
    background.setPadding(8);
    background.setProperty(messageIndex, index);
    QTextFrame* frame = cursor.insertFrame(background);
    QTextCursor inside = frame->firstCursorPosition();
    const QTextCharFormat author = textColor(message.blocked ? palette.muted : palette.accent,
                                              true, qMax(12, textSize - 1));
    const QString sender = message.outgoing ? ui("Вы") : message.author;
    insertText(inside, sender.isEmpty() ? ui("Узел") : sender, author, 17);
    const ChatFeatures::ReplyQuote quote = ChatFeatures::splitQuote(message.text);
    if (quote.valid())
        insertQuote(inside, quote, textPreferences, palette, textSize);
    else
        inside.insertBlock();
    insertMessageBody(inside, message, quote.body, textPreferences, palette, textSize, display);
    insertMessageFooter(inside, message, receipt, palette);
    cursor = afterFrame(frame);
    return frame;
}

bool spinning(const Receipt& receipt) {
    return receipt.kind == SendingReceipt || receipt.kind == WaitingReceipt;
}

void drawCheck(QPainter& painter, qreal x, qreal progress) {
    if (progress <= 0)
        return;
    const QPointF start(x, 8.8);
    const QPointF bend(x + 3.5, 11.6);
    const QPointF end(x + 9.3, 6.3);
    const qreal split = 0.32;
    if (progress < split) {
        painter.drawLine(start, start + (bend - start) * (progress / split));
        return;
    }
    painter.drawLine(start, bend);
    painter.drawLine(bend, bend + (end - bend) * qMin<qreal>(1, (progress - split) / (1 - split)));
}

void drawReceipt(QPainter& painter, const QRect& box, const Receipt& receipt, qint64 time,
                 const Appearance::Palette& palette) {
    painter.save();
    painter.translate(box.topLeft());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(receipt.kind == ConfirmedReceipt ? palette.accent : palette.muted,
                       1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal progress = receipt.startedAt < 0 ? 1 :
                           qBound<qreal>(0, qreal(time - receipt.startedAt) / receiptAnimationMs, 1);
    switch (receipt.kind) {
        case SendingReceipt:
        case WaitingReceipt: {
            const bool waiting = receipt.kind == WaitingReceipt;
            if (waiting)
                drawCheck(painter, 0.9, progress);
            const int angle = int((time % 910) * 360 / 910) * 16;
            painter.drawArc(QRectF(waiting ? 13 : 7, waiting ? 4 : 3, waiting ? 6 : 9, waiting ? 6 : 9),
                            90 * 16 - angle, 250 * 16);
            break;
        }
        case AcceptedReceipt:
            drawCheck(painter, 6, progress);
            break;
        case ConfirmedReceipt:
            drawCheck(painter, 2, progress * 1.5);
            drawCheck(painter, 7.5, (progress - 0.22) / 0.78);
            break;
        case NoReceipt:
            break;
    }
    painter.restore();
}

} // namespace

namespace ChatFeatures {

ReplyQuote splitQuote(const QString& text) {
    ReplyQuote result;
    result.body = text;
    const int close = mentionEnd(text);
    int begin = 0;
    if (close >= 0) {
        begin = close + 1;
        while (begin < text.size() && text.at(begin).isSpace())
            ++begin;
        if (begin == close + 1)
            return result;
    }
    if (begin >= text.size() || text.at(begin) != QLatin1Char('>'))
        return result;
    const int newline = text.indexOf(QLatin1Char('\n'), begin + 1);
    if (newline < 0)
        return result;
    const QString excerpt = text.mid(begin + 1, newline - begin - 1).trimmed();
    if (excerpt.isEmpty())
        return result;
    result.author = close < 0 ? QString() : text.mid(2, close - 2);
    result.excerpt = excerpt;
    result.prefix = text.left(newline + 1);
    result.body = text.mid(newline + 1);
    return result;
}

QString makeQuotePrefix(const QString& author, const QString& message,
                        const QStringList& newerTexts, int availableBytes) {
    QString name = author.simplified();
    name.replace(QLatin1Char(']'), QChar(0xff3d));
    name = name.left(fitPrefix(name, nameLimit * 4, nameLimit));
    const QString plain = quoteSource(message);
    if (name.isEmpty() || plain.isEmpty())
        return QString();
    const QString head = QLatin1String("@[") + name + QLatin1String("] >");
    const int room = availableBytes - head.toUtf8().size() - 1;
    if (room <= 0)
        return QString();
    const int preferred = previewEnd(plain);
    const int distinguish = distinguishEnd(plain, newerTexts);
    if ((preferred == plain.size() || distinguish == plain.size()) &&
        plain.toUtf8().size() <= qMin(room, excerptBytes))
        return head + plain + QLatin1Char('\n');

    const int maximum = fitPrefix(plain, qMin(excerptBytes, room - 3), plain.size());
    int end = qMin(preferred, maximum);
    if (distinguish > preferred && distinguish <= maximum) {
        const int space = plain.indexOf(QLatin1Char(' '), distinguish);
        end = space >= 0 && space <= maximum ? space : distinguish;
    } else if (end > 0 && end < plain.size() && plain.at(end) != QLatin1Char(' ')) {
        const int space = plain.lastIndexOf(QLatin1Char(' '), end - 1);
        if (space >= qMax(8, qMin(distinguish, end)))
            end = space;
    }
    const QString punctuation = QLatin1String(" .,;:!?-");
    while (end > 0 && punctuation.contains(plain.at(end - 1)))
        --end;
    return end == 0 ? QString() : head + plain.left(end) + QLatin1String("...\n");
}

int emojiCount() {
    return emojiTotal;
}

QString emojiValue(int index) {
    if (index < 0 || index >= emojiTotal)
        return QString();
    const uint code = emojiCodes[index];
    QString result = QString::fromUcs4(&code, 1);
    for (unsigned i = 0; i < sizeof(emojiPresentation) / sizeof(emojiPresentation[0]); ++i) {
        if (emojiPresentation[i] == code) {
            result += QChar(0xfe0f);
            break;
        }
    }
    return result;
}

} // namespace ChatFeatures

class ChatHistory : public QTextEdit {
public:
    explicit ChatHistory(ChatView* owner)
        : QTextEdit(owner), owner_(owner), lastDragY_(0), down_(false), dragging_(false),
          suppressRelease_(false), focusFrame_(-1), receiptTimer_(0) {
        setObjectName(QLatin1String("messageHistory"));
        setProperty("accessibleName", ui("История сообщений"));
        setReadOnly(true);
        setUndoRedoEnabled(false);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(4);
        receiptClock_.start();
    }

    bool touching() const {
        return down_ || verticalScrollBar()->isSliderDown();
    }

    void toggleBlockedRun(const ChatMessage& message) {
        const QString key = blockedRunKey(message);
        if (!openBlockedRuns_.remove(key))
            openBlockedRuns_.insert(key);
        showMessages(owner_->messages_, false, MessageText::preferences(), false, &owner_->messages_);
    }

    void showMessages(const QVector<ChatMessage>& messages, bool followTail,
                      const MessageText::Preferences& textPreferences = MessageText::Preferences(),
                      bool animateChanges = false, const QVector<ChatMessage>* previousMessages = 0,
                      bool openAtUnread = false) {
        QScrollBar* scroll = verticalScrollBar();
        const int previous = scroll->value();
        const bool follow = followTail || previous >= scroll->maximum();
        int anchorIndex = -1;
        int anchorOffset = 0;
        if (!follow && previousMessages) {
            for (QTextFrame::Iterator it = document()->rootFrame()->begin(); !it.atEnd(); ++it) {
                QTextFrame* frame = it.currentFrame();
                if (!frame || !frame->frameFormat().hasProperty(messageIndex))
                    continue;
                const QRectF bounds = document()->documentLayout()->frameBoundingRect(frame);
                if (bounds.bottom() > previous) {
                    const int oldIndex = frame->frameFormat().intProperty(messageIndex);
                    if (oldIndex >= 0 && oldIndex < previousMessages->size()) {
                        const ChatMessage& old = previousMessages->at(oldIndex);
                        for (int i = 0; i < messages.size(); ++i) {
                            if (sameMessageIdentity(old, messages.at(i))) {
                                anchorIndex = i;
                                anchorOffset = previous - qRound(bounds.top());
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        QHash<quint32, Receipt> previousReceipts;
        quint32 latest = 0;
        if (animateChanges) {
            for (int i = 0; i < receipts_.size(); ++i) {
                const Receipt& receipt = receipts_.at(i);
                if (receipt.timestamp != 0) {
                    previousReceipts.insert(receipt.timestamp, receipt);
                    latest = qMax(latest, receipt.timestamp);
                }
            }
        }
        receipts_.clear();
        navigationFrames_.clear();
        linkMarkers_.clear();
        const MessageLinks links{*owner_->linkPreviews_, openLinkCards_, owner_->activeId_, linkMarkers_};
        clear();
        QTextCursor cursor(document());
        const qint64 now = receiptClock_.elapsed();
        QTextFrame* anchor = 0;
        QTextBlock unreadLine;
        int openRunEnd = 0;
        QSet<QString> nextOpenRuns;
        cursor.beginEditBlock();
        for (int i = 0; i < messages.size(); ++i) {
            const ChatMessage& message = messages.at(i);
            if (message.blocked && i >= openRunEnd) {
                int end = i;
                QStringList names;
                while (end < messages.size() && messages.at(end).blocked) {
                    const QString name = messages.at(end++).author;
                    if (!names.contains(name))
                        names.append(name);
                }
                const QString key = blockedRunKey(message);
                const bool opened = openBlockedRuns_.contains(key);
                const QString label = opened ? ui("Свернуть · %1").arg(names.join(ui(", "))) :
                    ui("%1 · скрыто %2").arg(names.join(ui(", "))).arg(end - i);
                QTextFrame* strip = insertBlockedRun(cursor, i, label, owner_->palette_);
                navigationFrames_.append(strip);
                if (anchorIndex == i || (!opened && anchorIndex > i && anchorIndex < end))
                    anchor = strip;
                if (!opened) {
                    i = end - 1;
                    continue;
                }
                nextOpenRuns.insert(key);
                openRunEnd = end;
            }
            if (!message.blocked && !unreadLine.isValid() && owner_->unreadMarker_ &&
                    sameMessageIdentity(*owner_->unreadMarker_, message)) {
                QTextBlockFormat marker;
                marker.setAlignment(Qt::AlignHCenter);
                marker.setTopMargin(4);
                marker.setBottomMargin(4);
                cursor.setBlockFormat(marker);
                unreadLine = cursor.block();
                cursor.insertText(ui("Непрочитанные сообщения"), ::textColor(owner_->palette_.accent, true, 12));
                cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
            }
            Receipt receipt;
            QTextFrame* frame = insertMessage(cursor, message, i, textPreferences,
                                              owner_->palette_, owner_->chatTextSize_, &receipt, &links);
            navigationFrames_.append(frame);
            if (i == anchorIndex && !anchor)
                anchor = frame;
            if (receipt.position < 0)
                continue;
            if (animateChanges && receipt.timestamp != 0) {
                const auto old = previousReceipts.constFind(receipt.timestamp);
                if (old != previousReceipts.constEnd() && old.value().kind == receipt.kind)
                    receipt.startedAt = old.value().startedAt;
                else if (receipt.kind != SendingReceipt &&
                         (old != previousReceipts.constEnd() || receipt.timestamp > latest))
                    receipt.startedAt = now;
            }
            receipts_.append(receipt);
        }
        cursor.endEditBlock();
        openBlockedRuns_.swap(nextOpenRuns);
        focusFrame_ = -1;
        const int anchored = anchor
            ? qRound(document()->documentLayout()->frameBoundingRect(anchor).top()) + anchorOffset : previous;
        if (openAtUnread && unreadLine.isValid()) {
            const qreal top = document()->documentLayout()->blockBoundingRect(unreadLine).top();
            scroll->setValue(qRound(top) - 4);
        } else if (follow) {
            setTextCursor(cursor);
            ensureCursorVisible();
            scroll->setValue(scroll->maximum());
        } else {
            scroll->setValue(anchored);
        }
        if (!owner_->searchInput_->text().trimmed().isEmpty())
            owner_->findMessage(ChatView::SearchStep::Refresh);
        viewport()->update();
    }

protected:
    void scrollContentsBy(int dx, int dy) override {
        QTextEdit::scrollContentsBy(dx, dy);
        if (owner_->backdrop_.style() == Qt::TexturePattern)
            viewport()->update();
    }

    void resizeEvent(QResizeEvent* event) {
        QScrollBar* scroll = verticalScrollBar();
        const bool follow = !touching() && scroll->value() == scroll->maximum();
        QTextEdit::resizeEvent(event);
        if (follow)
            scroll->setValue(scroll->maximum());
    }

    bool viewportEvent(QEvent* event) {
        if (down_ && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide))
            finishGesture();
        if (event->type() == QEvent::Hide && owner_->linkPreviews_)
            owner_->linkPreviews_->setVisible(QList<QUrl>());
        return QTextEdit::viewportEvent(event);
    }

    void paintEvent(QPaintEvent* event) {
        {
            QPainter backdrop(viewport());
            backdrop.setClipRegion(event->region());
            backdrop.fillRect(event->rect(), owner_->backdrop_);
        }
        QTextEdit::paintEvent(event);
        updateVisibleLinks();
        const qint64 now = receiptClock_.elapsed();
        const QRect visible = viewport()->rect();
        bool animated = false;
        if (!navigationFrames_.isEmpty() || !receipts_.isEmpty()) {
            QPainter painter(viewport());
            painter.setClipRegion(event->region());
            paintBubbleCorners(painter, event->rect());
            for (int i = firstVisibleReceipt(); i < receipts_.size(); ++i) {
                const Receipt& receipt = receipts_.at(i);
                const QRect box = receiptRect(receipt);
                if (box.top() > visible.bottom())
                    break;
                if (!box.intersects(visible))
                    continue;
                if (event->rect().intersects(box))
                    drawReceipt(painter, box, receipt, now, owner_->palette_);
                animated = animated || spinning(receipt) ||
                    (receipt.startedAt >= 0 && now - receipt.startedAt < receiptAnimationMs);
            }
        }
        if (animated && receiptTimer_ == 0)
            receiptTimer_ = startTimer(receiptTickMs);
        else if (!animated)
            stopReceiptTimer();
    }

    void timerEvent(QTimerEvent* event) {
        if (event->timerId() == holdTimer_) {
            stopHoldTimer();
            if (down_ && !dragging_) {
                if (QTextFrame* frame = frameAt(downAt_)) {
                    focusFrame_ = navigationFrames_.indexOf(frame);
                    suppressRelease_ = true;
                    queueFrameAction(frame);
                    finishGesture();
                }
            }
            return;
        }
        if (event->timerId() != receiptTimer_) {
            QTextEdit::timerEvent(event);
            return;
        }
        if (!isVisible()) {
            stopReceiptTimer();
            return;
        }
        const QRect visible = viewport()->rect();
        const qint64 now = receiptClock_.elapsed();
        bool dirty = false;
        for (int i = firstVisibleReceipt(); i < receipts_.size(); ++i) {
            Receipt& receipt = receipts_[i];
            const QRect box = receiptRect(receipt);
            if (box.top() > visible.bottom())
                break;
            if ((!spinning(receipt) && receipt.startedAt < 0) || !box.intersects(visible))
                continue;
            if (receipt.startedAt >= 0 && now - receipt.startedAt >= receiptAnimationMs)
                receipt.startedAt = -1;
            viewport()->update(box);
            dirty = true;
        }
        if (!dirty)
            stopReceiptTimer();
    }

    void mousePressEvent(QMouseEvent* event) {
        if (event->button() != Qt::LeftButton) {
            QTextEdit::mousePressEvent(event);
            return;
        }
        down_ = true;
        dragging_ = false;
        suppressRelease_ = false;
        downAt_ = event->pos();
        lastDragY_ = downAt_.y();
        stopHoldTimer();
        if (frameAt(downAt_))
            holdTimer_ = startTimer(messageHoldMs);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) {
        if (!down_) {
            QTextEdit::mouseMoveEvent(event);
            return;
        }
        moveGesture(event->pos());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) {
        if (suppressRelease_ && event->button() == Qt::LeftButton) {
            suppressRelease_ = false;
            event->accept();
            return;
        }
        if (!down_ || event->button() != Qt::LeftButton) {
            QTextEdit::mouseReleaseEvent(event);
            return;
        }
        moveGesture(event->pos());
        finishGesture();
        if (!dragging_ && (event->pos() - downAt_).manhattanLength() <= tapSlop) {
            QTextFrame* frame = frameAt(event->pos());
            if (frame && frame == frameAt(downAt_)) {
                focusFrame_ = navigationFrames_.indexOf(frame);
                const QString emoji = reactionAt(event->pos());
                if (!emoji.isEmpty() && emoji == reactionAt(downAt_)) {
                    owner_->queueMessageActions(frame->frameFormat().intProperty(messageIndex),
                                                false, emoji);
                } else if (!handleLinkTap(frame, event->pos())) {
                    queueFrameAction(frame);
                }
            }
        }
        event->accept();
    }

    void contextMenuEvent(QContextMenuEvent* event) {
        QTextFrame* frame = event->reason() == QContextMenuEvent::Keyboard
            ? navigationFrames_.value(focusFrame_) : frameAt(event->pos());
        if (frame && !suppressRelease_ && (!down_ || (!dragging_ && frame == frameAt(downAt_)))) {
            focusFrame_ = navigationFrames_.indexOf(frame);
            suppressRelease_ = down_;
            finishGesture();
            queueFrameAction(frame);
        }
        event->accept();
    }

    void hideEvent(QHideEvent* event) override {
        finishGesture();
        suppressRelease_ = false;
        QTextEdit::hideEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) {
        const int key = event->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            const int size = navigationFrames_.size();
            if (size > 0) {
                int next = focusFrame_ + (key == Qt::Key_Up ? -1 : 1);
                if (focusFrame_ < 0)
                    next = key == Qt::Key_Up ? size - 1 : 0;
                focusFrame_ = qBound(0, next, size - 1);
                focusMessage(navigationFrames_.at(focusFrame_));
            }
            event->accept();
        } else if (key == Qt::Key_Enter || key == Qt::Key_Return || key == Qt::Key_Select) {
            if (QTextFrame* frame = navigationFrames_.value(focusFrame_))
                queueFrameAction(frame);
            event->accept();
        } else {
            QTextEdit::keyPressEvent(event);
        }
    }

private:
    QString reactionAt(const QPoint& point) const {
        const int position = cursorForPosition(point).position();
        QTextCursor item(document());
        // A tap on an inline image can resolve to either side of its character.
        for (int at = position; at >= position - 1; --at) {
            if (at < 0 || at >= document()->characterCount() - 1 ||
                document()->characterAt(at) != QChar::ObjectReplacementCharacter)
                continue;
            item.setPosition(at + 1);
            const QTextCharFormat format = item.charFormat();
            if (!format.isImageFormat() || !format.hasProperty(reactionBadgeCount))
                continue;
            const QTextImageFormat image = format.toImageFormat();
            item.setPosition(at);
            const QRect bounds = inlineRect(item, qRound(image.width()) - reactionBadgePadding,
                                            qRound(image.height()));
            if (bounds.contains(point))
                return format.stringProperty(originalEmoji);
        }
        return QString();
    }

    void paintBubbleCorners(QPainter& painter, const QRect& dirty) const {
        const QPoint offset(horizontalScrollBar()->value(), verticalScrollBar()->value());
        const QRectF documentDirty(dirty.translated(offset));
        const QAbstractTextDocumentLayout* layout = document()->documentLayout();
        const auto first = std::lower_bound(navigationFrames_.constBegin(), navigationFrames_.constEnd(),
            documentDirty.top(), [layout](QTextFrame* frame, qreal y) {
                return bubbleBounds(layout, frame).bottom() < y;
            });
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        for (auto it = first; it != navigationFrames_.constEnd(); ++it) {
            QTextFrame* frame = *it;
            const QRectF bounds = bubbleBounds(layout, frame);
            if (bounds.top() > documentDirty.bottom())
                break;
            if (!frame->frameFormat().boolProperty(blockedRunMarker) && bounds.intersects(documentDirty))
                maskBubbleCorners(painter, bounds.translated(-offset), owner_->backdrop_);
        }
        painter.restore();
    }

    bool handleLinkTap(QTextFrame* frame, const QPoint& point) {
        const QString anchor = anchorAt(point);
        if (anchor.isEmpty() || anchor != anchorAt(downAt_))
            return false;
        const QString prefix = QLatin1String("ommesh-preview:");
        const bool preview = anchor.startsWith(prefix);
        const QUrl address = QUrl::fromEncoded((preview ? anchor.mid(prefix.size()) : anchor).toLatin1(), QUrl::StrictMode);
        if (!address.isValid() || (address.scheme() != QLatin1String("https") && address.scheme() != QLatin1String("http")))
            return false;
        if (!preview) {
            QDesktopServices::openUrl(address);
            return true;
        }
        const int index = frame->frameFormat().intProperty(messageIndex);
        if (index < 0 || index >= owner_->messages_.size() || owner_->messages_.at(index).blocked)
            return true;
        const QString key = linkSelection(owner_->messages_.at(index), owner_->activeId_, address);
        const LinkPreview::Card card = owner_->linkPreviews_->card(address);
        if (card.state == LinkPreview::Ready && openLinkCards_.remove(key)) {
            owner_->refreshLinkPreviews();
            return true;
        }
        if (openLinkCards_.size() >= 128)
            openLinkCards_.clear();
        openLinkCards_.insert(key);
        if (card.state == LinkPreview::Ready)
            owner_->refreshLinkPreviews();
        else
            owner_->linkPreviews_->request(address);
        return true;
    }

    void updateVisibleLinks() {
        if (!owner_->linkPreviews_ || owner_->linkPreviews_->mode() != LinkPreview::Automatic)
            return;
        QList<QUrl> visible;
        if (!linkMarkers_.isEmpty()) {
            const int first = cursorForPosition(QPoint(0, 0)).position();
            auto it = std::lower_bound(linkMarkers_.constBegin(), linkMarkers_.constEnd(), first,
                [](const PreviewLinkPosition& marker, int position) { return marker.end < position; });
            QTextCursor cursor(document());
            while (it != linkMarkers_.constEnd() && visible.size() < 12) {
                cursor.setPosition(it->start);
                const QRect start = cursorRect(cursor);
                if (start.top() >= viewport()->height())
                    break;
                cursor.setPosition(it->end);
                if (cursorRect(cursor).bottom() >= 0 && !visible.contains(it->address))
                    visible.append(it->address);
                ++it;
            }
        }
        owner_->linkPreviews_->setVisible(visible);
    }

    void moveGesture(const QPoint& at) {
        if (!dragging_ && (at - downAt_).manhattanLength() > tapSlop) {
            dragging_ = true;
            stopHoldTimer();
            owner_->pendingSentText_.clear();
        }
        if (dragging_) {
            QScrollBar* scroll = verticalScrollBar();
            scroll->setValue(scroll->value() + lastDragY_ - at.y());
            lastDragY_ = at.y();
        }
    }

    void finishGesture() {
        stopHoldTimer();
        if (down_) {
            down_ = false;
            QTimer::singleShot(0, owner_, SLOT(applyDeferredMessages()));
        }
    }

    void stopHoldTimer() {
        if (holdTimer_ != 0) {
            killTimer(holdTimer_);
            holdTimer_ = 0;
        }
    }

    int firstVisibleReceipt() const {
        const int first = cursorForPosition(QPoint(0, 0)).position();
        const auto at = std::lower_bound(receipts_.constBegin(), receipts_.constEnd(), first,
            [](const Receipt& receipt, int position) { return receipt.position < position; });
        return qMax(0, int(at - receipts_.constBegin()) - 1);
    }

    QRect receiptRect(const Receipt& receipt) const {
        QTextCursor cursor(document());
        cursor.setPosition(receipt.position);
        return inlineRect(cursor, receiptWidth, receiptHeight);
    }

    QRect inlineRect(const QTextCursor& cursor, int width, int height) const {
        const QRect caret = cursorRect(cursor);
        const QTextLine line = cursor.block().layout()->lineForTextPosition(cursor.positionInBlock());
        const int baseline = line.isValid() ? caret.top() + qRound(line.ascent()) : caret.bottom();
        return QRect(caret.x(), baseline - height + 1, width, height);
    }

    void stopReceiptTimer() {
        if (receiptTimer_ != 0) {
            killTimer(receiptTimer_);
            receiptTimer_ = 0;
        }
    }

    QTextFrame* frameAt(const QPoint& position) const {
        QTextFrame* frame = cursorForPosition(position).currentFrame();
        while (frame != 0 && !frame->frameFormat().hasProperty(messageIndex))
            frame = frame->parentFrame();
        if (!frame)
            return 0;
        const QPointF at(position.x() + horizontalScrollBar()->value(),
                         position.y() + verticalScrollBar()->value());
        return bubbleBounds(document()->documentLayout(), frame).contains(at) ? frame : 0;
    }

    void queueFrameAction(QTextFrame* frame) {
        const QTextFrameFormat format = frame->frameFormat();
        owner_->queueMessageActions(format.intProperty(messageIndex), format.boolProperty(blockedRunMarker));
    }

    void focusMessage(QTextFrame* frame) {
        QTextCursor cursor = frame->firstCursorPosition();
        setTextCursor(cursor);
        ensureCursorVisible();
        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;
        selection.cursor.select(QTextCursor::LineUnderCursor);
        selection.format.setBackground(owner_->palette_.selection);
        QList<QTextEdit::ExtraSelection> highlight;
        highlight.append(selection);
        setExtraSelections(highlight);
    }

    ChatView* owner_;
    QPoint downAt_;
    int lastDragY_;
    bool down_;
    bool dragging_;
    bool suppressRelease_;
    int focusFrame_;
    QVector<QTextFrame*> navigationFrames_;
    QSet<QString> openBlockedRuns_;
    QVector<Receipt> receipts_;
    QVector<PreviewLinkPosition> linkMarkers_;
    QSet<QString> openLinkCards_;
    QElapsedTimer receiptClock_;
    int receiptTimer_;
    int holdTimer_ = 0;
};

ChatView::ChatView(QWidget* parent)
    : QWidget(parent), palette_(Appearance::palette(Appearance::Graphite)),
      chatTextSize_(Appearance::DefaultChatTextSize), maxBytes_(0),
      canSend_(false), editingDraft_(false),
      restoreDisplay_(false), hasMessageSnapshot_(false) {
    setObjectName(QLatin1String("nativeChat"));
    setAttribute(Qt::WA_StyledBackground);

    QVBoxLayout* page = new QVBoxLayout(this);
    page->setContentsMargins(8, 5, 8, chatBottomMargin);
    page->setSpacing(5);
    QHBoxLayout* top = new QHBoxLayout;
    top->setSpacing(7);
    QPushButton* back = makeButton(ui("Чаты"), this, "chatBack");
    optionsButton_ = makeButton(QString(), this, "chatOptionsButton");
    optionsButton_->setProperty("accessibleName", ui("Действия чата"));
    optionsButton_->setFixedWidth(42);
    optionsButton_->setIconSize(QSize(24, 24));
    QMenu* chatMenu = new QMenu(optionsButton_);
    chatMenu->setObjectName(QLatin1String("chatOptions"));
    pinAction_ = chatMenu->addAction(ui("Закрепить чат"));
    optionsButton_->setMenu(chatMenu);
    searchButton_ = makeButton(QString(), this, "chatMessageSearchButton");
    searchButton_->setProperty("accessibleName", ui("Поиск по сообщениям"));
    searchButton_->setCheckable(true);
    searchButton_->setFixedWidth(44);
    searchButton_->setIconSize(QSize(24, 24));
    QVBoxLayout* headings = new QVBoxLayout;
    headings->setSpacing(1);
    title_ = makeLabel(QString(), this, "chatTitle");
    title_->setWordWrap(true);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_ = makeLabel(QString(), this, "chatStatus");
    status_->setWordWrap(true);
    headings->addWidget(title_);
    headings->addWidget(status_);
    top->addWidget(back);
    top->addLayout(headings, 1);
    top->addWidget(optionsButton_);
    top->addWidget(searchButton_);
    page->addLayout(top);

    searchRow_ = new QWidget(this);
    QHBoxLayout* search = new QHBoxLayout(searchRow_);
    search->setContentsMargins(0, 0, 0, 0);
    search->setSpacing(5);
    searchInput_ = new QLineEdit(searchRow_);
    searchInput_->setObjectName(QLatin1String("chatMessageSearch"));
    searchInput_->setProperty("accessibleName", ui("Найти сообщение"));
    searchInput_->setPlaceholderText(ui("Найти сообщение"));
    searchInput_->setInputMethodHints(Qt::ImhNoAutoUppercase);
    searchNext_ = makeButton(ui("Далее"), searchRow_, "chatMessageSearchNext");
    searchNext_->setEnabled(false);
    searchStatus_ = makeLabel(ui("Не найдено"), searchRow_, "chatMessageSearchStatus");
    searchStatus_->hide();
    search->addWidget(searchInput_, 1);
    search->addWidget(searchStatus_);
    search->addWidget(searchNext_);
    page->addWidget(searchRow_);
    searchRow_->hide();
    QShortcut* closeSearch = new QShortcut(QKeySequence(Qt::Key_Escape), searchRow_);
    closeSearch->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeSearch, SIGNAL(activated()), searchButton_, SLOT(toggle()));

    history_ = new ChatHistory(this);
    linkPreviews_ = new LinkPreview::Store(this);
    linkPreviews_->setMode(LinkPreview::preference());
    connect(linkPreviews_, SIGNAL(changed()), this, SLOT(refreshLinkPreviews()), Qt::QueuedConnection);
    connect(history_->verticalScrollBar(), SIGNAL(sliderReleased()),
            this, SLOT(applyDeferredMessages()), Qt::QueuedConnection);
    page->addWidget(history_, 1);
    quotePreview_ = new QWidget(this);
    QHBoxLayout* preview = new QHBoxLayout(quotePreview_);
    preview->setContentsMargins(0, 0, 0, 0);
    preview->setSpacing(4);
    quoteText_ = new QTextEdit(quotePreview_);
    quoteText_->setObjectName(QLatin1String("quotePreview"));
    quoteText_->setProperty("accessibleName", ui("Цитата в черновике"));
    quoteText_->setReadOnly(true);
    quoteText_->setTextInteractionFlags(Qt::NoTextInteraction);
    quoteText_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    quoteText_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    quoteText_->setFixedHeight(50);
    QPushButton* clearQuote = makeButton(ui("×"), quotePreview_, "clearQuote");
    clearQuote->setProperty("accessibleName", ui("Убрать цитату"));
    clearQuote->setFixedWidth(40);
    clearQuote->setFocusPolicy(Qt::TabFocus);
    preview->addWidget(quoteText_, 1);
    preview->addWidget(clearQuote);
    page->addWidget(quotePreview_);

    byteLimit_ = makeLabel(QString(), this, "chatByteLimit");
    byteLimit_->setAlignment(Qt::AlignRight);
    page->addWidget(byteLimit_);

    QHBoxLayout* input = new QHBoxLayout;
    input->setSpacing(5);
    composer_ = new QTextEdit(this);
    composer_->setObjectName(QLatin1String("messageComposer"));
    composer_->setProperty("accessibleName", ui("Текст сообщения"));
    composer_->setAcceptRichText(false);
    composer_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    composer_->setFixedHeight(59);
    keyboardWatch_ = new QTimer(this);
    composer_->installEventFilter(this);
    emojiButton_ = makeButton(QString(), this, "emojiButton");
    emojiButton_->setProperty("accessibleName", ui("Смайлики"));
    emojiButton_->setToolTip(ui("Смайлики"));
    emojiButton_->setFixedWidth(44);
    emojiButton_->setIcon(QIcon(QPixmap::fromImage(emojiImage(0))));
    emojiButton_->setIconSize(QSize(24, 24));
    attachmentButton_ = makeButton(QString(), this, "attachFile");
    attachmentButton_->setProperty("accessibleName", ui("Прикрепить файл"));
    attachmentButton_->setToolTip(ui("Прикрепить файл"));
    attachmentButton_->setFixedWidth(42);
    attachmentButton_->setIconSize(QSize(22, 22));
    attachmentButton_->hide();
    sendButton_ = makeButton(ui("Отправить"), this, "sendMessage");
    for (auto* button : {emojiButton_, attachmentButton_, sendButton_})
        button->setFocusPolicy(Qt::TabFocus);
    input->addWidget(composer_, 1);
    input->addWidget(emojiButton_);
    input->addWidget(attachmentButton_);
    input->addWidget(sendButton_);
    page->addLayout(input);

    connect(back, SIGNAL(clicked()), this, SIGNAL(backRequested()));
    connect(pinAction_, SIGNAL(triggered(bool)), this, SIGNAL(pinRequested()));
    connect(searchButton_, SIGNAL(toggled(bool)), this, SLOT(toggleMessageSearch(bool)));
    connect(searchInput_, SIGNAL(textChanged(QString)), this, SLOT(updateMessageSearch()));
    connect(searchInput_, SIGNAL(returnPressed()), this, SLOT(nextMessageSearchMatch()));
    connect(searchNext_, SIGNAL(clicked()), this, SLOT(nextMessageSearchMatch()));
    connect(clearQuote, SIGNAL(clicked()), this, SLOT(removeQuote()));
    connect(emojiButton_, SIGNAL(clicked()), this, SLOT(chooseEmoji()));
    connect(attachmentButton_, SIGNAL(clicked()), this, SLOT(chooseAttachment()), Qt::QueuedConnection);
    connect(composer_, SIGNAL(textChanged()), this, SLOT(updateComposer()));
    connect(qApp, SIGNAL(focusChanged(QWidget*,QWidget*)), this, SLOT(composerFocusChanged(QWidget*,QWidget*)));
    connect(keyboardWatch_, SIGNAL(timeout()), this, SLOT(updateKeyboardInset()));
    connect(sendButton_, SIGNAL(clicked()), this, SLOT(send()));
    const Appearance::Preferences appearance = Appearance::preferences();
    setAppearance(Appearance::palette(appearance.theme), appearance.chatTextSize);
    updateComposer();
}

void ChatView::composerFocusChanged(QWidget* previous, QWidget* focus) {
    if (previous != composer_ && focus != composer_ && !keyboardWatch_->isActive())
        return;
    // The native editor can momentarily take Qt focus when opening the FEP.
    if (!focus && keyboardWatch_->isActive())
        return;
    const bool split = focus == composer_ && !NativeKeyboard::fullScreenPreferred();
    if (NativeKeyboard::setSplitMode(focus, split)) {
        if (!keyboardWatch_->isActive()) {
            NativeKeyboard::takePanelEvent();
            keyboardTouchActive_ = NativeKeyboard::touchInputActive();
        }
        keyboardWatch_->start(keyboardWatchMs);
    } else {
        keyboardWatch_->stop();
        keyboardPanelOpen_ = false;
    }
    updateKeyboardInset();
}

bool ChatView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == composer_ && keyboardWatch_->isActive()) {
        const bool opening = event->type() == QEvent::RequestSoftwareInputPanel;
        if (opening || event->type() == QEvent::CloseSoftwareInputPanel) {
            NativeKeyboard::takePanelEvent();
            keyboardTouchActive_ = NativeKeyboard::touchInputActive();
            keyboardPanelOpen_ = opening;
            updateKeyboardInset();
        } else if (event->type() == QEvent::Hide) {
            keyboardWatch_->stop();
            keyboardPanelOpen_ = false;
            NativeKeyboard::setSplitMode(nullptr, false);
            updateKeyboardInset();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChatView::updateKeyboardInset() {
    QLayout* page = layout();
    const QMargins margins = page->contentsMargins();
    int bottom = chatBottomMargin;
    if (keyboardWatch_->isActive() && isVisible()) {
        const auto panelEvent = NativeKeyboard::takePanelEvent();
        const int touchActive = NativeKeyboard::touchInputActive();
        if (panelEvent != NativeKeyboard::PanelEvent::None)
            keyboardPanelOpen_ = panelEvent == NativeKeyboard::PanelEvent::Open;
        else if (touchActive >= 0 && touchActive != keyboardTouchActive_ &&
                 (touchActive == 1 || keyboardTouchActive_ == 1))
            keyboardPanelOpen_ = touchActive != 0;
        if (touchActive >= 0)
            keyboardTouchActive_ = touchActive;
        if (keyboardPanelOpen_) {
            const int visibleBottom = NativeKeyboard::splitViewBottom(QApplication::desktop()->screenGeometry(window()));
            if (visibleBottom >= 0) {
                const int overlap = mapToGlobal(QPoint(0, height())).y() - visibleBottom;
                const int maxInset = height() - page->minimumSize().height() + margins.bottom() - chatBottomMargin;
                bottom += qBound(0, overlap, qMax(0, maxInset));
            }
        }
    }
    if (margins.bottom() != bottom)
        page->setContentsMargins(margins.left(), margins.top(), margins.right(), bottom);
}

void ChatView::syncLinkPreviewMode() {
    linkPreviews_->setMode(LinkPreview::preference());
}

void ChatView::syncChatBackground() {
    backdrop_ = NodeChatOptions::backgroundBrush(NodeChatOptions::background(activeId_), palette_.canvas, palette_.accent);
    history_->viewport()->update();
}

void ChatView::refreshLinkPreviews() {
    if (history_->touching()) {
        previewRefreshPending_ = true;
        return;
    }
    previewRefreshPending_ = false;
    if (isVisible() && !messages_.isEmpty())
        history_->showMessages(messages_, false, MessageText::preferences(), false, &messages_);
}

void ChatView::setAppearance(const Appearance::Palette& palette, int chatTextSize) {
    attachmentButton_->setIcon(SettingsIcons::icon(SettingsIcons::Attachments, palette.text));
    optionsButton_->setIcon(SettingsIcons::icon(SettingsIcons::More, palette.text, 24));
    searchButton_->setIcon(SettingsIcons::icon(SettingsIcons::Search, palette.text, 24));
    palette_ = palette;
    chatTextSize_ = Appearance::checkedChatTextSize(chatTextSize);
    const QString style = paletteStyle(
        "#nativeChat { background: @canvas; }"
        "#nativeChat QLabel { color: @text; }"
        "#nativeChat QLabel#chatTitle { font-weight: bold; font-size: 18px; }"
        "#nativeChat QLabel#chatStatus, #nativeChat QLabel#chatByteLimit, "
        "#nativeChat QLabel#chatMessageSearchStatus { color: @muted; font-size: 12px; }"
        "#nativeChat QLineEdit#chatMessageSearch { color: @text; background: @chrome; "
        " min-height: 34px; border: 1px solid @border; border-radius: 6px; padding: 3px 7px; }"
        "#nativeChat QTextEdit { color: @text; font-size: @sizepx; "
        " border: 1px solid @border; border-radius: 6px; selection-background-color: @selection; }"
        "#nativeChat QTextEdit#messageComposer { background: @chrome; }"
        "#nativeChat QTextEdit#messageHistory { border: none; background: transparent; }"
        "#nativeChat QTextEdit#quotePreview { background: @chrome; border-left: 3px solid @accent; font-size: 14px; }"
        "#nativeChat QScrollBar:vertical { background: @chrome; width: 7px; margin: 0; }"
        "#nativeChat QScrollBar::handle:vertical { background: @border; min-height: 18px; border-radius: 3px; }"
        "#nativeChat QScrollBar::add-line:vertical, #nativeChat QScrollBar::sub-line:vertical { height: 0; }"
        "#nativeChat QPushButton { color: @text; background: @surface; border: 1px solid @border;"
        " border-radius: 7px; padding: 4px 7px; }"
        "#nativeChat QPushButton#sendMessage { color: @onAccent; background: @accent; font-weight: bold; }"
        "#nativeChat QPushButton#chatOptionsButton::menu-indicator { width: 0px; }"
        "#nativeChat QPushButton#chatMessageSearchButton:checked { background: @selection; }"
        "#nativeChat QPushButton:pressed { background: @selection; }"
        "#nativeChat QPushButton:disabled, #nativeChat QPushButton#sendMessage:disabled { "
        " color: @muted; background: @surface; }"
        "QMenu#chatOptions { color: @text; background: @surface;"
        " border: 1px solid @border; }"
        "QMenu#chatOptions::item { min-height: 32px; padding: 4px 10px; }"
        "QMenu#chatOptions::item:selected { background: @selection; }"
        "#messageActions QPushButton[messageActionRow=\"true\"] { text-align: left; padding: 4px 10px; }"
        "#messageActions QPushButton[messageActionRow=\"true\"]:focus { border: 1px solid @accent; }"
        "#messageActions QPushButton#messageActionBlock[danger=\"true\"] { color: @danger; }"
        "#reactionStrip { background: @chrome; }"
        "#reactionStrip QPushButton { color: @text; background: @surface; border: 1px solid @border;"
        " border-radius: 7px; padding: 0px; }"
        "#reactionStrip QPushButton[own=\"true\"], #reactionStrip QPushButton:focus { border: 1px solid @accent; }"
        "#reactionStrip QPushButton:pressed { background: @selection; }", palette, chatTextSize_);
    if (style == styleSheet())
        return;
    setStyleSheet(style);
    syncChatBackground();
    quoteText_->setFixedHeight(qMax(50, chatTextSize_ * 2 + 10));
    showQuotePreview();
    if (!messages_.isEmpty())
        history_->showMessages(messages_, false, MessageText::preferences(), false, &messages_);
}

void ChatView::setContext(const QString& title, const QString& status, const QString& activeId) {
    title_->setText(title);
    status_->setText(status);
    status_->setVisible(!status.isEmpty());
    if (activeId_ == activeId)
        return;
    dismissMessageActions(false);
    searchButton_->setChecked(false);
    activeId_ = activeId;
    attachmentButton_->setVisible(!activeId_.isEmpty() && Attachments::ready());
    syncChatBackground();
    actionTarget_.clear();
    hasMessageSnapshot_ = false;
    hasDeferredMessages_ = false;
    deferredMessages_.clear();
    unreadMarker_.reset();
    pendingSentText_.clear();
    messages_.clear();
    history_->showMessages(messages_, true);
    clearDraft();
}

void ChatView::setMessages(const QVector<ChatMessage>& messages, bool opening) {
    if (!opening && history_->touching()) {
        deferredMessages_ = messages;
        hasDeferredMessages_ = true;
        return;
    }
    hasDeferredMessages_ = false;
    deferredMessages_.clear();
    if (opening) {
        dismissMessageActions(false);
        searchButton_->setChecked(false);
        attachmentButton_->setVisible(!activeId_.isEmpty() && Attachments::ready());
        syncChatBackground();
        actionTarget_.clear();
        pendingSentText_.clear();
        unreadMarker_.reset();
        const auto firstUnread = std::find_if(messages.constBegin(), messages.constEnd(),
            [](const ChatMessage& message) { return message.unread; });
        if (firstUnread != messages.constEnd())
            unreadMarker_ = *firstUnread;
    }
    const MessageText::Preferences textPreferences = MessageText::preferences();
    const bool animateChanges = !opening && hasMessageSnapshot_;
    hasMessageSnapshot_ = true;
    bool unchanged = !opening && messages.size() == messages_.size() && restoreDisplay_ == textPreferences.restore;
    for (int i = 0; unchanged && i < messages.size(); ++i)
        unchanged = sameMessage(messages.at(i), messages_.at(i));
    if (unchanged)
        return;
    bool follow = opening || messages_.isEmpty();
    if (!pendingSentText_.isEmpty()) {
        for (int i = messages.size() - 1; i >= 0; --i) {
            const ChatMessage& message = messages.at(i);
            if (message.outgoing && message.timestamp > pendingSentAfter_ && message.text == pendingSentText_) {
                follow = true;
                pendingSentText_.clear();
                break;
            }
        }
    }
    restoreDisplay_ = textPreferences.restore;
    const bool openAtUnread = opening && unreadMarker_ &&
        QSettings().value(QLatin1String("ui/openAtUnread"), true).toBool();
    history_->showMessages(messages, follow, textPreferences, animateChanges, &messages_, openAtUnread);
    messages_ = messages;
}

void ChatView::applyDeferredMessages() {
    if (history_->touching())
        return;
    if (hasDeferredMessages_) {
        const QVector<ChatMessage> pending = deferredMessages_;
        setMessages(pending);
    }
    if (previewRefreshPending_)
        refreshLinkPreviews();
}

void ChatView::followSentMessage(const QString& text) {
    pendingSentText_ = text;
    pendingSentAfter_ = 0;
    for (int i = 0; i < messages_.size(); ++i) {
        if (messages_.at(i).outgoing)
            pendingSentAfter_ = qMax(pendingSentAfter_, messages_.at(i).timestamp);
    }
    if (history_->verticalScrollBar()->isSliderDown())
        pendingSentText_.clear();
}

void ChatView::setDraft(const QString& text) {
    editingDraft_ = true;
    quote_ = ChatFeatures::splitQuote(text);
    composer_->clear();
    QTextCursor cursor(composer_->document());
    insertText(cursor, quote_.body, QTextCharFormat(), chatTextSize_ + 3);
    composer_->setTextCursor(cursor);
    composer_->setCurrentCharFormat(QTextCharFormat());
    editingDraft_ = false;
    showQuotePreview();
    updateComposer();
}

QString ChatView::draft() const {
    return quote_.prefix + composerText();
}

void ChatView::clearDraft() {
    setDraft(QString());
}

void ChatView::setSendingState(bool canSend, int maxBytes) {
    canSend_ = canSend;
    maxBytes_ = maxBytes;
    updateComposer();
}

void ChatView::setPinned(bool pinned) {
    pinAction_->setText(pinned ? ui("Открепить чат") : ui("Закрепить чат"));
}

void ChatView::toggleMessageSearch(bool open) {
    searchRow_->setVisible(open);
    if (open) {
        searchInput_->setFocus();
    } else {
        searchInput_->clear();
        searchButton_->setFocus();
    }
}

void ChatView::updateMessageSearch() {
    findMessage(SearchStep::Start);
}

void ChatView::nextMessageSearchMatch() {
    findMessage(SearchStep::Earlier);
}

void ChatView::findMessage(SearchStep step) {
    const QString query = searchInput_->text().trimmed();
    QTextCursor match;
    QTextDocument* document = history_->document();
    const int end = document->characterCount() - 1;
    const int previous = qMin(searchPosition_, end);
    if (!query.isEmpty()) {
        if (step == SearchStep::Earlier && previous >= 0)
            match = document->find(query, previous, QTextDocument::FindBackward);
        if (step == SearchStep::Refresh && previous >= 0) {
            match = document->find(query, previous);
            if (!match.isNull() && match.selectionStart() != previous)
                match = QTextCursor();
        }
        if (match.isNull())
            match = document->find(query, end, QTextDocument::FindBackward);
    }
    searchStatus_->setVisible(!query.isEmpty() && match.isNull());
    searchNext_->setEnabled(!match.isNull());
    searchPosition_ = match.isNull() ? -1 : match.selectionStart();

    QList<QTextEdit::ExtraSelection> highlights;
    if (!match.isNull()) {
        QTextEdit::ExtraSelection highlight;
        highlight.cursor = match;
        highlight.format.setBackground(palette_.selection);
        highlights.append(highlight);
        if (step != SearchStep::Refresh || previous < 0) {
            match.setPosition(searchPosition_);
            history_->setTextCursor(match);
            history_->ensureCursorVisible();
        }
    }
    history_->setExtraSelections(highlights);
}

QString ChatView::composerText(int start, int end) const {
    QString result;
    QTextDocument* document = composer_->document();
    if (end < 0)
        end = document->characterCount() - 1;
    if (start >= end)
        return result;
    for (QTextBlock block = document->findBlock(start); block.isValid() && block.position() < end;
         block = block.next()) {
        for (QTextBlock::Iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int begin = qMax(start, fragment.position());
            const int length = qMin(end, fragment.position() + fragment.length()) - begin;
            if (length <= 0)
                continue;
            const QTextCharFormat format = fragment.charFormat();
            if (format.isImageFormat() && format.hasProperty(originalEmoji)) {
                const QString original = format.stringProperty(originalEmoji);
                for (int n = 0; n < length; ++n)
                    result += original;
            } else {
                result += fragment.text().mid(begin - fragment.position(), length);
            }
        }
        const int separator = block.position() + block.length() - 1;
        if (separator >= start && separator < end)
            result += QLatin1Char('\n');
    }
    return result;
}

QString ChatView::outgoingText(const QString& body) const {
    return quote_.prefix + MessageText::pack(body, MessageText::preferences());
}

void ChatView::showQuotePreview() {
    quotePreview_->setVisible(quote_.valid());
    if (!quote_.valid())
        return;
    quoteText_->clear();
    QTextCursor cursor(quoteText_->document());
    const int quoteSize = qMax(12, chatTextSize_ - 1);
    insertText(cursor, quote_.author.isEmpty() ? ui("Цитата") : quote_.author,
               textColor(palette_.accent, true, quoteSize), quoteSize + 2);
    cursor.insertBlock();
    insertText(cursor, MessageText::restore(quote_.excerpt, MessageText::preferences()),
               textColor(palette_.text, false, quoteSize), quoteSize + 2);
}

void ChatView::removeQuote() {
    quote_ = ChatFeatures::ReplyQuote();
    showQuotePreview();
    updateComposer();
    composer_->setFocus();
}

void ChatView::updateComposer() {
    if (editingDraft_)
        return;
    const QString text = composerText();
    const int bytes = outgoingText(text).toUtf8().size();
    byteLimit_->setText(maxBytes_ > 0 ? ui("%1 из %2 байт UTF-8").arg(bytes).arg(maxBytes_) : QString());
    byteLimit_->setVisible(maxBytes_ > 0);
    attachmentButton_->setEnabled(maxBytes_ > 0);
    sendButton_->setEnabled(canSend_ && maxBytes_ > 0 && bytes <= maxBytes_ && !text.trimmed().isEmpty()
                            && !text.contains(QChar(0)));
}

void ChatView::chooseAttachment() {
    if (maxBytes_ <= 0 || !Attachments::ready() || activeId_.isEmpty())
        return;
    auto* dialog = new AttachmentUploadPanel(outgoingText(composerText()), maxBytes_, this);
    dialog->setProperty("attachmentChat", activeId_);
    connect(dialog, SIGNAL(linkReady(QString)), this, SLOT(insertAttachment(QString)));
    dialog->open();
}

void ChatView::insertAttachment(const QString& link) {
    if (!sender() || sender()->property("attachmentChat").toString() != activeId_ || link.isEmpty())
        return;
    const QString text = composerText();
    const QString separator = text.isEmpty() || text.endsWith(QLatin1Char('\n')) ? QString() : QLatin1String("\n");
    if (outgoingText(text + separator + link).toUtf8().size() > maxBytes_)
        return;
    QTextCursor cursor(composer_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(separator + link, QTextCharFormat());
    composer_->setTextCursor(cursor);
    composer_->setCurrentCharFormat(QTextCharFormat());
}

void ChatView::send() {
#ifdef Q_OS_SYMBIAN
    if (composer_->hasFocus()) {
        if (QInputContext* context = composer_->inputContext())
            context->reset();
    }
#endif
    if (sendButton_->isEnabled())
        emit sendRequested(outgoingText(composerText()));
}

void ChatView::queueMessageActions(int index, bool blockedRun, const QString& reactionEmoji) {
    if (index < 0 || index >= messages_.size() || !isVisible() || activeId_.isEmpty() ||
            messageActions_ || !actionTarget_.isEmpty())
        return;
    actionMessage_ = messages_.at(index);
    actionTarget_ = activeId_;
    actionBlockedRun_ = blockedRun;
    actionReaction_ = reactionEmoji;
    actionNewerTexts_.clear();
    if (reactionEmoji.isEmpty()) {
        for (int i = index + 1; i < messages_.size(); ++i) {
            const ChatMessage& next = messages_.at(i);
            if (next.author == actionMessage_.author && next.outgoing == actionMessage_.outgoing)
                actionNewerTexts_.append(next.text);
        }
    }
    QTimer::singleShot(0, this, SLOT(showMessageActions()));
}

void ChatView::showMessageActions() {
    if (actionTarget_.isEmpty() || actionTarget_ != activeId_ || !isVisible() || messageActions_)
        return;
    const ChatMessage& message = actionMessage_;
    if (actionBlockedRun_) {
        actionTarget_.clear();
        history_->toggleBlockedRun(message);
        return;
    }
    if (!actionReaction_.isEmpty()) {
        const ChatMessage selected = message;
        const QString reaction = actionReaction_;
        actionTarget_.clear();
        actionReaction_.clear();
        showReactionAuthors(selected, reaction);
        return;
    }

    QWidget* content = new QWidget(this);
    content->setObjectName(QLatin1String("messageActions"));
    QVBoxLayout* actions = new QVBoxLayout(content);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(4);
    const auto addRow = [this, content, actions](MessageMenuAction choice, const char* name,
                                                 const QString& text) {
        QPushButton* row = makeButton(text, content, name);
        row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        row->setProperty("messageMenuAction", int(choice));
        row->setProperty("messageActionRow", true);
        connect(row, SIGNAL(clicked()), this, SLOT(selectMessageAction()), Qt::QueuedConnection);
        actions->addWidget(row);
        return row;
    };
    if (message.canReact) {
        QWidget* strip = new QWidget(content);
        strip->setObjectName(QLatin1String("reactionStrip"));
        strip->setAttribute(Qt::WA_StyledBackground);
        QHBoxLayout* layout = new QHBoxLayout(strip);
        layout->setContentsMargins(4, 5, 4, 5);
        layout->setSpacing(4);
        const char* names[] = {"Палец вверх", "Сердце", "Смех", "Удивление", "Грусть", "Спасибо"};
        for (int i = 0; i <= moreReactions; ++i) {
            const bool more = i == moreReactions;
            QPushButton* tile = new QPushButton(more ? QLatin1String("+") : QString(), strip);
            tile->setObjectName(QLatin1String("quickReaction-") + QString::number(i));
            tile->setProperty("accessibleName", more ? ui("Другие реакции") : ui(names[i]));
            tile->setProperty("messageMenuAction", FirstQuickReaction + i);
            tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            tile->setMinimumWidth(30);
            tile->setFixedHeight(39);
            if (!more) {
                const int index = quickReactions[i];
                tile->setIcon(QIcon(QPixmap::fromImage(emojiImage(index))));
                tile->setIconSize(QSize(23, 23));
                tile->setProperty("own", ownsReaction(message, ChatFeatures::emojiValue(index)));
            }
            connect(tile, SIGNAL(clicked()), this, SLOT(selectMessageAction()), Qt::QueuedConnection);
            layout->addWidget(tile);
        }
        actions->addWidget(strip);
        if (ownsReaction(message))
            addRow(RemoveReaction, "messageActionRemoveReaction", ui("Убрать реакцию"));
    }
    addRow(Reply, "messageActionReply", ui("Ответить"));
    if (message.outgoing) {
        QPushButton* repeat = addRow(Resend, "messageActionResend", ui("Отправить заново"));
        repeat->setEnabled(canSend_ && maxBytes_ > 0 && message.text.toUtf8().size() <= maxBytes_);
    }
    if (std::any_of(message.reactions.constBegin(), message.reactions.constEnd(), visibleReaction))
        addRow(ReactionPeople, "messageActionPeople", ui("Кто поставил реакции"));
    addRow(MessageDetails, "messageActionDetails", ui("Подробности"));
    if (!message.blockId.isEmpty()) {
        QPushButton* block = addRow(BlockSender, "messageActionBlock",
            (message.blocked ? ui("Разблокировать «%1»") : ui("Заблокировать «%1»"))
                .arg(message.author.left(nameLimit).replace(QLatin1String("&"), QLatin1String("&&"))));
        block->setProperty("danger", !message.blocked);
    }
    messageActions_ = showCompactSheet(this, content, ui("Действия с сообщением"),
                                      SLOT(closeMessageActions()), [this] { closeMessageActions(); });
    messageActions_->setObjectName(QLatin1String("messageActionsOverlay"));
}

bool ChatView::dismissMessageActions(bool animate) {
    if (!messageActions_)
        return false;
    QWidget* sheet = messageActions_.data();
    messageActions_ = nullptr;
    actionTarget_.clear();
    dismissCompactSheet(sheet, animate);
    if (animate && isVisible())
        history_->setFocus(Qt::OtherFocusReason);
    return true;
}

void ChatView::closeMessageActions() {
    dismissMessageActions();
}

void ChatView::hideEvent(QHideEvent* event) {
    dismissMessageActions(false);
    actionTarget_.clear();
    QWidget::hideEvent(event);
}

void ChatView::selectMessageAction() {
    auto* button = qobject_cast<QPushButton*>(sender());
    if (!button || !button->isEnabled() || !messageActions_ || !messageActions_->isAncestorOf(button))
        return;
    const int choice = button->property("messageMenuAction").toInt();
    const ChatMessage message = actionMessage_;
    const QStringList newer = actionNewerTexts_;
    const QString target = actionTarget_;
    dismissMessageActions(false);
    if (target != activeId_ || !isVisible())
        return;

    if (choice >= FirstQuickReaction) {
        const int quick = choice - FirstQuickReaction;
        if (quick < moreReactions)
            reactTo(message, ChatFeatures::emojiValue(quickReactions[quick]));
        else if (quick == moreReactions) {
            const int index = pickEmoji(ui("Выбрать реакцию"));
            if (index >= 0 && activeId_ == target)
                reactTo(message, ChatFeatures::emojiValue(index));
        }
        return;
    }

    switch (choice) {
    case RemoveReaction:
        emit reactionRequested(message, QString());
        break;
    case Reply:
        replyTo(message, newer);
        break;
    case Resend:
        if (canSend_ && maxBytes_ > 0 && message.text.toUtf8().size() <= maxBytes_)
            emit resendRequested(message.text);
        break;
    case ReactionPeople:
        showReactionAuthors(message);
        break;
    case MessageDetails:
        showDetails(message);
        break;
    case BlockSender: {
        const bool byName = message.blockId.startsWith(QLatin1String("n:"));
        const QString confirmation = byName
            ? ui("Скрыть сообщения всех, кто подписывается именем «%1»?").arg(message.author)
            : ui("Скрыть сообщения автора «%1» в комнатах под любым именем?").arg(message.author);
        if ((message.blocked || QMessageBox::question(this, ui("Заблокировать отправителя"), confirmation,
                                                       QMessageBox::Yes | QMessageBox::No,
                                                       QMessageBox::No) == QMessageBox::Yes) &&
                activeId_ == target) {
            emit senderBlockRequested(message.blockId, message.author, !message.blocked);
        }
        break;
    }
    }
}

void ChatView::reactTo(const ChatMessage& message, const QString& emoji) {
    emit reactionRequested(message, ownsReaction(message, emoji) ? QString() : emoji);
}

void ChatView::replyTo(const ChatMessage& message, const QStringList& newerTexts) {
    const QString body = composerText();
    const QString author = message.author.isEmpty() ? ui("Узел") : message.author;
    const int available = maxBytes_ - qMax(1, MessageText::pack(body, MessageText::preferences()).toUtf8().size());
    const QString prefix = ChatFeatures::makeQuotePrefix(author, message.text, newerTexts, available);
    if (prefix.isEmpty()) {
        byteLimit_->setVisible(true);
        byteLimit_->setText(ui("В черновике мало места для цитаты"));
        return;
    }
    quote_ = ChatFeatures::splitQuote(prefix);
    showQuotePreview();
    updateComposer();
    composer_->setFocus();
}

void ChatView::showDetails(const ChatMessage& message) {
    QDialog dialog(this);
    dialog.setObjectName(QLatin1String("messageDetails"));
    dialog.setWindowTitle(ui("Подробности сообщения"));
    dialog.setStyleSheet(paletteStyle(
        "QDialog#messageDetails { background: @canvas; }"
        "QTextEdit { background: @chrome; color: @text; border: 1px solid @border; font-size: @sizepx; }"
        "QPushButton { color: @text; background: @surface; border: 1px solid @border; }",
        palette_, chatTextSize_));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    auto* text = new MessageDetailsText(palette_.chrome, &dialog);
    text->setReadOnly(true);
    text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QTextCursor cursor(text->document());
    text->setBubble(insertMessage(cursor, message, 0, MessageText::preferences(), palette_, chatTextSize_));
    insertReactionAuthors(cursor, message.reactions, palette_, QString(), true);
    const QTextCharFormat format = textColor(palette_.muted);
    const QString route = routeText(message);
    if (!route.isEmpty()) {
        cursor.insertBlock();
        cursor.insertText(ui("Маршрут"), textColor(palette_.accent, true));
        cursor.insertBlock();
        insertText(cursor, route, format);
    }
    cursor.insertBlock();
    cursor.insertText(ui("%1 байт UTF-8").arg(message.text.toUtf8().size()), format);
    layout->addWidget(text, 1);
    QPushButton* close = makeButton(ui("Назад"), &dialog, "closeMessageDetails");
    layout->addWidget(close);
    connect(close, SIGNAL(clicked()), &dialog, SLOT(reject()));
    dialog.resize(qMax(270, width() - 18), qMin(420, qMax(250, height() - 36)));
    dialog.exec();
}

void ChatView::showReactionAuthors(const ChatMessage& message, const QString& emoji) {
    QDialog dialog(this);
    dialog.setObjectName(QLatin1String("reactionAuthors"));
    const QString title = ui("Кто поставил реакции");
    dialog.setWindowTitle(title);
    dialog.setStyleSheet(paletteStyle(
        "QDialog#reactionAuthors { background: @canvas; }"
        "QLabel { color: @text; font-weight: bold; }"
        "QTextEdit { background: @chrome; color: @text; border: 1px solid @border; font-size: @sizepx; }"
        "QPushButton { color: @text; background: @surface; border: 1px solid @border; }",
        palette_, chatTextSize_));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->addWidget(makeLabel(title, &dialog, "reactionAuthorsHeading"));
    QTextEdit* text = new QTextEdit(&dialog);
    text->setObjectName(QLatin1String("reactionAuthorsText"));
    text->setReadOnly(true);
    text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QTextCursor cursor(text->document());
    insertReactionAuthors(cursor, message.reactions, palette_, emoji, false);
    layout->addWidget(text, 1);
    QPushButton* close = makeButton(ui("Назад"), &dialog, "closeReactionAuthors");
    layout->addWidget(close);
    connect(close, SIGNAL(clicked()), &dialog, SLOT(reject()));
    dialog.resize(qMax(270, width() - 18), qMin(360, qMax(200, height() - 36)));
    dialog.exec();
}

int ChatView::pickEmoji(const QString& title) {
    QDialog dialog(this);
    dialog.setObjectName(QLatin1String("emojiPicker"));
    dialog.setWindowTitle(title);
    dialog.setStyleSheet(paletteStyle(
        "QDialog#emojiPicker { background: @canvas; }"
        "QLabel { color: @text; font-weight: bold; }"
        "QPushButton { color: @text; background: @surface; border: 1px solid @border; border-radius: 6px; }"
        "QPushButton:pressed, QPushButton:focus { border: 1px solid @accent; }"
        "QTabBar::tab { color: @muted; background: @surface; padding: 9px 4px; }"
        "QTabBar::tab:selected { color: @accent; background: @chrome; }", palette_, chatTextSize_));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(9, 8, 9, 8);
    layout->setSpacing(6);
    QHBoxLayout* heading = new QHBoxLayout;
    heading->addWidget(makeLabel(title, &dialog, "emojiHeading"), 1);
    QPushButton* close = makeButton(ui("Назад"), &dialog, "closeEmoji");
    connect(close, SIGNAL(clicked()), &dialog, SLOT(reject()));
    heading->addWidget(close);
    layout->addLayout(heading);

    QStackedWidget* pages = new QStackedWidget(&dialog);
    pages->setObjectName(QLatin1String("emojiPages"));
    QTabBar* categories = new QTabBar(&dialog);
    categories->setObjectName(QLatin1String("emojiCategories"));
    categories->setExpanding(true);
    categories->setDrawBase(false);
    categories->setUsesScrollButtons(false);
    categories->setElideMode(Qt::ElideRight);
    QSignalMapper* selection = new QSignalMapper(&dialog);
    const char* names[] = {"Частые", "Лица", "Жесты", "Знаки", "Разное"};
    const int starts[] = {0, 24, 36, 42, 51, emojiTotal};
    for (int page = 0; page < 5; ++page) {
        categories->addTab(ui(names[page]));
        QWidget* panel = new QWidget(pages);
        QGridLayout* grid = new QGridLayout(panel);
        grid->setSpacing(4);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (int index = starts[page]; index < starts[page + 1]; ++index) {
            QPushButton* tile = new QPushButton(QIcon(QPixmap::fromImage(emojiImage(index))), QString(), panel);
            tile->setObjectName(QLatin1String("emoji-") + QString::number(index));
            tile->setProperty("accessibleName", ChatFeatures::emojiValue(index));
            tile->setIconSize(QSize(30, 30));
            tile->setFixedSize(42, 42);
            const int cell = index - starts[page];
            grid->addWidget(tile, cell / 6, cell % 6);
            selection->setMapping(tile, index + 2);
            connect(tile, SIGNAL(clicked()), selection, SLOT(map()));
        }
        pages->addWidget(panel);
    }
    connect(categories, SIGNAL(currentChanged(int)), pages, SLOT(setCurrentIndex(int)));
    connect(selection, SIGNAL(mapped(int)), &dialog, SLOT(done(int)));
    layout->addWidget(pages, 1);
    layout->addWidget(categories);
    dialog.resize(qMax(300, width() - 18), 310);
    const int picked = dialog.exec() - 2;
    return picked >= 0 && picked < emojiTotal ? picked : -1;
}

void ChatView::chooseEmoji() {
    const QString target = activeId_;
    const int picked = pickEmoji(ui("Смайлики"));
    if (picked < 0 || target != activeId_)
        return;
    const QString value = ChatFeatures::emojiValue(picked);
    const QTextCursor selectionCursor = composer_->textCursor();
    const QString candidate = composerText(0, selectionCursor.selectionStart()) + value +
                              composerText(selectionCursor.selectionEnd());
    if (maxBytes_ > 0 && outgoingText(candidate).toUtf8().size() > maxBytes_) {
        byteLimit_->setText(ui("Со смайликом сообщение будет слишком длинным"));
        return;
    }
    QTextCursor cursor = selectionCursor;
    insertEmoji(cursor, picked, value, chatTextSize_ + 3);
    composer_->setTextCursor(cursor);
    composer_->setCurrentCharFormat(QTextCharFormat());
    composer_->setFocus();
}
