#pragma once

#include <QtCore/QString>

class QBrush;
class QColor;
class QSettings;
class QWidget;

namespace NodeChatOptions {

enum Field { Notifications, Sound, Background };

QString label(Field field, const QString& target, bool room);
bool choose(Field field, const QString& target, bool room, QWidget* parent);
bool allowsNotification(const QSettings& settings, const QString& target, bool room,
                        const QString& message, const QString& ownName);
void playNotification(const QSettings& settings, const QString& target);

// Background value 0 is the plain conversation canvas.
int background(const QString& target);
QBrush backgroundBrush(int mode, const QColor& canvas, const QColor& accent);

} // namespace NodeChatOptions
