#include "window.hpp"

#include "chat_features.hpp"
#include "demo_radio.hpp"
#include "message_text.hpp"
#include "node_map.hpp"
#include "radio_settings.hpp"
#include "settings_icons.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QButtonGroup>
#include <QtGui/QDialog>
#include <QtGui/QFontMetrics>
#include <QtGui/QHBoxLayout>
#include <QtGui/QIcon>
#include <QtGui/QIntValidator>
#include <QtGui/QKeyEvent>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QListWidget>
#include <QtGui/QMenu>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QPushButton>
#include <QtGui/QScrollArea>
#include <QtGui/QScrollBar>
#include <QtGui/QStackedWidget>
#include <QtGui/QStyledItemDelegate>
#include <QtGui/QToolButton>
#include <QtGui/QVBoxLayout>

#include <algorithm>

namespace {

enum ItemData { ItemId = Qt::UserRole, ItemDetail, ItemStamp, ItemUnread,
                ItemChannel, ItemPinned, ItemRole };
const int ChatRoomRole = -1;

QString ui(const char* utf8) { return QString::fromUtf8(utf8); }

QLabel* label(const QString& text, QWidget* parent, const char* object = nullptr) {
    QLabel* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    if (object)
        result->setObjectName(QLatin1String(object));
    return result;
}

QPushButton* button(const QString& text, QWidget* parent, bool primary = false) {
    QPushButton* result = new QPushButton(text, parent);
    if (primary)
        result->setObjectName(QLatin1String("primary"));
    result->setMinimumHeight(42);
    return result;
}

QVBoxLayout* pageLayout(QWidget* parent) {
    QVBoxLayout* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(14, 12, 14, 10);
    layout->setSpacing(8);
    return layout;
}

QLineEdit* searchBox(const QString& hint, QWidget* parent) {
    QLineEdit* search = new QLineEdit(parent);
    search->setPlaceholderText(hint);
    search->setProperty("accessibleName", hint);
    search->setMinimumHeight(44);
    search->setMaxLength(96);
    search->setInputMethodHints(Qt::ImhNoAutoUppercase);
    return search;
}

QWidget* surface(QWidget* parent, const char* object) {
    QWidget* widget = new QWidget(parent);
    widget->setObjectName(QLatin1String(object));
    widget->setAttribute(Qt::WA_StyledBackground);
    return widget;
}

QString plainLine(QString value) {
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return value.trimmed();
}

QString shortDate(quint32 timestamp) {
    if (timestamp == 0)
        return QString();
    const QDateTime time = QDateTime::fromTime_t(timestamp);
    const qint64 days = time.date().daysTo(QDate::currentDate());
    return days == 0 ? time.toString(QLatin1String("HH:mm")) :
           days == 1 ? ui("Вчера") : time.toString(QLatin1String("dd.MM"));
}

QString roleName(int role) {
    switch (role) {
        case 1: return ui("Собеседник");
        case 2: return ui("Репитер");
        case 3: return ui("Комната");
        case 4: return ui("Датчик");
        default: return ui("Узел");
    }
}

void drawNode(QPainter& painter, const QPoint& center) {
    const QPoint top = center + QPoint(0, -8);
    const QPoint left = center + QPoint(-8, 5);
    const QPoint right = center + QPoint(8, 5);
    painter.drawLine(top, left);
    painter.drawLine(top, right);
    painter.drawLine(left, right);
    painter.drawEllipse(top, 2, 2);
    painter.drawEllipse(left, 2, 2);
    painter.drawEllipse(right, 2, 2);
}

QIcon tabIcon(const Appearance::Palette& colors, int index, int unread = 0, bool active = false) {
    QPixmap pixels(42, 26);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.save();
    painter.translate(8, 1);
    painter.setPen(QPen(colors.muted, 2));
    if (index == 0) {
        painter.drawRoundedRect(QRect(3, 4, 19, 13), 4, 4);
        painter.drawLine(8, 16, 6, 21);
        painter.drawLine(6, 21, 12, 17);
        painter.drawPoint(9, 10);
        painter.drawPoint(13, 10);
        painter.drawPoint(17, 10);
    } else if (index == 1) {
        drawNode(painter, QPoint(13, 12));
    } else {
        SettingsIcons::draw(painter, SettingsIcons::Gear, QRect(1, 0, 24, 24),
                            active ? colors.accent : colors.muted);
    }
    painter.restore();
    if (unread > 0) {
        const QString value = unread > 99 ? ui("99+") : QString::number(unread);
        const int badgeWidth = unread > 99 ? 20 : unread > 9 ? 16 : 13;
        const QRect badge(pixels.width() - badgeWidth, 0, badgeWidth, 14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.accent);
        painter.drawRoundedRect(badge, 7, 7);
        QFont font = painter.font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colors.onAccent);
        painter.drawText(badge, Qt::AlignCenter, value);
    }
    painter.end();
    return QIcon(pixels);
}

QPixmap radioAvatar(const Appearance::Palette& colors) {
    QPixmap icon(46, 46);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Appearance::mix(colors.accent, colors.surface, colors.dark ? 23 : 13));
    painter.drawEllipse(QRect(1, 1, 44, 44));
    SettingsIcons::draw(painter, SettingsIcons::Node, QRect(9, 9, 28, 28), colors.onAccent);
    painter.end();
    return icon;
}

int approximateBatteryPercent(int millivolts) {
    // Estimate charge for a single Li-ion cell only, from its last reported voltage.
    const int curve[] = {4190, 4050, 3990, 3890, 3800, 3720, 3650, 3580, 3530, 3420, 3100};
    if (millivolts < 2800 || millivolts > 4400)
        return -1;
    if (millivolts >= curve[0])
        return 100;
    for (int i = 1; i < int(sizeof(curve) / sizeof(curve[0])); ++i) {
        if (millivolts >= curve[i])
            return 100 - 10 * i + qRound(10.0 * (millivolts - curve[i]) / (curve[i - 1] - curve[i]));
    }
    return 0;
}

QString radioVoltage(int millivolts) {
    return millivolts > 0
        ? ui("%1 В").arg(QString::number(millivolts / 1000.0, 'f', 2)
                             .replace(QLatin1Char('.'), QLatin1Char(',')))
        : QString();
}

void drawRoom(QPainter& painter, const QPoint& center) {
    painter.drawEllipse(center + QPoint(-4, -4), 3, 3);
    painter.drawEllipse(center + QPoint(5, -2), 2, 2);
    painter.drawArc(QRect(center.x() - 10, center.y(), 12, 12), 0, 180 * 16);
    painter.drawArc(QRect(center.x() + 1, center.y() + 1, 9, 9), 0, 155 * 16);
}

void drawPin(QPainter& painter, int x, int y) {
    painter.drawLine(x - 4, y - 5, x + 4, y - 5);
    painter.drawLine(x - 2, y - 5, x - 2, y - 1);
    painter.drawLine(x + 2, y - 5, x + 2, y - 1);
    painter.drawLine(x - 4, y, x + 4, y);
    painter.drawLine(x, y, x, y + 5);
}

const char* const chatFilters[] = {"Все чаты", "Непрочитанные", "Каналы", "Личные", "Закреплённые"};

QSet<QString> roomIds(const Client& client) {
    QSet<QString> rooms;
    for (const NetworkNode& node : client.nodes()) {
        if (node.role == 3)
            rooms.insert(node.id);
    }
    return rooms;
}

class TouchList : public QListWidget {
public:
    explicit TouchList(QWidget* parent) : QListWidget(parent) {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setUniformItemSizes(true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QListWidget::mousePressEvent(event);
            return;
        }
        press_ = event->pos();
        scroll_ = verticalScrollBar()->value();
        dragged_ = false;
        QListWidgetItem* item = itemAt(press_);
        pressedId_ = item ? item->data(ItemId).toString() : QString();
        pressTime_.start();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton) || !pressTime_.isValid()) {
            QListWidget::mouseMoveEvent(event);
            return;
        }
        const QPoint offset = event->pos() - press_;
        if (offset.manhattanLength() > 8)
            dragged_ = true;
        if (dragged_)
            verticalScrollBar()->setValue(scroll_ - offset.y());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !pressTime_.isValid()) {
            QListWidget::mouseReleaseEvent(event);
            return;
        }
        const qint64 held = pressTime_.elapsed();
        pressTime_.invalidate();
        if (!dragged_ && (event->pos() - press_).manhattanLength() <= 8) {
            QListWidgetItem* item = itemAt(event->pos());
            if (item && pressedId_ == item->data(ItemId).toString()) {
                setCurrentItem(item);
                if (held >= 550 && contextMenuPolicy() == Qt::CustomContextMenu)
                    emit customContextMenuRequested(event->pos());
                else
                    emit itemClicked(item);
            }
        }
        event->accept();
    }

private:
    QElapsedTimer pressTime_;
    QPoint press_;
    QString pressedId_;
    int scroll_ = 0;
    bool dragged_ = false;
};

