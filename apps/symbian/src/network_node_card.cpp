#include "network_node_card.hpp"

#include "appearance.hpp"
#include "network_filters.hpp"
#include "network_node_management.hpp"
#include "node_chat_options.hpp"
#include "radio_settings.hpp"
#include "settings_icons.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtGui/QAction>
#include <QtGui/QApplication>
#include <QtGui/QClipboard>
#include <QtGui/QHBoxLayout>
#include <QtGui/QLabel>
#include <QtGui/QMenu>
#include <QtGui/QMessageBox>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString ui(const char* utf8) {
    return QString::fromUtf8(utf8);
}

QLabel* label(QWidget* parent, const QString& text, const char* style = nullptr) {
    auto* result = new QLabel(text, parent);
    if (style)
        result->setObjectName(QLatin1String(style));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

QPushButton* button(QWidget* parent, const QString& text, const char* name) {
    auto* result = new QPushButton(text, parent);
    result->setObjectName(QLatin1String(name));
    result->setMinimumHeight(41);
    return result;
}

void emphasize(QPushButton* button, const Appearance::Palette& colors) {
    button->setStyleSheet(QString::fromLatin1(
        "QPushButton:enabled { background: %1; color: %2; border-color: %1; }"
        "QPushButton:pressed { background: %3; }")
        .arg(colors.accent.name(), colors.onAccent.name(),
             Appearance::mix(colors.accent, colors.surface, 30).name()));
}

QWidget* card(QWidget* parent) {
    auto* result = new QWidget(parent);
    result->setObjectName(QLatin1String("settingsGroup"));
    result->setAttribute(Qt::WA_StyledBackground);
    return result;
}

QString roleName(int role) {
    switch (role) {
    case 1: return ui("Человек");
    case 2: return ui("Репитер");
    case 3: return ui("Комната");
    case 4: return ui("Датчик");
    default: return ui("Узел MeshCore");
    }
}

QString advertAge(quint32 stamp) {
    const quint32 now = QDateTime::currentDateTime().toTime_t();
    if (!stamp)
        return ui("ещё не слышно");
    if (now >= stamp) {
        const quint32 elapsed = now - stamp;
        if (elapsed < 60)
            return ui("слышно только что");
        if (elapsed < 3600)
            return ui("слышно %1 мин назад").arg(elapsed / 60);
        if (elapsed < 24 * 3600)
            return ui("слышно %1 ч назад").arg(elapsed / 3600);
    }
    return ui("слышно %1").arg(QDateTime::fromTime_t(stamp).toString(QLatin1String("dd.MM hh:mm")));
}

QString routeDescription(int hops) {
    if (hops < 0)
        return ui("По всей сети");
    if (hops == 0)
        return ui("Напрямую");
    return ui("Переходов · %1").arg(hops);
}

QString relativePosition(const NetworkNode& self, const NetworkNode& node) {
    if (!NetworkFilters::hasPosition(self) || !NetworkFilters::hasPosition(node))
        return QString();
    constexpr double radians = 3.14159265358979323846 / 180000000.0;
    const double fromLat = self.latitude * radians;
    const double toLat = node.latitude * radians;
    const double longitudeDelta = (static_cast<double>(node.longitude) - self.longitude) * radians;
    const double north = std::cos(fromLat) * std::sin(toLat) -
                         std::sin(fromLat) * std::cos(toLat) * std::cos(longitudeDelta);
    const double east = std::cos(toLat) * std::sin(longitudeDelta);
    const double dot = std::sin(fromLat) * std::sin(toLat) +
                       std::cos(fromLat) * std::cos(toLat) * std::cos(longitudeDelta);
    const double distance = 6371.0 * std::atan2(std::sqrt(north * north + east * east), dot);
    const double direction = std::atan2(east, north) / (radians * 1000000);
    const int degrees = (qRound(direction) + 360) % 360;
    return ui("%1 км · азимут %2°").arg(distance, 0, 'f', 1).arg(degrees);
}

QPixmap nodeAvatar(const Appearance::Palette& colors, int role) {
    QPixmap result(46, 46);
    result.fill(Qt::transparent);
    const SettingsIcons::Kind icon = role == 2 ? SettingsIcons::Node :
        role == 3 ? SettingsIcons::Contacts : role == 4 ? SettingsIcons::Readings : SettingsIcons::Name;
    const QColor accent = role == 2 ? colors.accent : role == 3
        ? Appearance::mix(colors.accent, QColor(132, 120, 212), 48) : role == 4
        ? Appearance::mix(colors.accent, QColor(216, 174, 117), 50) : colors.muted;
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Appearance::mix(colors.surface, accent, colors.dark ? 24 : 15));
    painter.drawEllipse(QRect(1, 1, 44, 44));
    SettingsIcons::draw(painter, icon, QRect(10, 10, 26, 26), accent);
    return result;
}

