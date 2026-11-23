#ifndef OMMESH_BELLE_SETTINGS_ICONS_HPP
#define OMMESH_BELLE_SETTINGS_ICONS_HPP

#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>

namespace SettingsIcons {

enum Kind {
    Search, Node, Name, Frequency, Readings, Contacts, Privacy, Advanced,
    Notifications, Messages, Attachments, Backup, Appearance, Connection,
    Air, Log, Power, About, Chevron, Back, Gear, Map, Channel, Reconnect, Wlan,
    Copy, Lock, More, Terminal, RouteCheck, Sound, Close, Check, Chat
};

inline void draw(QPainter& painter, Kind kind, const QRect& bounds, const QColor& color) {
    painter.save();
    painter.translate(bounds.x(), bounds.y());
    painter.scale(bounds.width() / 24.0, bounds.height() / 24.0);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    QPainterPath outline;
    switch (kind) {
    case Search:
        painter.drawEllipse(QRectF(4, 4, 12, 12));
        painter.drawLine(QPointF(14.4, 14.4), QPointF(20, 20));
        break;
    case Copy:
        painter.drawRoundedRect(QRectF(8, 8, 12, 12), 1.8, 1.8);
        outline.moveTo(5, 16);
        outline.lineTo(4, 16);
        outline.lineTo(4, 4);
        outline.lineTo(16, 4);
        outline.lineTo(16, 5);
        break;
    case Lock:
        painter.drawRoundedRect(QRectF(5, 10, 14, 11), 2, 2);
        outline.moveTo(8, 10);
        outline.lineTo(8, 7);
        outline.cubicTo(8, 2, 16, 2, 16, 7);
        outline.lineTo(16, 10);
        break;
    case More:
        painter.drawPoint(QPointF(5, 12));
        painter.drawPoint(QPointF(12, 12));
        painter.drawPoint(QPointF(19, 12));
        break;
    case Terminal:
        painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        outline.moveTo(7, 9);
        outline.lineTo(10, 12);
        outline.lineTo(7, 15);
        painter.drawLine(QPointF(13, 15), QPointF(17, 15));
        break;
    case RouteCheck:
        outline.moveTo(4, 8);
        outline.lineTo(19, 8);
        outline.moveTo(15, 4);
        outline.lineTo(19, 8);
        outline.lineTo(15, 12);
        outline.moveTo(20, 16);
        outline.lineTo(5, 16);
        outline.moveTo(9, 12);
        outline.lineTo(5, 16);
        outline.lineTo(9, 20);
        break;
    case Sound:
        outline.moveTo(4, 9);
        outline.lineTo(8, 9);
        outline.lineTo(13, 5);
        outline.lineTo(13, 19);
        outline.lineTo(8, 15);
        outline.lineTo(4, 15);
        outline.closeSubpath();
        painter.drawArc(QRectF(8, 7, 11, 10), -53 * 16, 106 * 16);
        painter.drawArc(QRectF(5, 4, 18, 16), -49 * 16, 98 * 16);
        break;
    case Close:
        painter.drawLine(QPointF(6, 6), QPointF(18, 18));
        painter.drawLine(QPointF(6, 18), QPointF(18, 6));
        break;
    case Check:
        outline.moveTo(5, 12);
        outline.lineTo(10, 17);
        outline.lineTo(19, 7);
        break;
    case Chat:
        outline.moveTo(4, 19);
        outline.lineTo(4, 7);
        outline.quadTo(4, 4, 8, 4);
        outline.lineTo(17, 4);
        outline.quadTo(21, 4, 21, 8);
        outline.lineTo(21, 13);
        outline.quadTo(21, 17, 17, 17);
        outline.lineTo(9, 17);
        outline.lineTo(4, 20);
        outline.closeSubpath();
        painter.drawPoint(QPointF(9, 11));
        painter.drawPoint(QPointF(13, 11));
        painter.drawPoint(QPointF(17, 11));
        break;
    case Node:
    case Frequency:
        painter.drawEllipse(QRectF(10.2, 10.2, 3.6, 3.6));
        for (int radius = 6; radius <= 10; radius += 4) {
            const QRectF wave(12 - radius, 12 - radius, radius * 2, radius * 2);
            painter.drawArc(wave, -46 * 16, 92 * 16);
            painter.drawArc(wave, 134 * 16, 92 * 16);
        }
        break;
    case Contacts:
        painter.drawArc(QRectF(12, 4, 7, 7), -98 * 16, 195 * 16);
        outline.moveTo(15, 13.5);
        outline.cubicTo(18, 13.5, 20.5, 15.5, 20.5, 18.5);
        // The first contact uses the same silhouette as the Name row.
        [[fallthrough]];
    case Name: {
        const int center = kind == Contacts ? 9 : 12;
        painter.drawEllipse(QRectF(center - 3.2, 4, 6.4, 6.4));
        outline.moveTo(center - 5.8, 19.5);
        outline.cubicTo(center - 5.8, 11.7, center + 5.8, 11.7, center + 5.8, 19.5);
        break;
    }
    case Readings:
        painter.drawArc(QRectF(3.8, 7.5, 16.4, 15), 11 * 16, 158 * 16);
        painter.drawLine(QPointF(12, 15), QPointF(15.2, 11.5));
        break;
    case Privacy:
        outline.moveTo(12, 3.2);
        outline.lineTo(19, 6.3);
        outline.lineTo(19, 11);
        outline.cubicTo(19, 15.4, 16, 18.6, 12, 20.6);
        outline.cubicTo(8, 18.6, 5, 15.4, 5, 11);
        outline.lineTo(5, 6.3);
        outline.closeSubpath();
        break;
    case Advanced:
        for (int y = 8; y <= 16; y += 8) {
            const int knob = y == 8 ? 15 : 9;
            painter.drawLine(QPointF(3.5, y), QPointF(knob - 2, y));
            painter.drawLine(QPointF(knob + 2, y), QPointF(20.5, y));
            painter.drawEllipse(QRectF(knob - 2, y - 2, 4, 4));
        }
        break;
    case Notifications:
        outline.moveTo(5, 16.5);
        outline.cubicTo(7, 14.5, 7, 13.1, 7, 9);
        outline.cubicTo(7, 2.5, 17, 2.5, 17, 9);
        outline.cubicTo(17, 13.1, 17, 14.5, 19, 16.5);
        outline.closeSubpath();
        outline.moveTo(10, 19.2);
        outline.quadTo(12, 22, 14, 19.2);
        break;
    case Messages:
        painter.drawLine(QPointF(5.5, 5.5), QPointF(18.5, 5.5));
        painter.drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
        painter.drawLine(QPointF(8.5, 18.5), QPointF(15.5, 18.5));
        break;
    case Attachments:
        outline.moveTo(16, 8);
        outline.lineTo(9, 15);
        outline.cubicTo(7.4, 16.6, 9.6, 19, 11.3, 17.4);
        outline.lineTo(18.7, 10);
        outline.cubicTo(22.5, 6.2, 17.4, 1.1, 13.6, 4.9);
        outline.lineTo(5.9, 12.6);
        outline.cubicTo(0.3, 18.2, 7.7, 24.3, 12.7, 19.5);
        outline.lineTo(19, 13.2);
        break;
    case Backup:
        outline.moveTo(6, 3.5);
        outline.lineTo(14.5, 3.5);
        outline.lineTo(18.5, 7.5);
        outline.lineTo(18.5, 20.5);
        outline.lineTo(6, 20.5);
        outline.closeSubpath();
        outline.moveTo(14.5, 3.5);
        outline.lineTo(14.5, 7.5);
        outline.lineTo(18.5, 7.5);
        painter.drawLine(QPointF(9.5, 14.5), QPointF(15, 14.5));
        break;
    case Appearance:
        outline.moveTo(12, 4);
        outline.cubicTo(7.2, 4, 3.8, 7.3, 3.8, 12);
        outline.cubicTo(3.8, 16.8, 7.4, 20, 11.7, 20);
        outline.cubicTo(14.3, 20, 13.1, 17.8, 13.1, 16.9);
        outline.cubicTo(13.1, 14.8, 15.2, 14.5, 17.5, 14.5);
        outline.cubicTo(22.1, 14.5, 21.2, 4, 12, 4);
        outline.closeSubpath();
        painter.drawPoint(QPointF(8.1, 12.2));
        painter.drawPoint(QPointF(10.7, 8.1));
        painter.drawPoint(QPointF(15.3, 8.5));
        break;
    case Connection:
        outline.moveTo(7, 7);
        outline.lineTo(17.6, 15.9);
        outline.lineTo(12, 20.5);
        outline.lineTo(12, 3.5);
        outline.lineTo(17.6, 8.1);
        outline.lineTo(7, 17);
        break;
    case Air:
        for (int y = 7; y <= 17; y += 5) {
            outline.moveTo(3.5, y);
            outline.cubicTo(6.3, y - 4, 9.2, y + 4, 12, y);
            outline.cubicTo(14.8, y - 4, 17.7, y + 4, 20.5, y);
        }
        break;
    case Log:
        painter.drawRoundedRect(QRectF(5.5, 3.5, 13, 17), 1.1, 1.1);
        for (int y = 8; y <= 16; y += 4)
            painter.drawLine(QPointF(9, y), QPointF(y == 16 ? 13 : 15, y));
        break;
    case Power:
        painter.drawArc(QRectF(4.3, 5.2, 15.4, 15.4), 129 * 16, 282 * 16);
        painter.drawLine(QPointF(12, 3), QPointF(12, 11.7));
        break;
    case About:
        painter.drawEllipse(QRectF(4, 4, 16, 16));
        painter.drawPoint(QPointF(12, 8.2));
        painter.drawLine(QPointF(12, 11.5), QPointF(12, 16.2));
        break;
    case Chevron:
        outline.moveTo(9.4, 5.8);
        outline.lineTo(15.2, 12);
        outline.lineTo(9.4, 18.2);
        break;
    case Back:
        outline.moveTo(14.6, 5.8);
        outline.lineTo(8.8, 12);
        outline.lineTo(14.6, 18.2);
        break;
    case Gear: {
        const QPointF teeth[] = {
            {14, 3}, {14.4, 5.3}, {16.2, 6.3}, {18.3, 5.5}, {20.3, 9},
            {18.6, 10.5}, {18.6, 13.5}, {20.3, 15}, {18.3, 18.5}, {16.2, 17.7}, {14.4, 18.7},
            {14, 21}, {10, 21}, {9.6, 18.7}, {7.8, 17.7}, {5.7, 18.5}, {3.7, 15},
            {5.4, 13.5}, {5.4, 10.5}, {3.7, 9}, {5.7, 5.5}, {7.8, 6.3}, {9.6, 5.3}
        };
        outline.moveTo(10, 3);
        for (const QPointF& tooth : teeth)
            outline.lineTo(tooth);
        outline.closeSubpath();
        painter.drawEllipse(QRectF(9, 9, 6, 6));
        break;
    }
    case Map:
        outline.moveTo(4, 6.1);
        outline.lineTo(9.4, 4);
        outline.lineTo(14.6, 6.1);
        outline.lineTo(20, 4);
        outline.lineTo(20, 17.9);
        outline.lineTo(14.6, 20);
        outline.lineTo(9.4, 17.9);
        outline.lineTo(4, 20);
        outline.closeSubpath();
        painter.drawLine(QPointF(9.4, 4), QPointF(9.4, 17.9));
        painter.drawLine(QPointF(14.6, 6.1), QPointF(14.6, 20));
        break;
    case Channel:
        painter.drawLine(QPointF(5.2, 9), QPointF(19.8, 9));
        painter.drawLine(QPointF(4.2, 15), QPointF(18.8, 15));
        painter.drawLine(QPointF(10.5, 4.8), QPointF(7.5, 19.2));
        painter.drawLine(QPointF(16.5, 4.8), QPointF(13.5, 19.2));
        break;
    case Reconnect:
        painter.drawArc(QRectF(4.5, 4.5, 15, 15), 22 * 16, 285 * 16);
        outline.moveTo(19.4, 5.2);
        outline.lineTo(19.4, 9.4);
        outline.lineTo(15.4, 9.4);
        break;
    case Wlan:
        for (int radius = 5; radius <= 13; radius += 4)
            painter.drawArc(QRectF(12 - radius, 18 - radius, radius * 2, radius * 2),
                            49 * 16, 82 * 16);
        painter.drawPoint(QPointF(12, 18));
        break;
    }

    if (!outline.isEmpty())
        painter.drawPath(outline);
    painter.restore();
}

inline QIcon icon(Kind kind, const QColor& color, int size = 22) {
    QPixmap pixels(size, size);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    draw(painter, kind, pixels.rect(), color);
    painter.end();
    return QIcon(pixels);
}

}  // namespace SettingsIcons

#endif