class TouchScrollArea : public QScrollArea {
public:
    explicit TouchScrollArea(QWidget* parent) : QScrollArea(parent) {
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    void setContent(QWidget* content) {
        setWidget(content);
        viewport()->installEventFilter(this);
        content->installEventFilter(this);
        const QList<QWidget*> children = content->findChildren<QWidget*>();
        for (QWidget* child : children)
            child->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove &&
                event->type() != QEvent::MouseButtonRelease)
            return QScrollArea::eventFilter(watched, event);
        QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            pressed_ = true;
            dragging_ = false;
            start_ = mouse->globalPos();
            scroll_ = verticalScrollBar()->value();
            button_ = qobject_cast<QAbstractButton*>(watched);
        } else if (event->type() == QEvent::MouseMove && pressed_ && (mouse->buttons() & Qt::LeftButton)) {
            const QPoint offset = mouse->globalPos() - start_;
            if (qAbs(offset.y()) > 8 && qAbs(offset.y()) > qAbs(offset.x()))
                dragging_ = true;
            if (dragging_) {
                if (button_)
                    button_->setDown(false);
                verticalScrollBar()->setValue(scroll_ - offset.y());
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            pressed_ = false;
            if (dragging_) {
                dragging_ = false;
                return true;
            }
        }
        return QScrollArea::eventFilter(watched, event);
    }

private:
    QPointer<QAbstractButton> button_;
    QPoint start_;
    int scroll_ = 0;
    bool pressed_ = false;
    bool dragging_ = false;
};

class RowPainter : public QStyledItemDelegate {
public:
    explicit RowPainter(QWidget* parent) : QStyledItemDelegate(parent), view_(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const Appearance::Palette colors = Appearance::paletteFor(view_);
        painter->save();
        const QRect area = option.rect;
        painter->setClipRect(area);
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(area, colors.selection);
            painter->fillRect(QRect(area.left(), area.top() + 11, 3, area.height() - 22),
                              colors.accent);
        }

        const QString title = index.data().toString();
        const bool channel = index.data(ItemChannel).toBool();
        const int role = index.data(ItemRole).toInt();
        const int unread = index.data(ItemUnread).toInt();
        const bool isPublic = channel && title.compare(QLatin1String("Public"), Qt::CaseInsensitive) == 0;
        const QColor color = isPublic ? QColor(94, 77, 49) : avatarColor(index.data(ItemId).toString(), role);
        const QPoint avatar(area.left() + 23, area.center().y());
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        if (channel)
            painter->drawRoundedRect(QRect(avatar.x() - 19, avatar.y() - 19, 38, 38), 12, 12);
        else
            painter->drawEllipse(avatar, 19, 19);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QColor(234, 241, 244));

        QFont titleFont = option.font;
        titleFont.setPixelSize(17);
        titleFont.setBold(true);
        painter->setFont(titleFont);
        if (role > 0) {
            drawNode(*painter, avatar);
        } else if (role == ChatRoomRole) {
            drawRoom(*painter, avatar);
        } else {
            const QString icon = channel ? QLatin1String("#") :
                    title.isEmpty() || title.at(0).isHighSurrogate() ? ui("·") : title.left(1).toUpper();
            painter->drawText(QRect(avatar.x() - 17, avatar.y() - 17, 34, 34), Qt::AlignCenter, icon);
        }

        const int left = area.left() + 53;
        const int right = area.right() - 8;
        const int first = area.top() + 10;
        const int second = area.top() + 34;
        QFont smallFont = option.font;
        smallFont.setPixelSize(13);
        QFontMetrics smallMetrics(smallFont);
        const QString stamp = index.data(ItemStamp).toString();
        const int stampWidth = stamp.isEmpty() ? 0 : qMin(60, smallMetrics.width(stamp) + 5);
        const bool pinned = index.data(ItemPinned).toBool();
        const int titleWidth = qMax(0, right - left - stampWidth - (stampWidth ? 8 : 0) - (pinned ? 16 : 0));
        const QFontMetrics titleMetrics(titleFont);
        const QString displayTitle = titleMetrics.elidedText(title, Qt::ElideRight, titleWidth);
        painter->setPen(colors.text);
        painter->drawText(QRect(left, first, titleWidth, 22), Qt::AlignLeft | Qt::AlignVCenter,
                          displayTitle);
        if (pinned) {
            painter->setPen(colors.accent);
            drawPin(*painter, left + qMin(titleWidth, titleMetrics.width(displayTitle)) + 8, first + 11);
        }

        painter->setFont(smallFont);
        painter->setPen(colors.muted);
        painter->drawText(QRect(right - stampWidth, first, stampWidth, 22),
                          Qt::AlignRight | Qt::AlignVCenter,
                          smallMetrics.elidedText(stamp, Qt::ElideRight, stampWidth));
        const int badgeWidth = unread > 99 ? 33 : unread > 9 ? 27 : unread > 0 ? 22 : 0;
        const int detailWidth = qMax(0, right - left - badgeWidth - (badgeWidth ? 8 : 0));
        const QString detail = index.data(ItemDetail).toString();
        if (unread > 0)
            painter->setPen(colors.text);
        painter->drawText(QRect(left, second, detailWidth, 21), Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(detail, Qt::ElideRight, detailWidth));
        if (badgeWidth) {
            const QRect badge(right - badgeWidth, second, badgeWidth, 22);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.accent);
            painter->drawRoundedRect(badge, 11, 11);
            painter->setPen(colors.onAccent);
            painter->drawText(badge, Qt::AlignCenter, unread > 99 ? ui("99+") : QString::number(unread));
        }
        painter->setPen(Appearance::mix(colors.canvas, colors.border, 67));
        painter->drawLine(left, area.bottom(), area.right(), area.bottom());
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, 60);
    }

private:
    QWidget* view_;

    static QColor avatarColor(const QString& id, int role) {
        static const QRgb colors[] = {0x39645d, 0x435d79, 0x695978, 0x6a593f, 0x3a616d};
        const uint index = role > 0 ? static_cast<uint>(role - 1) : qHash(id);
        return QColor::fromRgb(colors[index % (sizeof colors / sizeof colors[0])]);
    }
};

TouchList* makeList(QWidget* parent, const QString& name) {
    TouchList* list = new TouchList(parent);
    list->setProperty("accessibleName", name);
    list->setItemDelegate(new RowPainter(list));
    return list;
}

QListWidgetItem* appendRow(QListWidget* list, const QString& id, const QString& title,
                           const QString& detail, const QString& stamp, int unread,
                           bool channel, bool pinned, int role = 0) {
    QListWidgetItem* item = new QListWidgetItem(title, list);
    item->setData(ItemId, id);
    item->setData(ItemDetail, detail);
    item->setData(ItemStamp, stamp);
    item->setData(ItemUnread, unread);
    item->setData(ItemChannel, channel);
    item->setData(ItemPinned, pinned);
    item->setData(ItemRole, role);
    item->setData(Qt::AccessibleTextRole, title + QLatin1String(" ") + detail);
    return item;
}

struct ChatRow {
    ChatTarget target;
    QString detail;
    quint32 timestamp = 0;
    bool pinned = false;
};

struct ListRow {
    QString id;
    QString title;
    QString detail;
    QString stamp;
    int unread = 0;
    bool channel = false;
    bool pinned = false;
    int role = 0;

    bool matches(const QListWidgetItem& item) const {
        return id == item.data(ItemId).toString() && title == item.text() &&
               detail == item.data(ItemDetail).toString() && stamp == item.data(ItemStamp).toString() &&
               unread == item.data(ItemUnread).toInt() && channel == item.data(ItemChannel).toBool() &&
               pinned == item.data(ItemPinned).toBool() && role == item.data(ItemRole).toInt();
    }
};

void displayRows(QListWidget* list, const QVector<ListRow>& rows) {
    bool unchanged = list->count() == rows.size();
    for (int index = 0; unchanged && index < rows.size(); ++index)
        unchanged = rows[index].matches(*list->item(index));
    if (unchanged)
        return;

    const int scroll = list->verticalScrollBar()->value();
    const QListWidgetItem* current = list->currentItem();
    const QString selected = current ? current->data(ItemId).toString() : QString();
    list->clear();
    for (const ListRow& row : rows) {
        QListWidgetItem* item = appendRow(list, row.id, row.title, row.detail, row.stamp,
                                        row.unread, row.channel, row.pinned, row.role);
        if (row.id == selected)
            list->setCurrentItem(item);
    }
    list->verticalScrollBar()->setValue(scroll);
}