QPixmap star(const QColor& color, bool filled) {
    QPixmap result(22, 22);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(filled ? QBrush(color) : QBrush(Qt::NoBrush));
    QPainterPath outline;
    constexpr double turn = 3.14159265358979323846 / 5;
    for (int index = 0; index < 10; ++index) {
        const double radius = index % 2 == 0 ? 9.1 : 4.4;
        const QPointF point(11 + radius * std::sin(index * turn),
                            11 - radius * std::cos(index * turn));
        if (index == 0)
            outline.moveTo(point);
        else
            outline.lineTo(point);
    }
    outline.closeSubpath();
    painter.drawPath(outline);
    return result;
}

enum CardAction {
    Route = 1, Neighbours, Settings, Access, Console, History, CopyKey,
    ChatNotifications, ChatSound, ChatBackground, Readings
};

QVBoxLayout* group(QVBoxLayout* page) {
    QWidget* surface = card(page->parentWidget());
    auto* rows = new QVBoxLayout(surface);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    page->addWidget(surface);
    return rows;
}

QLabel* rowIcon(QWidget* owner, SettingsIcons::Kind icon, const QColor& color, int size = 18) {
    auto* image = new QLabel(owner);
    image->setPixmap(SettingsIcons::icon(icon, color, size).pixmap(size));
    image->setFixedSize(size, size);
    image->setAttribute(Qt::WA_TransparentForMouseEvents);
    return image;
}

QPushButton* iconButton(QWidget* parent, SettingsIcons::Kind icon, const QColor& color,
                       const char* name, const QString& description) {
    auto* result = button(parent, QString(), name);
    result->setIcon(SettingsIcons::icon(icon, color, 18));
    result->setIconSize(QSize(18, 18));
    result->setFixedSize(35, 35);
    result->setToolTip(description);
    result->setProperty("accessibleName", description);
    return result;
}

