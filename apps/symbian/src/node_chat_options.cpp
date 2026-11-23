#include "node_chat_options.hpp"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtGui/QBrush>
#include <QtGui/QDesktopServices>
#include <QtGui/QDialog>
#include <QtGui/QImage>
#include <QtGui/QListWidget>
#include <QtGui/QPainter>
#include <QtGui/QPushButton>
#include <QtGui/QSound>
#include <QtGui/QVBoxLayout>
#include <cmath>

namespace NodeChatOptions {
namespace {

enum { Inherited = -1, Off = 0, All = 1, Mentions = 2 };
const char* const fields[] = {"Уведомления", "Звук", "Фон"};
const char* const keys[] = {"notices", "sound", "background"};
const char* const sounds[] = {"Без звука", "Чирп", "Принято", "Хоп", "Сонар"};
const char* const backgrounds[] = {"Без фона", "Ночной эфир", "Маленький космос", "Бумажный сад",
    "Лаборатория", "Кварталы", "Вне сети", "Гранатовый шёлк", "Маленький праздник"};
const int soundCount = sizeof(sounds) / sizeof(sounds[0]);
const int backgroundCount = sizeof(backgrounds) / sizeof(backgrounds[0]);

QString ui(const char* text) { return QString::fromUtf8(text); }

QString storageKey(Field field, const QString& target) {
    return QLatin1String("chatOptions/") + QString::fromLatin1(target.toUtf8().toHex()) +
        QLatin1Char('/') + QLatin1String(keys[field]);
}

int readChoice(const QSettings& settings, const QString& key, int maximum, int fallback) {
    bool valid = false;
    const int value = settings.value(key, fallback).toInt(&valid);
    return valid && value >= fallback && value <= maximum ? value : fallback;
}

int selected(const QSettings& settings, Field field, const QString& target, bool room) {
    const int maximum = field == Sound ? soundCount - 1 : field == Background ? backgroundCount - 1 : room ? Mentions : All;
    return readChoice(settings, storageKey(field, target), maximum, Inherited);
}

bool defaultNotices(const QSettings& settings, const QString& target) {
    const char* key = target.startsWith(QLatin1String("c/")) ? "ui/notifyChannels" : "ui/notifyDirect";
    return settings.value(QLatin1String(key), true).toBool();
}

int fallback(const QSettings& settings, Field field, const QString& target) {
    if (field == Notifications)
        return defaultNotices(settings, target) ? All : Off;
    if (field == Sound)
        return readChoice(settings, QLatin1String("ui/notificationSound"), soundCount - 1, 0);
    return readChoice(settings, QLatin1String("ui/chatBackground"), backgroundCount - 1, 0);
}

QString description(Field field, int value, bool room) {
    if (field == Sound)
        return ui(sounds[value]);
    if (field == Background)
        return ui(backgrounds[value]);
    if (value == Mentions)
        return ui("Упоминания");
    return value == Off ? ui("Выкл.") : room ? ui("Все") : ui("Вкл.");
}

QString choiceLabel(const QSettings& settings, Field field, const QString& target, bool room, int choice) {
    return choice == Inherited ? ui("По умолчанию (%1)").arg(description(field, fallback(settings, field, target), room))
                               : description(field, choice, room);
}

void appendLittleEndian(QByteArray& bytes, quint32 value, int width) {
    for (int i = 0; i < width; ++i)
        bytes.append(static_cast<char>((value >> (8 * i)) & 0xff));
}

QByteArray waveform(int sound) {
    struct Tone { int from; int to; int ms; };
    const Tone definitions[][5] = {
        {{670, 1170, 75}, {0, 0, 30}, {830, 1510, 75}, {0, 0, 30}, {1150, 550, 95}},
        {{720, 720, 55}, {0, 0, 43}, {720, 720, 140}, {0, 0, 43}, {720, 720, 55}},
        {{530, 530, 65}, {0, 0, 45}, {720, 720, 65}, {0, 0, 45}, {1000, 850, 105}},
        {{910, 850, 93}, {0, 0, 125}, {430, 400, 135}, {0, 0, 0}, {0, 0, 0}}
    };
    const int sampleRate = 16000;
    QByteArray samples;
    samples.reserve(sampleRate);
    for (const Tone& tone : definitions[sound - 1]) {
        const int length = tone.ms * sampleRate / 1000;
        const int fade = sampleRate / 125;
        double phase = 0;
        for (int i = 0; i < length; ++i) {
            const double frequency = tone.from + (tone.to - tone.from) * double(i) / length;
            phase += frequency * 6.283185307179586 / sampleRate;
            const double envelope = qMin(1.0, qMin(i, length - 1 - i) / double(fade));
            const qint16 value = static_cast<qint16>(8500 * envelope * std::sin(phase));
            appendLittleEndian(samples, static_cast<quint16>(value), 2);
        }
    }
    QByteArray wave("RIFF");
    appendLittleEndian(wave, 36 + samples.size(), 4);
    wave.append("WAVEfmt ");
    appendLittleEndian(wave, 16, 4);
    appendLittleEndian(wave, 1, 2);
    appendLittleEndian(wave, 1, 2);
    appendLittleEndian(wave, sampleRate, 4);
    appendLittleEndian(wave, sampleRate * 2, 4);
    appendLittleEndian(wave, 2, 2);
    appendLittleEndian(wave, 16, 2);
    wave.append("data");
    appendLittleEndian(wave, samples.size(), 4);
    return wave + samples;
}

void soundPreview(int sound) {
    if (sound <= 0 || sound >= soundCount)
        return;
    const QString root = QDesktopServices::storageLocation(QDesktopServices::CacheLocation);
    if (root.isEmpty() || !QDir().mkpath(root))
        return;
    const QString fileName = QDir(root).filePath(QString::fromLatin1("chat-signal-%1.wav").arg(sound));
    QFile file(fileName);
    if (file.size() <= 44) {
        const QByteArray wave = waveform(sound);
        if (!file.open(QIODevice::WriteOnly) || file.write(wave) != wave.size()) {
            file.remove();
            return;
        }
        file.close();
    }
    QSound::play(fileName);
}

void pattern(QPainter& painter, int mode) {
    switch (mode) {
    case 1:
        painter.drawLine(12, 13, 40, 34);
        painter.drawEllipse(QPoint(12, 13), 4, 4);
        painter.drawEllipse(QPoint(40, 34), 5, 5);
        painter.drawPoint(13, 44);
        break;
    case 2:
        painter.drawEllipse(QPoint(18, 19), 8, 8);
        painter.drawEllipse(6, 15, 24, 8);
        painter.drawLine(42, 38, 42, 46);
        painter.drawLine(38, 42, 46, 42);
        break;
    case 3:
        painter.drawLine(13, 42, 36, 13);
        painter.drawEllipse(10, 22, 12, 6);
        painter.drawEllipse(27, 24, 13, 7);
        painter.drawEllipse(27, 11, 14, 6);
        break;
    case 4:
        painter.drawEllipse(QPoint(28, 28), 12, 12);
        painter.drawEllipse(13, 23, 30, 10);
        painter.drawLine(28, 10, 28, 46);
        painter.drawPoint(14, 12);
        painter.drawPoint(42, 44);
        break;
    case 5:
        painter.drawRoundedRect(9, 10, 16, 19, 3, 3);
        painter.drawRoundedRect(29, 27, 17, 19, 3, 3);
        painter.drawLine(33, 15, 44, 15);
        break;
    case 6: {
        const QPoint ridge[] = {QPoint(6, 39), QPoint(22, 17), QPoint(31, 27), QPoint(39, 21), QPoint(49, 39)};
        painter.drawPolyline(ridge, sizeof(ridge) / sizeof(ridge[0]));
        painter.drawEllipse(QPoint(39, 11), 4, 4);
        break;
    }
    case 7: {
        const QPoint diamond[] = {QPoint(28, 9), QPoint(43, 28), QPoint(28, 47), QPoint(13, 28)};
        painter.drawPolygon(diamond, sizeof(diamond) / sizeof(diamond[0]));
        painter.drawEllipse(QPoint(28, 28), 6, 6);
        break;
    }
    default:
        painter.drawPoint(11, 13);
        painter.drawPoint(41, 12);
        painter.drawPoint(19, 39);
        painter.drawPoint(44, 42);
        painter.drawLine(25, 12, 29, 16);
        painter.drawLine(10, 32, 14, 28);
        painter.drawLine(33, 37, 36, 42);
        break;
    }
}

} // namespace

QString label(Field field, const QString& target, bool room) {
    const QSettings settings;
    return choiceLabel(settings, field, target, room, selected(settings, field, target, room));
}

bool choose(Field field, const QString& target, bool room, QWidget* parent) {
    if (target.isEmpty())
        return false;
    QSettings settings;
    QDialog dialog(parent);
    dialog.setObjectName(QLatin1String("nodeChatOptions"));
    dialog.setWindowTitle(ui(fields[field]));
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QLatin1String("nodeChatOptionList"));
    list->setProperty("accessibleName", dialog.windowTitle());
    const int active = selected(settings, field, target, room);
    QVector<int> values;
    values << Inherited;
    if (field == Notifications) {
        values << All;
        if (room)
            values << Mentions;
        values << Off;
    } else if (field == Sound) {
        for (int option = 1; option < soundCount; ++option)
            values << option;
        values << Off;
    } else {
        for (int option = 0; option < backgroundCount; ++option)
            values << option;
    }
    for (int option : values) {
        auto* entry = new QListWidgetItem(choiceLabel(settings, field, target, room, option), list);
        entry->setData(Qt::UserRole, option);
        entry->setSizeHint(QSize(0, 44));
        if (option == active)
            list->setCurrentItem(entry);
    }
    layout->addWidget(list);
    auto* back = new QPushButton(ui("Назад"), &dialog);
    back->setObjectName(QLatin1String("closeNodeChatOptions"));
    back->setMinimumHeight(40);
    layout->addWidget(back);
    QObject::connect(back, SIGNAL(clicked()), &dialog, SLOT(reject()));
    QObject::connect(list, SIGNAL(itemClicked(QListWidgetItem*)), &dialog, SLOT(accept()), Qt::QueuedConnection);
    QObject::connect(list, SIGNAL(itemActivated(QListWidgetItem*)), &dialog, SLOT(accept()), Qt::QueuedConnection);
    dialog.resize(330, qMin(515, 68 + 44 * values.size()));
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return false;
    const int option = list->currentItem()->data(Qt::UserRole).toInt();
    const QString key = storageKey(field, target);
    if (option == Inherited)
        settings.remove(key);
    else
        settings.setValue(key, option);
    settings.sync();
    if (field == Sound)
        playNotification(settings, target);
    return option != active;
}