QString themeStyle(const Appearance::Palette& colors) {
    QString style = QLatin1String(
        "#window, #dashboard, #pane, #detailPage, #appearanceSwatches, QDialog {"
        " background: @canvas@; color: @text@; }"
        "#tabs { background: @chrome@; border-top: 1px solid @border@; }"
        "#nodeCard { background: @nodeCard@; border-radius: 13px; }"
        "#settingsReconnectBanner { background: @chrome@; border: 1px solid @border@; border-radius: 10px; }"
        "#settingsGroup { background: @group@; border-radius: 11px; }"
        "QLabel { color: @text@; font-size: 16px; }"
        "QLabel#heading { font-size: 26px; font-weight: bold; }"
        "QLabel#settingsSectionTitle { color: @muted@; font-size: 13px; font-weight: bold; }"
        "QLabel#settingsNodeName { color: @text@; font-size: 19px; font-weight: bold; }"
        "QLabel#settingsBatteryPercent { color: @text@; font-size: 17px; font-weight: bold; }"
        "QLabel#settingsReconnectText { color: @text@; font-size: 13px; }"
        "QLabel#subtitle { font-size: 18px; font-weight: bold; }"
        "QLabel#muted, QLabel#settingsNodeStatus, QLabel#settingsBatteryVoltage { color: @muted@; font-size: 13px; }"
        "QLabel#radioName { color: @text@; font-size: 13px; font-weight: bold; }"
        "QLabel#radioState { color: @muted@; font-size: 12px; }"
        "QLabel#error { color: @danger@; font-size: 14px; }"
        "QCheckBox { color: @text@; font-size: 15px; spacing: 8px; }"
        "QLineEdit, QTextEdit, QComboBox { background: @surface@; color: @text@;"
        " border: 1px solid @border@; border-radius: 9px;"
        " selection-background-color: @accent@; selection-color: @onAccent@; }"
        "QLineEdit { padding: 5px 10px; font-size: 17px; }"
        "QLineEdit:focus, QTextEdit:focus, QComboBox:focus { border-color: @accent@; }"
        "QComboBox { padding: 4px 5px 4px 8px; font-size: 14px; }"
        "QComboBox QAbstractItemView, QMenu { color: @text@; background: @surface@;"
        " selection-background-color: @selection@; }"
        "QMenu::item { padding: 10px 17px; }"
        "QListWidget, QScrollArea { border: 0; background: @canvas@; color: @text@; }"
        "QScrollBar:vertical { background: @chrome@; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: @scrollHandle@; border-radius: 4px; min-height: 26px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: @chrome@; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QPushButton { background: @surface@; color: @text@; border: 1px solid @border@;"
        " border-radius: 8px; padding: 5px 9px; font-size: 15px; }"
        "QPushButton#settingsRetryNow { font-size: 13px; padding: 4px 6px; }"
        "QToolButton#settingsSearchButton, QPushButton#connectionBack, QPushButton#detailBack {"
        " background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0; }"
        "QToolButton#settingsSearchButton:focus, QPushButton#connectionBack:focus, QPushButton#detailBack:focus {"
        " border-color: @border@; }"
        "QToolButton#settingsSearchButton:checked, QToolButton#settingsSearchButton:pressed,"
        " QPushButton#connectionBack:pressed, QPushButton#detailBack:pressed { background: @selection@; }"
        "QPushButton#primary, QPushButton#radioAdvertise { background: @accent@; color: @onAccent@;"
        " border-color: @accent@;"
        " font-weight: bold; }"
        "QPushButton#newChat { color: @accent@; border-color: @border@; background: @selection@;"
        " font-size: 24px; font-weight: bold; }"
        "QPushButton:checked { background: @selection@; color: @text@; border-color: @accent@; }"
        "QPushButton:focus { border-color: @accent@; }"
        "QPushButton:pressed, QPushButton#primary:pressed, QPushButton#newChat:pressed,"
        " QPushButton#radioAdvertise:pressed {"
        " background: @pressed@; color: @text@; }"
        "QPushButton:disabled, QPushButton#radioAdvertise:disabled { background: @chrome@; color: @muted@;"
        " border-color: @border@; }"
        "QToolButton#tabButton { color: @muted@; border: 0; padding: 4px; font-size: 13px;"
        " background: transparent; }"
        "QToolButton#tabButton:checked { color: @accent@; background: @selection@;"
        " border-top: 2px solid @accent@; font-weight: bold; }"
        "QToolButton#tabButton:focus { color: @text@; }"
        "QSlider#chatTextSize::groove:horizontal { height: 5px; margin: 8px 9px;"
        " background: @border@; border-radius: 2px; }"
        "QSlider#chatTextSize::sub-page:horizontal { background: @accent@; border-radius: 2px; }"
        "QSlider#chatTextSize::handle:horizontal { width: 24px; margin: -8px -5px;"
        " background: @accent@; border: 2px solid @canvas@; border-radius: 10px; }");
    const struct { const char* marker; QColor color; } replacements[] = {
        {"@canvas@", colors.canvas}, {"@chrome@", colors.chrome}, {"@surface@", colors.surface},
        {"@border@", colors.border}, {"@text@", colors.text}, {"@muted@", colors.muted},
        {"@accent@", colors.accent}, {"@onAccent@", colors.onAccent}, {"@selection@", colors.selection},
        {"@danger@", colors.danger},
        {"@nodeCard@", Appearance::mix(colors.surface, colors.accent, 7)},
        {"@group@", Appearance::mix(colors.canvas, colors.surface, 79)},
        {"@pressed@", Appearance::mix(colors.selection, colors.accent, 20)},
        {"@scrollHandle@", Appearance::mix(colors.border, colors.accent, 28)}
    };
    for (const auto& replacement : replacements)
        style.replace(QLatin1String(replacement.marker), replacement.color.name());
    return style;
}

}  // namespace

QScrollArea* makeTouchScrollArea(QWidget* parent, QWidget* content) {
    auto* scroll = new TouchScrollArea(parent);
    scroll->setContent(content);
    return scroll;
}

void ClientWindow::applyAppearance(int theme, int chatTextSize) {
    const Appearance::Theme selected = Appearance::checkedTheme(theme);
    const int size = Appearance::checkedChatTextSize(chatTextSize);
    const Appearance::Palette colors = Appearance::palette(selected);
    const bool changed = selected != appearanceTheme_ || colors.dark != appearanceDark_;
    const bool sizeChanged = size != chatTextSize_;
    appearanceTheme_ = selected;
    appearanceDark_ = colors.dark;
    chatTextSize_ = size;
    if (changed) {
        setProperty("ommeshTheme", appearanceTheme_);
        QPalette appColors = QApplication::palette();
        appColors.setColor(QPalette::Window, colors.canvas);
        appColors.setColor(QPalette::WindowText, colors.text);
        appColors.setColor(QPalette::Base, colors.surface);
        appColors.setColor(QPalette::AlternateBase, colors.chrome);
        appColors.setColor(QPalette::Text, colors.text);
        appColors.setColor(QPalette::Button, colors.surface);
        appColors.setColor(QPalette::ButtonText, colors.text);
        appColors.setColor(QPalette::Highlight, colors.accent);
        appColors.setColor(QPalette::HighlightedText, colors.onAccent);
        appColors.setColor(QPalette::Link, colors.accent);
        setPalette(appColors);
        setStyleSheet(themeStyle(colors));
        if (settingsNodeAvatar_)
            settingsNodeAvatar_->setPixmap(radioAvatar(colors));
        if (settingsAnnounce_)
            settingsAnnounce_->setIcon(SettingsIcons::icon(SettingsIcons::Frequency, colors.onAccent, 18));
        if (settingsSearchButton_)
            settingsSearchButton_->setIcon(SettingsIcons::icon(SettingsIcons::Search, colors.muted));
        if (connectionBack_)
            connectionBack_->setIcon(SettingsIcons::icon(SettingsIcons::Back, colors.muted));
        if (detailBack_)
            detailBack_->setIcon(SettingsIcons::icon(SettingsIcons::Back, colors.muted));
        if (tabs_) {
            for (int index = Chats; index <= Settings; ++index)
                tabs_->button(index)->setIcon(tabIcon(colors, index, index == Chats ? unreadBadge_ : 0,
                                                     index == tab_));
        }
    }
    if ((changed || sizeChanged) && chatPage_)
        chatPage_->setAppearance(colors, chatTextSize_);
}

ClientWindow::ClientWindow(Client& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    setWindowTitle(ui("OMMesh Belle"));
    setObjectName(QLatin1String("window"));
    resize(360, 640);
    const Appearance::Preferences appearance = Appearance::preferences();
    applyAppearance(appearance.theme, appearance.chatTextSize);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    pages_ = new QStackedWidget(this);
    dashboard_ = makeDashboard();
    connectionPage_ = makeConnectionPage();
    pages_->addWidget(dashboard_);
    pages_->addWidget(connectionPage_);
    layout->addWidget(pages_, 1);
    layout->addWidget(makeTabs());

    QSettings preferences;
    host_->setText(preferences.value(QLatin1String("connection/host")).toString());
    port_->setText(preferences.value(QLatin1String("connection/port"), 5000).toString());
    pins_ = preferences.value(QLatin1String("chats/pins")).toMap();

    connect(&client_, SIGNAL(changed()), this, SLOT(scheduleViewUpdate()));
    updateView();
}

QWidget* ClientWindow::makeDashboard() {
    QWidget* dashboard = surface(pages_, "dashboard");
    QVBoxLayout* layout = new QVBoxLayout(dashboard);
    layout->setContentsMargins(0, 0, 0, 0);
    tabPages_ = new QStackedWidget(dashboard);
    tabPages_->addWidget(makeChatsPage());
    tabPages_->addWidget(makeNetworkPage());
    tabPages_->addWidget(makeSettingsPage());
    layout->addWidget(tabPages_);
    return dashboard;
}

QWidget* ClientWindow::makeTabs() {
    QWidget* strip = surface(this, "tabs");
    QHBoxLayout* layout = new QHBoxLayout(strip);
    layout->setContentsMargins(4, 0, 4, 2);
    layout->setSpacing(2);
    const char* names[] = {"Чаты", "Сеть", "Настройки"};
    const Appearance::Palette colors = Appearance::paletteFor(this);
    tabs_ = new QButtonGroup(strip);
    for (int index = Chats; index <= Settings; ++index) {
        QToolButton* tab = new QToolButton(strip);
        tab->setObjectName(QLatin1String("tabButton"));
        tab->setText(ui(names[index]));
        tab->setProperty("accessibleName", tab->text());
        tab->setIcon(tabIcon(colors, index));
        tab->setIconSize(QSize(42, 26));
        tab->setMinimumHeight(58);
        tab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        tab->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        tab->setCheckable(true);
        tab->setFocusPolicy(Qt::StrongFocus);
        tab->setChecked(index == Chats);
        tabs_->addButton(tab, index);
        layout->addWidget(tab, 1);
        if (index == Chats)
            chatsTab_ = tab;
    }
    connect(tabs_, SIGNAL(buttonClicked(int)), this, SLOT(selectTab(int)));
    return strip;
}