QPushButton* actionRow(QVBoxLayout* rows, const QString& title, const char* name,
                       int action, const Appearance::Palette& colors, int icon = -1,
                       QLabel** value = nullptr, SettingsIcons::Kind trailing = SettingsIcons::Chevron,
                       QLabel** trailingIcon = nullptr, bool valueBelow = false) {
    auto* item = new QPushButton(rows->parentWidget());
    item->setObjectName(QLatin1String(name));
    item->setProperty("networkRow", true);
    item->setProperty("cardAction", action);
    item->setProperty("accessibleName", title);
    item->setMinimumHeight(41);
    auto* line = new QHBoxLayout(item);
    line->setContentsMargins(10, 4, 10, 4);
    line->setSpacing(7);
    if (icon >= 0)
        line->addWidget(rowIcon(item, static_cast<SettingsIcons::Kind>(icon), colors.muted));
    QLabel* titleLabel = label(item, QString(), "networkRowTitle");
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setText(QLatin1String("<span>") + title + QLatin1String("</span>"));
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QBoxLayout* text = line;
    if (valueBelow) {
        auto* stack = new QVBoxLayout;
        stack->setSpacing(1);
        line->addLayout(stack, 1);
        text = stack;
    }
    text->addWidget(titleLabel, valueBelow || !value ? 0 : 1);
    if (value) {
        *value = label(item, QString(), "networkRowValue");
        (*value)->setAlignment((valueBelow ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        (*value)->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->addWidget(*value, valueBelow ? 0 : 1);
    }
    if (action) {
        QLabel* end = rowIcon(item, trailing, colors.muted, 15);
        line->addWidget(end);
        if (trailingIcon)
            *trailingIcon = end;
    } else {
        item->setEnabled(false);
    }
    if (rows->count()) {
        auto* separator = new QWidget(rows->parentWidget());
        separator->setObjectName(QLatin1String("networkRowDivider"));
        separator->setAttribute(Qt::WA_StyledBackground);
        separator->setFixedHeight(1);
        rows->addWidget(separator);
    }
    rows->addWidget(item);
    return item;
}

}  // namespace

NetworkNodeCard::NetworkNodeCard(Client& client, const NetworkNode& node, bool favorite, QWidget* parent)
    : QWidget(parent), client_(client), node_(node) {
    setObjectName(QLatin1String("networkNodeCard"));
    const Appearance::Palette colors = Appearance::paletteFor(this);
    const NetworkNode self = client_.selfNode();
    const bool own = !node_.publicKey.isEmpty() && node_.publicKey == self.publicKey;
    const bool conversation = node_.role == 1 || node_.role == 3;
    const bool managed = !own && node_.role >= 2 && node_.role <= 4;
    if (NetworkFilters::hasPosition(node_)) {
        coordinates_ = QString::fromLatin1("%1, %2").arg(node_.latitude / 1000000.0, 0, 'f', 5)
                                                    .arg(node_.longitude / 1000000.0, 0, 'f', 5);
    }
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(9, 2, 9, 9);
    page->setSpacing(8);

    auto* heading = new QHBoxLayout;
    heading->setSpacing(8);
    auto* avatar = new QLabel(this);
    avatar->setPixmap(nodeAvatar(colors, node_.role));
    avatar->setFixedSize(46, 46);
    heading->addWidget(avatar, 0, Qt::AlignTop);
    auto* identity = new QVBoxLayout;
    identity->setSpacing(2);
    auto* nameLine = new QHBoxLayout;
    nameLine->setSpacing(3);
    auto* name = label(this, node_.title, "networkNodeTitle");
    name->setToolTip(node_.title);
    nameLine->addWidget(name, 1);
    auto* copyNameButton = iconButton(this, SettingsIcons::Copy, colors.muted, "networkNodeCopyName",
                                      ui("Копировать имя"));
    nameLine->addWidget(copyNameButton);
    connect(copyNameButton, SIGNAL(clicked()), this, SLOT(copyName()), Qt::QueuedConnection);
    identity->addLayout(nameLine);
    const QString summary = own ? ui("Своя нода") : roleName(node_.role) + ui(" · ") + advertAge(node_.lastAdvert);
    identity->addWidget(label(this, summary, "muted"));
    const QString distance = own ? QString() : relativePosition(self, node_);
    if (!distance.isEmpty())
        identity->addWidget(label(this, distance, "muted"));
    heading->addLayout(identity, 1);
    auto* starButton = button(this, QString(), "networkNodeFavorite");
    starButton->setFixedSize(36, 36);
    starButton->setIconSize(QSize(22, 22));
    starButton->setProperty("accessibleName", ui("Избранное"));
    starButton->setToolTip(ui("Избранное"));
    QIcon favoriteIcon;
    favoriteIcon.addPixmap(star(colors.muted, false), QIcon::Normal, QIcon::Off);
    favoriteIcon.addPixmap(star(colors.accent, true), QIcon::Normal, QIcon::On);
    starButton->setIcon(favoriteIcon);
    starButton->setCheckable(true);
    starButton->setChecked(favorite);
    heading->addWidget(starButton, 0, Qt::AlignTop);
    page->addLayout(heading);
    connect(starButton, SIGNAL(toggled(bool)), this, SIGNAL(favoriteChanged(bool)));

    auto* actions = new QHBoxLayout;
    actions->setSpacing(7);
    if (conversation && !own) {
        auto* write = button(this, ui("Написать"), "networkNodeChat");
        write->setIcon(SettingsIcons::icon(SettingsIcons::Chat, colors.onAccent, 18));
        write->setEnabled(client_.maxTextBytes(node_.id) > 0);
        emphasize(write, colors);
        actions->addWidget(write, 1);
        connect(write, SIGNAL(clicked()), this, SLOT(openChat()), Qt::QueuedConnection);
    }
    if (managed) {
        signIn_ = button(this, ui("Войти"), "networkNodeSignIn");
        signIn_->setIcon(SettingsIcons::icon(SettingsIcons::Lock, conversation ? colors.muted : colors.onAccent, 18));
        if (!conversation)
            emphasize(signIn_, colors);
        actions->addWidget(signIn_, 1);
        connect(signIn_, SIGNAL(clicked()), this, SLOT(openSignIn()), Qt::QueuedConnection);
    }
    auto* more = button(this, ui("Ещё"), "networkNodeMore");
    more->setIcon(SettingsIcons::icon(SettingsIcons::More, colors.accent, 18));
    actions->addWidget(more, 1);
    connect(more, SIGNAL(clicked()), this, SLOT(openMore()), Qt::QueuedConnection);
    page->addLayout(actions);

    if (!own) {
        auto* routeSurface = card(this);
        auto* route = new QHBoxLayout(routeSurface);
        route->setContentsMargins(3, 1, 4, 1);
        route->setSpacing(2);
        auto* routeRows = new QVBoxLayout;
        route->addLayout(routeRows, 1);
        auto* routeButton = actionRow(routeRows, ui("Маршрут"), "networkNodeRoute", Route,
                                      colors, -1, &routeValue_, SettingsIcons::Chevron, nullptr, true);
        routeButton->setMinimumHeight(47);
        routeValue_->setText(routeDescription(node_.path));
        connect(routeButton, SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        routeCheck_ = iconButton(routeSurface, SettingsIcons::RouteCheck, colors.accent,
                                 "networkNodeRouteCheck", ui("Проверить маршрут"));
        connect(routeCheck_, SIGNAL(clicked()), this, SLOT(checkRoute()), Qt::QueuedConnection);
        route->addWidget(routeCheck_);
        page->addWidget(routeSurface);
        routeFeedback_ = label(this, QString(), "networkNodeFeedback");
        routeFeedback_->hide();
        page->addWidget(routeFeedback_);
    }

    if (conversation && !own) {
        auto* preferences = group(page);
        connect(actionRow(preferences, ui("Уведомления"), "networkNodeNotifications", ChatNotifications,
                          colors, SettingsIcons::Notifications, &notifications_),
                SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        connect(actionRow(preferences, ui("Звук"), "networkNodeSound", ChatSound,
                          colors, SettingsIcons::Sound, &sound_),
                SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        connect(actionRow(preferences, ui("Фон"), "networkNodeBackground", ChatBackground,
                          colors, SettingsIcons::Appearance, &background_),
                SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        updateChatOptions();
    }

    if (!own && (conversation || managed)) {
        readings_ = new QWidget(this);
        auto* readingLayout = new QVBoxLayout(readings_);
        readingLayout->setContentsMargins(0, 0, 0, 0);
        readingLayout->setSpacing(4);
        readingLayout->addWidget(label(readings_, ui("ПОКАЗАНИЯ"), "settingsSectionTitle"));
        auto* rows = group(readingLayout);
        readingButton_ = actionRow(rows, ui("Обновить"), "networkNodeReadings", Readings,
                                   colors, SettingsIcons::Readings, nullptr, SettingsIcons::Air);
        connect(readingButton_, SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        readingsValue_ = label(readings_, QString(), "muted");
        readingsValue_->hide();
        readingLayout->addWidget(readingsValue_);
        page->addWidget(readings_);
    }

    if (managed) {
        page->addWidget(label(this, ui("УПРАВЛЕНИЕ"), "settingsSectionTitle"));
        auto* rows = group(page);
        const auto addManagement = [&](const QString& title, const char* name, int action,
                                       SettingsIcons::Kind icon, bool admin) {
            QLabel* status = nullptr;
            QPushButton* row = actionRow(rows, title, name, action, colors, icon, nullptr,
                                         SettingsIcons::Lock, &status);
            management_.append({row, status, admin});
            connect(row, SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
        };
        if (node_.role == 2)
            addManagement(ui("Соседи"), "networkNodeNeighbours", Neighbours, SettingsIcons::Contacts, false);
        if (node_.role == 4)
            addManagement(ui("История"), "networkNodeHistory", History, SettingsIcons::Readings, false);
        addManagement(ui("Настройки"), "networkNodeSettings", Settings, SettingsIcons::Advanced, true);
        addManagement(ui("Доступ"), "networkNodeAccess", Access, SettingsIcons::Privacy, true);
        addManagement(ui("Консоль"), "networkNodeConsole", Console, SettingsIcons::Terminal, true);
        managementHint_ = label(this, QString(), "muted");
        page->addWidget(managementHint_);
    }

    page->addWidget(label(this, ui("ПОДРОБНОСТИ"), "settingsSectionTitle"));
    auto* details = group(page);
    if (!node_.publicKey.isEmpty()) {
        QLabel* publicKey = nullptr;
        auto* copy = actionRow(details, ui("Открытый ключ"), "networkNodeCopyKey", CopyKey,
                               colors, -1, &publicKey, SettingsIcons::Copy);
        publicKey->setText(node_.publicKey.left(16) + ui("…"));
        connect(copy, SIGNAL(clicked()), this, SLOT(runAction()), Qt::QueuedConnection);
    }
    if (!coordinates_.isEmpty()) {
        QLabel* coordinates = nullptr;
        auto* position = actionRow(details, ui("Координаты"), "networkNodeCoordinates", 0,
                                   colors, -1, &coordinates);
        position->setProperty("networkInfoRow", true);
        coordinates->setTextFormat(Qt::RichText);
        coordinates->setText(QLatin1String("<span>") + coordinates_ + QLatin1String("</span>"));
    }
    feedback_ = label(this, QString(), "networkNodeFeedback");
    feedback_->hide();
    page->addWidget(feedback_);
    page->addStretch();
    connect(&client_, SIGNAL(changed()), this, SLOT(refreshState()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(nodeManagementResult(int,int,bool,QByteArray,QString)),
            this, SLOT(managementResult(int,int,bool,QByteArray,QString)), Qt::QueuedConnection);
    connect(&client_, SIGNAL(consoleResult(int,int,bool,QString)),
            this, SLOT(consoleResult(int,int,bool,QString)), Qt::QueuedConnection);
    refreshState();
}

void NetworkNodeCard::openChat() {
    if (client_.maxTextBytes(node_.id) > 0)
        emit chatRequested(node_.id);
}

void NetworkNodeCard::locateNode() {
    if (!coordinates_.isEmpty())
        emit showOnMap(node_.id);
}

void NetworkNodeCard::openSignIn() {
    if (!client_.connected())
        return;
    if (client_.nodeSignedIn(node_.id)) {
        auto* options = new QMenu(this);
        options->addAction(ui("Войти заново"), this, SLOT(signInAgain()));
        options->addAction(ui("Выйти"), this, SLOT(signOut()));
        connect(options, SIGNAL(aboutToHide()), options, SLOT(deleteLater()));
        options->popup(signIn_->mapToGlobal(QPoint(0, signIn_->height())));
        return;
    }
    signInAgain();
}

void NetworkNodeCard::signInAgain() {
    if (!client_.connected())
        return;
    RadioConsoleDialog dialog(client_, node_.id, ui("Войти · %1").arg(node_.title), window(), true);
    if (dialog.exec() == QDialog::Accepted)
        showFeedback(ui("Вход выполнен"));
    refreshState();
}

void NetworkNodeCard::signOut() {
    const int request = client_.nextConsoleRequestId();
    if (client_.logOutNode(request, node_.id)) {
        managementRequest_ = request;
        showFeedback(ui("Выход…"));
    }
}

void NetworkNodeCard::openConsole() {
    if (!client_.connected() || !client_.nodeConsoleAuthorized(node_.id))
        return;
    RadioConsoleDialog dialog(client_, node_.id, ui("Консоль · %1").arg(node_.title), window());
    dialog.exec();
}

void NetworkNodeCard::openMore() {
    auto* anchor = qobject_cast<QPushButton*>(sender());
    if (!anchor)
        return;
    auto* options = new QMenu(this);
    options->addAction(ui("Копировать имя"), this, SLOT(copyName()));
    if (!node_.publicKey.isEmpty())
        options->addAction(ui("Копировать открытый ключ"), this, SLOT(copyKey()));
    if (!coordinates_.isEmpty()) {
        options->addAction(ui("Показать на карте"), this, SLOT(locateNode()));
        options->addAction(ui("Копировать координаты"), this, SLOT(copyCoordinates()));
    }
    if (node_.role == 2 && client_.nodeConsoleAuthorized(node_.id)) {
        options->addSeparator();
        struct RemoteAction { const char* caption; const char* command; bool confirm; };
        const RemoteAction commands[] = {{"Анонсировать по сети", "advert", false},
                                         {"Анонсировать рядом", "advert.zerohop", false},
                                         {"Сбросить статистику", "clear stats", true}};
        for (const RemoteAction& command : commands) {
            QAction* action = options->addAction(ui(command.caption), this, SLOT(runRemoteOption()));
            action->setData(QLatin1String(command.command));
            action->setProperty("confirm", command.confirm);
            action->setEnabled(!client_.busy() && !commandRequest_);
        }
    }
    connect(options, SIGNAL(aboutToHide()), options, SLOT(deleteLater()));
    options->popup(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void NetworkNodeCard::runRemoteOption() {
    QAction* action = qobject_cast<QAction*>(sender());
    if (!action || commandRequest_ || !client_.nodeConsoleAuthorized(node_.id))
        return;
    const QString title = action->text();
    const QString command = action->data().toString();
    if (action->property("confirm").toBool() &&
            QMessageBox::question(window(), title, ui("Сбросить счётчики репитера %1?").arg(node_.title),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const int request = client_.nextConsoleRequestId();
    if (client_.runConsole(request, node_.id, command)) {
        commandRequest_ = request;
        showFeedback(ui("Команда отправляется…"));
    }
}

void NetworkNodeCard::consoleResult(int request, int operation, bool success, const QString& text) {
    if (operation != meshcore::RadioConsole::Remote || request != commandRequest_)
        return;
    commandRequest_ = 0;
    showFeedback(success && text.isEmpty() ? ui("Команда выполнена") : text);
}

void NetworkNodeCard::runAction() {
    const int action = sender()->property("cardAction").toInt();
    switch (action) {
    case Route: openRoute(); break;
    case Neighbours: emit managementRequested(node_.id, NetworkNodeManagementPanel::Neighbours); break;
    case Settings: emit managementRequested(node_.id, NetworkNodeManagementPanel::Settings); break;
    case Access: emit managementRequested(node_.id, NetworkNodeManagementPanel::Access); break;
    case Console: openConsole(); break;
    case History: showReadingsHistory(); break;
    case CopyKey: copyKey(); break;
    case ChatNotifications:
    case ChatSound:
    case ChatBackground:
        editChatOption(action);
        break;
    case Readings: requestReadings(); break;
    }
}

void NetworkNodeCard::openRoute() {
    const QString title = ui("Маршрут · %1").arg(node_.title);
    QMessageBox box(window());
    box.setWindowTitle(title);
    box.setText(routeDescription(node_.path));
    QPushButton* trace = client_.canTraceNodeRoute(node_.id)
        ? box.addButton(ui("По пути"), QMessageBox::ActionRole) : nullptr;
    QPushButton* direct = client_.canTraceNodeRoute(node_.id, true) && node_.path != 0
        ? box.addButton(ui("Напрямую"), QMessageBox::ActionRole) : nullptr;
    QPushButton* reset = node_.path >= 0 && client_.connected()
        ? box.addButton(ui("Сбросить"), QMessageBox::ActionRole) : nullptr;
    box.addButton(QMessageBox::Close);
    box.exec();
    if ((trace && box.clickedButton() == trace) || (direct && box.clickedButton() == direct)) {
        requestRouteCheck(box.clickedButton() == direct);
    } else if (reset && box.clickedButton() == reset && !managementRequest_) {
        const int request = client_.nextConsoleRequestId();
        if (client_.requestNodeRouteReset(request, node_.id)) {
            managementRequest_ = request;
            routeFeedback_->setText(ui("Сброс маршрута…"));
        } else {
            routeFeedback_->setText(ui("Дождитесь окончания текущего запроса."));
        }
        routeFeedback_->show();
    }
}

void NetworkNodeCard::copyKey() {
    copyText(node_.publicKey, ui("Открытый ключ скопирован"));
}

void NetworkNodeCard::copyName() {
    copyText(node_.title, ui("Имя скопировано"));
}

void NetworkNodeCard::copyCoordinates() {
    copyText(coordinates_, ui("Координаты скопированы"));
}

void NetworkNodeCard::copyText(const QString& text, const QString& confirmation) {
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
        showFeedback(confirmation);
    }
}

void NetworkNodeCard::showFeedback(const QString& message) {
    feedback_->setText(message);
    feedback_->show();
}

void NetworkNodeCard::refreshState() {
    const Appearance::Palette colors = Appearance::paletteFor(this);
    const bool online = client_.connected();
    const bool signedIn = online && client_.nodeSignedIn(node_.id);
    const bool admin = signedIn && client_.nodeConsoleAuthorized(node_.id);
    if (signIn_) {
        signIn_->setEnabled(online && !client_.busy());
        signIn_->setText(admin ? ui("Админ") : signedIn ? ui("Вы вошли") : ui("Войти"));
        const QColor color = node_.role == 3 ? colors.muted : colors.onAccent;
        signIn_->setIcon(SettingsIcons::icon(signedIn ? SettingsIcons::Check : SettingsIcons::Lock,
                                            color, 18));
    }
    for (const ManagementRow& row : management_) {
        const bool allowed = row.admin ? admin : signedIn;
        row.button->setEnabled(allowed);
        row.status->setPixmap(SettingsIcons::icon(allowed ? SettingsIcons::Chevron : SettingsIcons::Lock,
                                                  colors.muted, 15).pixmap(15));
    }
    if (managementHint_) {
        managementHint_->setText(signedIn ? ui("Для настроек, доступа и консоли нужны права администратора.")
            : ui("Войдите, чтобы открыть управление узлом. Для настроек, доступа и консоли нужны права администратора."));
        managementHint_->setVisible(!admin);
    }
    if (readings_) {
        readings_->setVisible(node_.role == 1 || signedIn);
        readingButton_->setEnabled(online && (!signIn_ || signedIn) && !client_.busy());
    }
    if (routeCheck_)
        routeCheck_->setEnabled(!managementRequest_ && (client_.canTraceNodeRoute(node_.id) ||
                                                        client_.canTraceNodeRoute(node_.id, true)));
}

void NetworkNodeCard::updateChatOptions() {
    const bool room = node_.role == 3;
    notifications_->setText(NodeChatOptions::label(NodeChatOptions::Notifications, node_.id, room));
    sound_->setText(NodeChatOptions::label(NodeChatOptions::Sound, node_.id, room));
    background_->setText(NodeChatOptions::label(NodeChatOptions::Background, node_.id, room));
}

void NetworkNodeCard::editChatOption(int action) {
    const auto field = action == ChatNotifications ? NodeChatOptions::Notifications :
                       action == ChatSound ? NodeChatOptions::Sound : NodeChatOptions::Background;
    if (NodeChatOptions::choose(field, node_.id, node_.role == 3, window()))
        updateChatOptions();
}

void NetworkNodeCard::requestReadings() {
    if (managementRequest_)
        return;
    const int request = client_.nextConsoleRequestId();
    if (client_.requestNodeTelemetry(request, node_.id)) {
        managementRequest_ = request;
        readingsValue_->setText(ui("Запрашиваем показания…"));
    } else {
        readingsValue_->setText(ui("Запрос сейчас недоступен."));
    }
    readingsValue_->show();
}

void NetworkNodeCard::showReadingsHistory() {
    QSettings settings;
    QStringList samples = settings.value(QLatin1String("network/readings/") + node_.id).toStringList();
    const QString title = ui("История · %1").arg(node_.title);
    QMessageBox::information(window(), title, samples.isEmpty() ? ui("Сохранённых показаний пока нет.") :
                                                                   samples.join(QLatin1String("\n")));
}

void NetworkNodeCard::managementResult(int request, int operation, bool success, const QByteArray& data,
                                       const QString& error) {
    if (request != managementRequest_)
        return;
    managementRequest_ = 0;
    if (operation == Client::NodeTelemetry) {
        QString readings = error;
        if (success) {
            const NodeTelemetryReadings result = Client::parseNodeTelemetry(data);
            QStringList values;
            if (result.batteryMillivolts)
                values << ui("Батарея %1 В").arg(*result.batteryMillivolts / 1000.0, 0, 'f', 2);
            if (result.temperatureTenthsC)
                values << ui("Температура %1 °C").arg(*result.temperatureTenthsC / 10.0, 0, 'f', 1);
            if (values.isEmpty()) {
                readings = ui("Узел не передал поддерживаемых показаний.");
            } else {
                readings = values.join(ui(" · "));
                QSettings settings;
                const QString key = QLatin1String("network/readings/") + node_.id;
                QStringList samples = settings.value(key).toStringList();
                samples.prepend(QDateTime::currentDateTime().toString(QLatin1String("dd.MM hh:mm")) +
                                QLatin1String("  ") + readings);
                while (samples.size() > 12)
                    samples.removeLast();
                settings.setValue(key, samples);
            }
        }
        readingsValue_->setText(readings);
        readingsValue_->show();
    } else if (operation == Client::NodeRouteTrace) {
        const auto result = success ? Client::parseNodeRouteTrace(data) : std::nullopt;
        if (result) {
            QString text = ui("Ответ получен · %1 мс").arg(result->roundTripMs);
            if (!result->snrQuarters.isEmpty()) {
                const int weakest = *std::min_element(result->snrQuarters.constBegin(), result->snrQuarters.constEnd());
                text += ui(" · SNR от %1 дБ").arg(weakest / 4.0, 0, 'f', 1);
            }
            routeFeedback_->setText(text);
        } else {
            routeFeedback_->setText(success ? ui("Не удалось прочитать ответ трассировки.") : error);
        }
        routeFeedback_->show();
    } else if (operation == Client::NodeRouteReset) {
        if (success) {
            node_.path = -1;
            routeValue_->setText(routeDescription(node_.path));
        }
        routeFeedback_->setText(success ? ui("Маршрут сброшен") : error);
        routeFeedback_->show();
    } else if (operation == Client::NodeLogout && success) {
        showFeedback(ui("Вы вышли из узла"));
    } else if (!success) {
        showFeedback(error);
    }
    refreshState();
}

void NetworkNodeCard::checkRoute() {
    requestRouteCheck(!client_.canTraceNodeRoute(node_.id));
}

void NetworkNodeCard::requestRouteCheck(bool direct) {
    if (managementRequest_ || !client_.canTraceNodeRoute(node_.id, direct))
        return;
    const int request = client_.nextConsoleRequestId();
    if (client_.requestNodeRouteTrace(request, node_.id, direct)) {
        managementRequest_ = request;
        routeFeedback_->setText(direct ? ui("Проверяем прямую связь…") : ui("Проверяем сохранённый маршрут…"));
        routeFeedback_->show();
        refreshState();
    }
}