bool allowsNotification(const QSettings& settings, const QString& target, bool room,
                        const QString& message, const QString& ownName) {
    if (settings.value(QLatin1String("ui/doNotDisturb"), false).toBool())
        return false;
    int mode = selected(settings, Notifications, target, room);
    if (mode == Inherited)
        mode = fallback(settings, Notifications, target);
    return mode == All || (mode == Mentions && !ownName.isEmpty() &&
                           message.contains(QLatin1String("@[") + ownName + QLatin1Char(']')));
}

void playNotification(const QSettings& settings, const QString& target) {
    const int mode = selected(settings, Sound, target, false);
    soundPreview(mode == Inherited ? fallback(settings, Sound, target) : mode);
}

int background(const QString& target) {
    const QSettings settings;
    const int mode = selected(settings, Background, target, false);
    return mode == Inherited ? fallback(settings, Background, target) : mode;
}

QBrush backgroundBrush(int mode, const QColor& canvas, const QColor& accent) {
    if (mode <= 0 || mode >= backgroundCount)
        return QBrush(canvas);
    QImage tile(56, 56, QImage::Format_RGB32);
    if (tile.isNull())
        return QBrush(canvas);
    tile.fill(canvas.rgb());
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor ink = accent;
    ink.setAlpha(48);
    painter.setPen(QPen(ink, mode == 8 ? 2 : 1, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    pattern(painter, mode);
    painter.end();
    return QBrush(tile);
}

} // namespace NodeChatOptions