QWidget* ClientWindow::makeChatsPage() {
    QWidget* page = surface(tabPages_, "pane");
    QVBoxLayout* layout = pageLayout(page);
    layout->setContentsMargins(14, 12, 14, 0);
    QHBoxLayout* heading = new QHBoxLayout;
    heading->addWidget(label(ui("Чаты"), page, "heading"));
    QVBoxLayout* radioSummary = new QVBoxLayout;
    radioSummary->setSpacing(1);
    radio_ = label(QString(), page, "radioName");
    radio_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    radio_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    radioState_ = label(QString(), page, "radioState");
    radioState_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    radioState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    radioSummary->addWidget(radio_);
    radioSummary->addWidget(radioState_);
    heading->addLayout(radioSummary, 1);
    QPushButton* add = button(ui("+"), page);
    add->setObjectName(QLatin1String("newChat"));
    add->setFixedWidth(48);
    add->setProperty("accessibleName", ui("Новый чат"));
    heading->addWidget(add);
    layout->addLayout(heading);

    chatSearch_ = searchBox(ui("Найти чат"), page);
    QHBoxLayout* searchControls = new QHBoxLayout;
    searchControls->addWidget(chatSearch_, 1);
    clearChatSearch_ = button(ui("×"), page);
    clearChatSearch_->setObjectName(QLatin1String("clearChatSearch"));
    clearChatSearch_->setFixedWidth(42);
    clearChatSearch_->setProperty("accessibleName", ui("Очистить поиск"));
    clearChatSearch_->setEnabled(false);
    searchControls->addWidget(clearChatSearch_);
    layout->addLayout(searchControls);
    QHBoxLayout* controls = new QHBoxLayout;
    chatCount_ = label(QString(), page, "muted");
    chatCount_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controls->addWidget(chatCount_, 1);
    chatFilter_ = button(ui(chatFilters[chatFilterIndex_]), page);
    chatFilter_->setObjectName(QLatin1String("chatFilter"));
    chatFilter_->setProperty("accessibleName", ui("Фильтр чатов"));
    chatFilter_->setMinimumHeight(38);
    controls->addWidget(chatFilter_);
    layout->addLayout(controls);
    emptyChats_ = label(QString(), page, "muted");
    emptyChats_->setWordWrap(true);
    emptyChats_->setAlignment(Qt::AlignCenter);
    layout->addWidget(emptyChats_);
    targets_ = makeList(page, ui("Чаты"));
    targets_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(targets_, 1);
    connectFromChats_ = button(ui("Подключить рацию"), page, true);
    layout->addWidget(connectFromChats_);
    layout->addSpacing(4);
    connect(add, SIGNAL(clicked()), this, SLOT(openNewChat()));
    connect(connectFromChats_, SIGNAL(clicked()), this, SLOT(openConnection()));
    connect(chatSearch_, SIGNAL(textChanged(QString)), this, SLOT(filterChats()));
    connect(clearChatSearch_, SIGNAL(clicked()), chatSearch_, SLOT(clear()));
    connect(chatFilter_, SIGNAL(clicked()), this, SLOT(chooseChatFilter()), Qt::QueuedConnection);
    connect(targets_, SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(openTarget(QListWidgetItem*)));
    connect(targets_, SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(openTarget(QListWidgetItem*)));
    connect(targets_, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(showChatOptions(QPoint)));
    return page;
}

QWidget* ClientWindow::makeNetworkPage() {
    QWidget* page = surface(tabPages_, "pane");
    QVBoxLayout* layout = pageLayout(page);
    layout->setContentsMargins(14, 12, 14, 0);
    QHBoxLayout* heading = new QHBoxLayout;
    heading->addWidget(label(ui("Сеть"), page, "heading"), 1);
    mapButton_ = button(ui("Карта"), page);
    heading->addWidget(mapButton_);
    layout->addLayout(heading);
    nodeSearch_ = searchBox(ui("Найти узел"), page);
    layout->addWidget(nodeSearch_);

    networkViews_ = new QStackedWidget(page);
    QWidget* listPage = surface(networkViews_, "pane");
    QVBoxLayout* listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(8);
    nodeCount_ = label(QString(), listPage, "muted");
    listLayout->addWidget(nodeCount_);
    emptyNodes_ = label(QString(), listPage, "muted");
    emptyNodes_->setWordWrap(true);
    emptyNodes_->setAlignment(Qt::AlignCenter);
    listLayout->addWidget(emptyNodes_);
    nodes_ = makeList(listPage, ui("Узлы"));
    listLayout->addWidget(nodes_, 1);
    networkViews_->addWidget(listPage);

    map_ = new NodeMap(networkViews_);
    map_->setTrace([this](const char* step, int code) { client_.traceConnection(step, code); });
    networkViews_->addWidget(map_);
    layout->addWidget(networkViews_, 1);

    connect(mapButton_, SIGNAL(clicked()), this, SLOT(showMap()));
    connect(nodeSearch_, SIGNAL(textChanged(QString)), this, SLOT(filterNodes()));
    connect(nodes_, SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(openNode(QListWidgetItem*)));
    connect(nodes_, SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(openNode(QListWidgetItem*)));
    connect(map_, SIGNAL(nodeSelected(QString)), this, SLOT(openNode(QString)));
    connect(map_, SIGNAL(settingsRequested()), this, SLOT(openMapSettings()));
    return page;
}

QWidget* ClientWindow::makeSettingsPage() {
    QWidget* page = surface(tabPages_, "pane");
    QVBoxLayout* layout = pageLayout(page);
    layout->setContentsMargins(12, 8, 12, 0);
    settingsReconnectBanner_ = surface(page, "settingsReconnectBanner");
    auto* bannerLayout = new QHBoxLayout(settingsReconnectBanner_);
    bannerLayout->setContentsMargins(9, 5, 6, 5);
    bannerLayout->setSpacing(6);
    settingsReconnectText_ = label(QString(), settingsReconnectBanner_, "settingsReconnectText");
    settingsReconnectText_->setWordWrap(true);
    settingsReconnectText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bannerLayout->addWidget(settingsReconnectText_, 1);
    settingsRetryNow_ = button(ui("Попробовать сейчас"), settingsReconnectBanner_);
    settingsRetryNow_->setObjectName(QLatin1String("settingsRetryNow"));
    bannerLayout->addWidget(settingsRetryNow_);
    layout->addWidget(settingsReconnectBanner_);
    settingsReconnectBanner_->hide();
    auto* heading = new QHBoxLayout;
    QLabel* title = label(ui("Настройки"), page, "heading");
    title->setContentsMargins(3, 0, 0, 0);
    heading->addWidget(title, 1);
    settingsSearchButton_ = new QToolButton(page);
    settingsSearchButton_->setObjectName(QLatin1String("settingsSearchButton"));
    settingsSearchButton_->setProperty("accessibleName", ui("Поиск настроек"));
    settingsSearchButton_->setCheckable(true);
    settingsSearchButton_->setFixedSize(42, 40);
    settingsSearchButton_->setIconSize(QSize(22, 22));
    settingsSearchButton_->setIcon(SettingsIcons::icon(SettingsIcons::Search, Appearance::paletteFor(this).muted));
    heading->addWidget(settingsSearchButton_);
    layout->addLayout(heading);
    settingsSearch_ = searchBox(ui("Найти настройку"), page);
    settingsSearch_->setObjectName(QLatin1String("settingsSearch"));
    layout->addWidget(settingsSearch_);
    settingsSearch_->hide();
    TouchScrollArea* scroll = new TouchScrollArea(page);
    settingsScroll_ = scroll;
    QWidget* content = surface(scroll, "pane");
    QVBoxLayout* settings = new QVBoxLayout(content);
    settings->setContentsMargins(0, 0, 2, 15);
    settings->setSpacing(16);

    QWidget* card = surface(content, "nodeCard");
    settingsCard_ = card;
    QVBoxLayout* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(8);
    QHBoxLayout* identification = new QHBoxLayout;
    identification->setSpacing(10);
    settingsNodeAvatar_ = label(QString(), card);
    settingsNodeAvatar_->setPixmap(radioAvatar(Appearance::paletteFor(this)));
    identification->addWidget(settingsNodeAvatar_, 0, Qt::AlignVCenter);
    QVBoxLayout* identityText = new QVBoxLayout;
    identityText->setSpacing(1);
    settingsName_ = label(QString(), card, "settingsNodeName");
    settingsName_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identityText->addWidget(settingsName_);
    settingsStatus_ = label(QString(), card, "settingsNodeStatus");
    settingsStatus_->setWordWrap(true);
    settingsStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identityText->addWidget(settingsStatus_);
    identification->addLayout(identityText, 1);
    identification->setAlignment(identityText, Qt::AlignVCenter);
    auto* battery = new QVBoxLayout;
    battery->setSpacing(1);
    settingsBatteryPercent_ = label(QString(), card, "settingsBatteryPercent");
    settingsBatteryPercent_->setMinimumWidth(56);
    settingsBatteryPercent_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    battery->addWidget(settingsBatteryPercent_);
    settingsBatteryVoltage_ = label(QString(), card, "settingsBatteryVoltage");
    settingsBatteryVoltage_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    battery->addWidget(settingsBatteryVoltage_);
    identification->addLayout(battery);
    identification->setAlignment(battery, Qt::AlignVCenter);
    cardLayout->addLayout(identification);
    QHBoxLayout* cardActions = new QHBoxLayout;
    settingsAnnounce_ = button(ui("Анонсировать"), card, true);
    settingsAnnounce_->setObjectName(QLatin1String("radioAdvertise"));
    settingsAnnounce_->setIcon(SettingsIcons::icon(SettingsIcons::Frequency, Appearance::paletteFor(this).onAccent, 18));
    cardActions->addWidget(settingsAnnounce_, 1);
    settingsLink_ = button(ui("Подключить"), card);
    settingsLink_->setObjectName(QLatin1String("settingsLink"));
    cardActions->addWidget(settingsLink_, 1);
    cardLayout->addLayout(cardActions);
    settings->addWidget(card);

    radioSettings_ = new RadioSettingsPanel(client_, content);
    radioSettings_->setObjectName(QLatin1String("settingsTools"));
    settings->addWidget(radioSettings_);
    emptySettings_ = label(ui("Поиск не нашёл настроек."), content, "muted");
    emptySettings_->setAlignment(Qt::AlignCenter);
    emptySettings_->setWordWrap(true);
    settings->addWidget(emptySettings_);
    emptySettings_->hide();
    settings->addStretch(1);
    scroll->setContent(content);
    layout->addWidget(scroll, 1);

    connect(settingsLink_, SIGNAL(clicked()), this, SLOT(manageSettingsConnection()));
    connect(settingsRetryNow_, SIGNAL(clicked()), this, SLOT(retrySettingsConnection()), Qt::QueuedConnection);
    connect(settingsAnnounce_, SIGNAL(clicked()), radioSettings_, SLOT(advertise()), Qt::QueuedConnection);
    connect(radioSettings_, SIGNAL(connectionSettingsRequested()), this, SLOT(openConnection()));
    connect(radioSettings_, SIGNAL(exitRequested()), this, SLOT(exitApplication()));
    connect(settingsSearchButton_, SIGNAL(toggled(bool)), this, SLOT(toggleSettingsSearch(bool)));
    connect(settingsSearch_, SIGNAL(textChanged(QString)), this, SLOT(filterSettings(QString)));
    connect(radioSettings_, SIGNAL(detailRequested(QWidget*,QString)),
            this, SLOT(showDetailPage(QWidget*,QString)));
    connect(radioSettings_, SIGNAL(mapSettingsRequested()), this, SLOT(openMapSettings()));
    connect(radioSettings_, SIGNAL(appearanceChanged(int,int)), this, SLOT(applyAppearance(int,int)));
    return page;
}

QWidget* ClientWindow::makeConnectionPage() {
    QWidget* page = surface(pages_, "pane");
    page->setObjectName(QLatin1String("connectionPage"));
    QVBoxLayout* layout = pageLayout(page);
    QHBoxLayout* heading = new QHBoxLayout;
    connectionBack_ = button(QString(), page);
    connectionBack_->setObjectName(QLatin1String("connectionBack"));
    connectionBack_->setProperty("accessibleName", ui("Назад"));
    connectionBack_->setFixedWidth(36);
    connectionBack_->setIcon(SettingsIcons::icon(SettingsIcons::Back, Appearance::paletteFor(this).muted));
    heading->addWidget(connectionBack_);
    QLabel* title = label(ui("Подключение"), page, "subtitle");
    heading->addWidget(title, 1);
    layout->addLayout(heading);
    QWidget* content = surface(page, "pane");
    auto* fields = new QVBoxLayout(content);
    fields->setContentsMargins(0, 0, 2, 0);
    fields->setSpacing(8);
    status_ = label(QString(), content, "muted");
    status_->setWordWrap(true);
    fields->addWidget(status_);
    QLabel* help = label(ui("Подключите телефон к сети Wi‑Fi ноды MeshCore или моста."), content, "muted");
    help->setWordWrap(true);
    fields->addWidget(help);

    QLabel* hostLabel = label(ui("IP-адрес ноды или моста"), content);
    host_ = new QLineEdit(content);
    host_->setMinimumHeight(44);
    host_->setMaxLength(253);
    host_->setPlaceholderText(QLatin1String("192.168.1.50"));
    host_->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    host_->setProperty("accessibleName", hostLabel->text());
    hostLabel->setBuddy(host_);
    fields->addWidget(hostLabel);
    fields->addWidget(host_);

    QLabel* portLabel = label(ui("TCP-порт"), content);
    port_ = new QLineEdit(content);
    port_->setMinimumHeight(44);
    port_->setMaxLength(5);
    port_->setInputMethodHints(Qt::ImhDigitsOnly);
    port_->setValidator(new QIntValidator(1, 65535, port_));
    port_->setProperty("accessibleName", portLabel->text());
    portLabel->setBuddy(port_);
    fields->addWidget(portLabel);
    fields->addWidget(port_);

    error_ = label(QString(), content, "error");
    error_->setWordWrap(true);
    error_->hide();
    fields->addWidget(error_);
    connectButton_ = button(ui("Подключить"), content, true);
    fields->addWidget(connectButton_);
    fields->addSpacing(8);
    fields->addWidget(radioSettings_->makeConnectionPreferences(content));
    fields->addStretch(1);
    layout->addWidget(makeTouchScrollArea(page, content), 1);
    connect(connectionBack_, SIGNAL(clicked()), this, SLOT(returnToDashboard()));
    connect(connectButton_, SIGNAL(clicked()), this, SLOT(scheduleConnection()));
    connect(port_, SIGNAL(returnPressed()), this, SLOT(scheduleConnection()));
    return page;
}

void ClientWindow::selectTab(int tab) {
    if (tab < Chats || tab > Settings)
        return;
    pendingTab_ = tab;
    queueNavigation(Navigation::Tab);
}

void ClientWindow::returnToDashboard() {
    queueNavigation(Navigation::Dashboard);
}

void ClientWindow::showChats() {
    selectTab(Chats);
}

void ClientWindow::exitApplication() {
    queueNavigation(Navigation::Exit);
}

void ClientWindow::openConnection() {
    queueNavigation(Navigation::Connection);
}

void ClientWindow::showDetailPage(QWidget* page, const QString& title) {
    if (!page)
        return;
    if (!page->parent())
        page->setParent(this);
    if (pendingDetail_ && pendingDetail_.data() != page && pendingDetail_.data() != detailContent_.data())
        pendingDetail_->deleteLater();
    pendingDetail_ = page;
    pendingDetailTitle_ = title;
    queueNavigation(Navigation::Detail);
}

void ClientWindow::queueNavigation(Navigation action) {
    if (action != Navigation::Detail && pendingDetail_) {
        if (pendingDetail_.data() != detailContent_.data())
            pendingDetail_->deleteLater();
        pendingDetail_ = nullptr;
    }
    if (action != Navigation::Connection && connectionRequest_ == ConnectionRequest::Connect)
        connectionRequest_ = ConnectionRequest::None;
    pendingTargetId_.clear();
    const bool scheduled = pendingNavigation_ != Navigation::None;
    pendingNavigation_ = action;
    if (!scheduled)
        QTimer::singleShot(0, this, SLOT(applyNavigation()));
}

void ClientWindow::focusNavigation(int tab) {
    QAbstractButton* anchor = tabs_->button(tab);
    if (!anchor->hasFocus())
        anchor->setFocus(Qt::OtherFocusReason);
}

void ClientWindow::applyNavigation() {
    const Navigation action = pendingNavigation_;
    pendingNavigation_ = Navigation::None;
    if (action == Navigation::None)
        return;
    if (action == Navigation::Map && (tab_ != Network || pages_->currentWidget() != dashboard_))
        return;
    if (action == Navigation::Detail && !pendingDetail_)
        return;

    client_.traceConnection("ui navigation begin", static_cast<int>(action));
    // Keep focus outside both stacks before they hide the old page.
    focusNavigation(action == Navigation::Tab ? pendingTab_ : tab_);
    client_.traceConnection("ui navigation focus settled");
    if (action == Navigation::Exit) {
        close();
        return;
    }
    if (action == Navigation::MapSettings) {
        MapSettingsPanel* settings = new MapSettingsPanel(this);
        connect(settings, SIGNAL(saved()), this, SLOT(mapSettingsSaved()));
        showDetailPage(settings, ui("Интернет-карта"));
        return;
    }
    if (action == Navigation::NewChat) {
        makeChatChooser();
        return;
    }

    const bool leavingDetail = pages_->currentWidget() == detailPage_ && action != Navigation::Detail;
    if (pages_->currentWidget() == chatPage_)
        saveDraft();
    if (action == Navigation::Tab) {
        tab_ = pendingTab_;
        if (tab_ == Network && mapVisible_)
            configureNodeMap(map_);
        tabs_->button(tab_)->setChecked(true);
        tabs_->button(Settings)->setIcon(tabIcon(Appearance::paletteFor(this), Settings, 0, tab_ == Settings));
        tabPages_->setCurrentIndex(tab_);
    } else if (action == Navigation::Map) {
        mapVisible_ = !mapVisible_;
        if (mapVisible_)
            configureNodeMap(map_);
        networkViews_->setCurrentIndex(mapVisible_ ? 1 : 0);
        mapButton_->setText(mapVisible_ ? ui("Список") : ui("Карта"));
    } else if (action == Navigation::MapSaved) {
        configureNodeMap(map_);
        radioSettings_->updateStatus();
    }

    if (action == Navigation::Detail)
        showPendingDetail();
    else
        pages_->setCurrentWidget(action == Navigation::Connection ? connectionPage_ : dashboard_);
    if (leavingDetail)
        clearDetail();
    client_.traceConnection("ui navigation page shown");
    updateView();
    client_.traceConnection("ui navigation end");
}

void ClientWindow::showPendingDetail() {
    QWidget* content = pendingDetail_.data();
    pendingDetail_ = nullptr;
    if (!content)
        return;
    if (!detailPage_) {
        detailPage_ = surface(pages_, "detailPage");
        detailLayout_ = pageLayout(detailPage_);
        QHBoxLayout* heading = new QHBoxLayout;
        detailBack_ = button(QString(), detailPage_);
        detailBack_->setObjectName(QLatin1String("detailBack"));
        detailBack_->setProperty("accessibleName", ui("Назад"));
        detailBack_->setFixedWidth(36);
        detailBack_->setIcon(SettingsIcons::icon(SettingsIcons::Back, Appearance::paletteFor(this).muted));
        detailTitle_ = label(QString(), detailPage_, "subtitle");
        detailTitle_->setWordWrap(true);
        detailTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        heading->addWidget(detailBack_);
        heading->addWidget(detailTitle_, 1);
        detailLayout_->addLayout(heading);
        pages_->addWidget(detailPage_);
        connect(detailBack_, SIGNAL(clicked()), this, SLOT(returnToDashboard()));
    }
    if (detailContent_.data() != content) {
        clearDetail();
        detailContent_ = content;
        detailLayout_->addWidget(content, 1);
        content->show();
    }
    detailTitle_->setText(pendingDetailTitle_);
    pages_->setCurrentWidget(detailPage_);
}

void ClientWindow::clearDetail() {
    if (detailContent_) {
        detailLayout_->removeWidget(detailContent_.data());
        detailContent_->hide();
        detailContent_->deleteLater();
        detailContent_ = nullptr;
    }
}

void ClientWindow::openNewChat() {
    queueNavigation(Navigation::NewChat);
}

void ClientWindow::makeChatChooser() {
    const QVector<ChatTarget> targets = client_.targets();
    if (targets.isEmpty() && !client_.connected()) {
        openConnection();
        return;
    }

    QWidget* panel = surface(this, "newChatView");
    QVBoxLayout* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    chooserSearch_ = searchBox(ui("Найти контакт или канал"), panel);
    chooserSearch_->setObjectName(QLatin1String("newChatSearch"));
    layout->addWidget(chooserSearch_.data());

    const char* captions[] = {"Все", "Личные", "Каналы"};
    const char* objectNames[] = {"newChatAll", "newChatDirect", "newChatChannels"};
    QButtonGroup* kinds = new QButtonGroup(panel);
    QHBoxLayout* filters = new QHBoxLayout;
    for (int n = 0; n < 3; ++n) {
        QPushButton* filter = button(ui(captions[n]), panel);
        filter->setObjectName(QLatin1String(objectNames[n]));
        filter->setCheckable(true);
        filter->setChecked(n == 0);
        kinds->addButton(filter, n);
        filters->addWidget(filter, 1);
    }
    layout->addLayout(filters);
    chooserEmpty_ = label(QString(), panel, "muted");
    chooserEmpty_->setWordWrap(true);
    chooserEmpty_->setAlignment(Qt::AlignCenter);
    layout->addWidget(chooserEmpty_.data());
    chooser_ = makeList(panel, ui("Контакты и каналы"));
    chooser_->setObjectName(QLatin1String("newChatTargets"));
    layout->addWidget(chooser_.data(), 1);
    chooserChannels_ = button(ui("Добавить канал"), panel);
    chooserChannels_->setObjectName(QLatin1String("newChatRadioChannels"));
    layout->addWidget(chooserChannels_.data());

    chooserKind_ = 0;
    updateChatChooser(targets);
    connect(chooserSearch_.data(), SIGNAL(textChanged(QString)), this, SLOT(filterChooser(QString)));
    connect(kinds, SIGNAL(buttonClicked(int)), this, SLOT(selectChooserKind(int)));
    connect(chooser_.data(), SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(openTarget(QListWidgetItem*)));
    connect(chooser_.data(), SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(openTarget(QListWidgetItem*)));
    connect(chooserChannels_.data(), SIGNAL(clicked()), this, SLOT(openChatChannels()));
    showDetailPage(panel, ui("Новый чат"));
}

void ClientWindow::filterChooser(const QString&) {
    scheduleViewUpdate();
    if (chooser_)
        chooser_->verticalScrollBar()->setValue(0);
}

void ClientWindow::selectChooserKind(int kind) {
    chooserKind_ = kind;
    filterChooser(QString());
}

void ClientWindow::openChatChannels() {
    if (client_.connected())
        QMetaObject::invokeMethod(radioSettings_, "addChannel", Qt::QueuedConnection);
}

void ClientWindow::updateChatChooser(const QVector<ChatTarget>& targets) {
    const QString query = chooserSearch_->text().trimmed();
    QVector<ListRow> rows;
    const QSet<QString> rooms = roomIds(client_);
    for (const ChatTarget& target : targets) {
        if ((chooserKind_ == 1 && target.channel) || (chooserKind_ == 2 && !target.channel))
            continue;
        if (!query.isEmpty() && !target.title.contains(query, Qt::CaseInsensitive) &&
            !target.id.contains(query, Qt::CaseInsensitive))
            continue;
        const bool room = rooms.contains(target.id);
        const QString detail = target.channel ? ui("Канал") : room ? ui("Комната") : ui("Личный чат");
        rows.append(ListRow{target.id, target.title, detail, QString(), target.unread,
                            target.channel, isPinned(target), room ? ChatRoomRole : 0});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const ListRow& left, const ListRow& right) {
        if (left.channel != right.channel)
            return left.channel;
        if (left.pinned != right.pinned)
            return left.pinned;
        return left.title.compare(right.title, Qt::CaseInsensitive) < 0;
    });
    displayRows(chooser_.data(), rows);
    chooserEmpty_->setText(query.isEmpty() ? ui("Здесь появятся известные станции и каналы.") :
                                           ui("Поиск не дал результатов."));
    chooserEmpty_->setVisible(rows.isEmpty());
    chooserChannels_->setEnabled(client_.connected());
}

void ClientWindow::filterChats() {
    scheduleViewUpdate();
    targets_->verticalScrollBar()->setValue(0);
}

void ClientWindow::chooseChatFilter() {
    if (!chatFilter_->isVisible())
        return;
    QMenu* menu = new QMenu(chatFilter_);
    menu->setObjectName(QLatin1String("chatFilterMenu"));
    for (int index = 0; index < static_cast<int>(sizeof(chatFilters) / sizeof(*chatFilters)); ++index) {
        QAction* action = menu->addAction(ui(chatFilters[index]));
        action->setCheckable(true);
        action->setChecked(index == chatFilterIndex_);
        action->setData(index);
    }
    connect(menu, SIGNAL(triggered(QAction*)), this, SLOT(applyChatFilter(QAction*)));
    connect(menu, SIGNAL(aboutToHide()), menu, SLOT(deleteLater()));
    menu->popup(chatFilter_->mapToGlobal(QPoint(0, chatFilter_->height())));
}

void ClientWindow::applyChatFilter(QAction* choice) {
    chatFilterIndex_ = choice->data().toInt();
    chatFilter_->setText(choice->text());
    filterChats();
}

void ClientWindow::filterNodes() {
    updateView();
    nodes_->verticalScrollBar()->setValue(0);
}

void ClientWindow::filterSettings(const QString& text) {
    const QString query = text.trimmed();
    const bool identity = ui("MeshCore Нода Подключить Отключить Анонсировать").contains(query, Qt::CaseInsensitive) ||
                          settingsName_->text().contains(query, Qt::CaseInsensitive);
    settingsCard_->setVisible(identity);
    const bool tools = radioSettings_->filter(query);
    radioSettings_->setVisible(tools);
    emptySettings_->setVisible(!identity && !tools);
    settingsScroll_->verticalScrollBar()->setValue(0);
}

void ClientWindow::toggleSettingsSearch(bool open) {
    if (open) {
        settingsSearch_->show();
        settingsSearch_->setFocus(Qt::OtherFocusReason);
    } else {
        settingsSearchButton_->setFocus(Qt::OtherFocusReason);
        settingsSearch_->clear();
        settingsSearch_->hide();
    }
}

void ClientWindow::openTarget(QListWidgetItem* item) {
    if (item && pendingNavigation_ == Navigation::None) {
        // The read count update may rebuild the list. Wait for its event handler.
        pendingTargetId_ = item->data(ItemId).toString();
        QTimer::singleShot(0, this, SLOT(openPendingTarget()));
    }
}

void ClientWindow::openPendingTarget() {
    const QString id = pendingTargetId_;
    pendingTargetId_.clear();
    if (id.isEmpty() || (pages_->currentWidget() == chatPage_ && id == activeId_))
        return;
    const QVector<ChatTarget> targets = client_.targets();
    for (const ChatTarget& target : targets) {
        if (target.id != id)
            continue;
        const bool leavingDetail = pages_->currentWidget() == detailPage_;
        client_.traceConnection("ui chat open begin");
        focusNavigation(tab_);
        if (!chatPage_) {
            client_.traceConnection("ui chat creation begin");
            chatPage_ = new ChatView(pages_);
            chatPage_->setAppearance(Appearance::paletteFor(this), chatTextSize_);
            pages_->addWidget(chatPage_);
            connect(chatPage_, SIGNAL(backRequested()), this, SLOT(showChats()));
            connect(chatPage_, SIGNAL(sendRequested(QString)), this, SLOT(sendMessage(QString)));
            connect(chatPage_, SIGNAL(resendRequested(QString)), this, SLOT(resendMessage(QString)));
            connect(chatPage_, SIGNAL(reactionRequested(ChatMessage,QString)),
                    this, SLOT(reactToMessage(ChatMessage,QString)));
            connect(chatPage_, SIGNAL(senderBlockRequested(QString,QString,bool)),
                    this, SLOT(setSenderBlocked(QString,QString,bool)));
            connect(chatPage_, SIGNAL(pinRequested()), this, SLOT(toggleActivePin()));
            client_.traceConnection("ui chat creation end");
        }
        if (pages_->currentWidget() == chatPage_)
            saveDraft();
        activeId_ = id;
        activeTitle_ = target.title;
        client_.traceConnection("ui chat context begin");
        chatPage_->setContext(activeTitle_, client_.status(), activeId_);
        chatPage_->setDraft(drafts_.value(activeId_));
        client_.traceConnection("ui chat context end");
        pages_->setCurrentWidget(chatPage_);
        if (leavingDetail)
            clearDetail();
        client_.traceConnection("ui chat page shown");
        chatPage_->syncLinkPreviewMode();
        chatPage_->setMessages(client_.messages(activeId_), true);
        client_.markRead(activeId_);
        updateView();
        client_.traceConnection("ui chat open end");
        return;
    }
}

void ClientWindow::openNode(QListWidgetItem* item) {
    if (item)
        openNode(item->data(ItemId).toString());
}

void ClientWindow::openNode(const QString& id) {
    showNodeTools(client_, id, this);
}

void ClientWindow::showMap() {
    queueNavigation(Navigation::Map);
}

void ClientWindow::openMapSettings() {
    queueNavigation(Navigation::MapSettings);
}

void ClientWindow::mapSettingsSaved() {
    if (sender() == detailContent_.data())
        queueNavigation(Navigation::MapSaved);
}

void ClientWindow::showChatOptions(const QPoint& position) {
    QListWidgetItem* item = targets_->itemAt(position);
    if (!item)
        return;
    const QString id = item->data(ItemId).toString();
    QMenu actions(this);
    QAction* pin = actions.addAction(item->data(ItemPinned).toBool() ?
                                     ui("Открепить чат") : ui("Закрепить чат"));
    if (actions.exec(targets_->viewport()->mapToGlobal(position)) == pin)
        togglePin(id);
}

void ClientWindow::toggleActivePin() {
    togglePin(activeId_);
}

bool ClientWindow::isPinned(const ChatTarget& target) const {
    const bool standardPublic = target.channel &&
        target.title.compare(QLatin1String("Public"), Qt::CaseInsensitive) == 0;
    return pins_.value(target.id, standardPublic).toBool();
}

void ClientWindow::togglePin(const QString& id) {
    const QVector<ChatTarget> targets = client_.targets();
    for (const ChatTarget& target : targets) {
        if (target.id != id)
            continue;
        pins_.insert(id, !isPinned(target));
        QSettings preferences;
        preferences.setValue(QLatin1String("chats/pins"), pins_);
        updateView();
        return;
    }
}

void ClientWindow::manageSettingsConnection() {
    if (client_.connected() || client_.connecting() || demoRadioActive(client_))
        scheduleConnection();
    else
        openConnection();
}

void ClientWindow::retrySettingsConnection() {
    if (connectionRequest_ == ConnectionRequest::None && client_.canRetryConnectionNow())
        client_.retryConnectionNow();
}

void ClientWindow::scheduleConnection() {
    if (connectionRequest_ != ConnectionRequest::None)
        return;
    connectionRequest_ = demoRadioActive(client_) || client_.connecting() || client_.connected()
        ? ConnectionRequest::Disconnect : ConnectionRequest::Connect;
    client_.traceConnection("ui submit queued");
    // The Symbian editor may still be committing input in the signal handler.
    QTimer::singleShot(0, this, SLOT(toggleConnection()));
}

void ClientWindow::toggleConnection() {
    const ConnectionRequest request = connectionRequest_;
    connectionRequest_ = ConnectionRequest::None;
    if (request == ConnectionRequest::None)
        return;
    if (request == ConnectionRequest::Disconnect) {
        client_.traceConnection("ui disconnect");
        error_->hide();
        if (demoRadioActive(client_))
            stopDemoRadio(client_);
        else
            client_.disconnectFromRadio();
        return;
    }
    if (pages_->currentWidget() != connectionPage_ || demoRadioActive(client_) ||
        client_.connecting() || client_.connected())
        return;
    client_.traceConnection("ui submit begin");
    error_->hide();

    const QString host = host_->text().trimmed();
    bool validPort = false;
    const uint port = port_->text().toUInt(&validPort);
    if (host.isEmpty()) {
        client_.traceConnection("ui missing address");
        error_->setText(ui("Введите IP-адрес рации или моста."));
        error_->show();
        host_->setFocus();
        return;
    }
    if (!validPort || port < 1 || port > 65535) {
        client_.traceConnection("ui invalid port");
        error_->setText(ui("Введите порт от 1 до 65535."));
        error_->show();
        port_->setFocus();
        return;
    }

    client_.traceConnection("ui save preferences begin");
    {
        QSettings preferences;
        preferences.setValue(QLatin1String("connection/host"), host);
        preferences.setValue(QLatin1String("connection/port"), port);
    }
    client_.traceConnection("ui save preferences end");
    if (host_->hasFocus() || port_->hasFocus()) {
        client_.traceConnection("ui editor focus begin");
        connectButton_->setFocus(Qt::OtherFocusReason);
        client_.traceConnection("ui editor focus end");
    }
    client_.traceConnection("ui start TCP");
    client_.connectTo(host, static_cast<quint16>(port));
    client_.traceConnection("ui TCP start returned");
}

void ClientWindow::scheduleViewUpdate() {
    if (viewUpdatePending_)
        return;
    viewUpdatePending_ = true;
    // Keep stack switches and editor updates outside socket signal callbacks.
    QTimer::singleShot(0, this, SLOT(applyViewUpdate()));
}

void ClientWindow::applyViewUpdate() {
    viewUpdatePending_ = false;
    updateView();
}

void ClientWindow::updateView() {
    if (updating_)
        return;
    updating_ = true;

    const bool connected = client_.connected();
    const bool reconnecting = client_.reconnecting();
    const bool connecting = client_.connecting() || reconnecting;
    const QString status = client_.status();
    const QString radioName = client_.radioName();
    const int batteryMv = connected ? client_.batteryMillivolts() : 0;
    const QString voltage = radioVoltage(batteryMv);
    status_->setText(connected ? ui("Подключено к ноде") : status);
    const QString settingsName = radioName.isEmpty() ? ui("MeshCore Нода") : plainLine(radioName);
    settingsName_->setText(QFontMetrics(settingsName_->font()).elidedText(settingsName, Qt::ElideRight,
                                                                       qMax(70, width() - 188)));
    settingsName_->setToolTip(settingsName);
    settingsStatus_->setText(connected ? ui("На связи") : reconnecting ? ui("Не в сети · переподключение")
                             : connecting ? status : ui("Не в сети"));
    const QString reconnectStatusStyle = reconnecting
        ? QLatin1String("color: ") + Appearance::paletteFor(this).danger.name() : QString();
    if (settingsStatus_->styleSheet() != reconnectStatusStyle)
        settingsStatus_->setStyleSheet(reconnectStatusStyle);
    settingsReconnectBanner_->setVisible(reconnecting);
    if (reconnecting)
        settingsReconnectText_->setText(ui("Переподключение · попытка %1").arg(client_.reconnectAttempt()));
    settingsRetryNow_->setEnabled(reconnecting && client_.canRetryConnectionNow());
    const int cardBatteryMv = reconnecting ? client_.lastBatteryMillivolts() : batteryMv;
    const int percent = approximateBatteryPercent(cardBatteryMv);
    const QString cardVoltage = radioVoltage(cardBatteryMv);
    settingsBatteryPercent_->setText(percent >= 0 ? ui("%1%").arg(percent)
                                    : cardVoltage.isEmpty() ? ui("—") : cardVoltage);
    const QString batteryHint = cardVoltage.isEmpty() ? ui("Напряжение неизвестно")
        : reconnecting ? ui("Последнее измерение до потери связи") : ui("Напряжение батареи");
    settingsBatteryPercent_->setToolTip(percent >= 0
        ? batteryHint + ui(" · Оценка заряда литий-ионной батареи") : batteryHint);
    settingsBatteryVoltage_->setText(cardVoltage);
    settingsBatteryVoltage_->setToolTip(batteryHint);
    settingsBatteryVoltage_->setVisible(percent >= 0);
    settingsLink_->setText(connected || connecting ? ui("Отключить") : ui("Подключить"));
    settingsAnnounce_->setEnabled(connected);
    connectButton_->setText(connected ? ui("Отключить") :
                            connecting ? ui("Отмена") : ui("Подключить"));
    const bool canEditAddress = !connected && !connecting;
    if (host_->isEnabled() != canEditAddress || port_->isEnabled() != canEditAddress) {
        client_.traceConnection("ui editor availability begin", canEditAddress);
        host_->setEnabled(canEditAddress);
        port_->setEnabled(canEditAddress);
        client_.traceConnection("ui editor availability end", canEditAddress);
    }
    const QString radioLabel = radioName.isEmpty() ? ui("MeshCore Нода") : plainLine(radioName);
    radio_->setText(QFontMetrics(radio_->font()).elidedText(radioLabel, Qt::ElideRight, width() / 2));
    QString radioSummary = connected ? ui("На связи") : connecting ? ui("Подключение…") : ui("Нет связи");
    if (!voltage.isEmpty())
        radioSummary += ui(" · %1").arg(voltage);
    radioState_->setText(radioSummary);
    clearChatSearch_->setEnabled(!chatSearch_->text().isEmpty());
    connectFromChats_->setVisible(!connected);

    if (connected && !wasConnected_ && pages_->currentWidget() == connectionPage_) {
        client_.traceConnection("ui show dashboard begin");
        focusNavigation(tab_);
        pages_->setCurrentWidget(dashboard_);
        client_.traceConnection("ui show dashboard end");
    }
    wasConnected_ = connected;

    QVector<ChatTarget> targets = client_.targets();
    if (pages_->currentWidget() == chatPage_) {
        for (ChatTarget& target : targets) {
            if (target.id == activeId_ && isActiveWindow() && target.unread > 0) {
                client_.markRead(activeId_);
                target.unread = 0;
                break;
            }
        }
        showMessages(targets);
    } else if (pages_->currentWidget() == detailPage_ && chooser_ &&
               chooser_->parent() == detailContent_.data()) {
        updateChatChooser(targets);
    } else if (pages_->currentWidget() == dashboard_) {
        if (tab_ == Chats)
            showTargets(targets);
        else if (tab_ == Network)
            showNodes(client_.nodes());
    }
    int unread = 0;
    for (const ChatTarget& target : targets)
        unread += target.unread;
    const int badge = qMin(100, unread);
    if (badge != unreadBadge_) {
        unreadBadge_ = badge;
        chatsTab_->setIcon(tabIcon(Appearance::paletteFor(this), Chats, badge));
        chatsTab_->setProperty("accessibleName", badge == 0 ? ui("Чаты") :
            ui("Чаты, новых сообщений: %1").arg(badge == 100 ? ui("99+") : QString::number(badge)));
    }
    updating_ = false;
}

void ClientWindow::showTargets(const QVector<ChatTarget>& targets) {
    const QString query = chatSearch_->text().trimmed();
    const int filter = chatFilterIndex_;
    const QMap<QString, ChatMessage> latest = client_.latestMessages();
    const QSet<QString> rooms = roomIds(client_);
    const MessageText::Preferences textPreferences = MessageText::preferences();
    QVector<ChatRow> rows;
    int unreadChats = 0;
    for (const ChatTarget& target : targets) {
        const QMap<QString, ChatMessage>::const_iterator previous = latest.constFind(target.id);
        const bool hasPrevious = previous != latest.constEnd();
        const bool pinned = isPinned(target);
        if (filter == 1 && !target.unread)
            continue;
        if (filter == 2 && !target.channel)
            continue;
        if (filter == 3 && target.channel)
            continue;
        if (filter == 4 && !pinned)
            continue;
        if (filter == 0 && query.isEmpty() && !target.channel && !hasPrevious && !pinned)
            continue;
        QString detail;
        quint32 stamp = 0;
        if (hasPrevious) {
            const ChatMessage& message = previous.value();
            const QString author = message.outgoing ? ui("Вы") : message.author;
            detail = plainLine(MessageText::restore(ChatFeatures::splitQuote(message.text).body,
                                                    textPreferences));
            if (!author.isEmpty())
                detail.prepend(plainLine(author) + QLatin1String(": "));
            stamp = message.timestamp;
        } else {
            detail = target.channel ? ui("Канал") : rooms.contains(target.id) ? ui("Комната") : ui("Личный чат");
        }
        if (!query.isEmpty() && !target.title.contains(query, Qt::CaseInsensitive) &&
                !detail.contains(query, Qt::CaseInsensitive))
            continue;
        if (target.unread > 0)
            ++unreadChats;
        rows.append(ChatRow{target, detail, stamp, pinned});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const ChatRow& left, const ChatRow& right) {
        if (left.pinned != right.pinned)
            return left.pinned;
        return left.timestamp > right.timestamp;
    });

    QString count = ui("Чатов: %1").arg(rows.size());
    if (unreadChats)
        count += ui(" · с новыми: %1").arg(unreadChats);
    chatCount_->setText(count);
    emptyChats_->setText(query.isEmpty() ? ui("Здесь будут ваши чаты и каналы.") :
                                          ui("Поиск не нашёл чатов."));
    emptyChats_->setVisible(rows.isEmpty());
    QVector<ListRow> rendered;
    rendered.reserve(rows.size());
    for (const ChatRow& row : rows) {
        rendered.append(ListRow{row.target.id, row.target.title, row.detail, shortDate(row.timestamp),
                               row.target.unread, row.target.channel, row.pinned,
                               rooms.contains(row.target.id) ? ChatRoomRole : 0});
    }
    displayRows(targets_, rendered);
}

void ClientWindow::showNodes(const QVector<NetworkNode>& nodes) {
    const QString query = nodeSearch_->text().trimmed();
    QVector<NetworkNode> visible;
    for (const NetworkNode& node : nodes) {
        if (query.isEmpty() || node.title.contains(query, Qt::CaseInsensitive) ||
                node.id.contains(query, Qt::CaseInsensitive))
            visible.append(node);
    }
    std::stable_sort(visible.begin(), visible.end(), [](const NetworkNode& left, const NetworkNode& right) {
        return left.lastAdvert > right.lastAdvert;
    });
    if (mapVisible_) {
        map_->setNodes(visible);
        return;
    }

    nodeCount_->setText(ui("Узлов: %1").arg(visible.size()));
    emptyNodes_->setText(query.isEmpty() ? ui("Узлы появятся после подключения к рации.") :
                                          ui("Поиск не нашёл узлов."));
    emptyNodes_->setVisible(visible.isEmpty());
    QVector<ListRow> rendered;
    rendered.reserve(visible.size());
    for (const NetworkNode& node : visible) {
        QString detail = roleName(node.role);
        if (node.path == 0)
            detail += ui(" · напрямую");
        else if (node.path > 0)
            detail += ui(" · переходов: %1").arg(node.path);
        rendered.append(ListRow{node.id, node.title, detail, shortDate(node.lastAdvert), 0,
                                false, false, node.role > 0 ? node.role : 5});
    }
    displayRows(nodes_, rendered);
}

void ClientWindow::showMessages(const QVector<ChatTarget>& targets) {
    for (const ChatTarget& target : targets) {
        if (target.id == activeId_) {
            activeTitle_ = target.title;
            chatPage_->setPinned(isPinned(target));
            break;
        }
    }
    chatPage_->setContext(activeTitle_, client_.status(), activeId_);
    chatPage_->setMessages(client_.messages(activeId_));
    chatPage_->setSendingState(client_.connected() && !client_.busy(), client_.maxTextBytes(activeId_));
}

void ClientWindow::sendMessage(const QString& text) {
    if (client_.send(activeId_, text)) {
        chatPage_->followSentMessage(text);
        chatPage_->clearDraft();
        drafts_.remove(activeId_);
    }
}

void ClientWindow::resendMessage(const QString& text) {
    if (client_.send(activeId_, text))
        chatPage_->followSentMessage(text);
}

void ClientWindow::reactToMessage(const ChatMessage& message, const QString& emoji) {
    client_.react(activeId_, message, emoji);
}

void ClientWindow::setSenderBlocked(const QString& id, const QString& name, bool blocked) {
    client_.setSenderBlocked(id, name, blocked);
}

void ClientWindow::saveDraft() {
    if (activeId_.isEmpty())
        return;
    const QString draft = chatPage_->draft();
    if (draft.isEmpty())
        drafts_.remove(activeId_);
    else
        drafts_.insert(activeId_, draft);
}

void ClientWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::ActivationChange)
        && appearanceTheme_ == Appearance::System)
        applyAppearance(appearanceTheme_, chatTextSize_);
    if (event->type() == QEvent::ActivationChange && pages_ && isActiveWindow())
        scheduleViewUpdate();
}

void ClientWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() != Qt::Key_Back && event->key() != Qt::Key_Escape) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (pages_->currentWidget() == chatPage_)
        showChats();
    else if (pages_->currentWidget() == connectionPage_ || pages_->currentWidget() == detailPage_)
        returnToDashboard();
    else if (tab_ == Network && mapVisible_)
        showMap();
    else if (tab_ != Chats)
        showChats();
    else
        exitApplication();
    event->accept();
}
