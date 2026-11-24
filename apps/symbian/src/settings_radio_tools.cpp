#include "settings_radio_tools.hpp"

#include "appearance.hpp"
#include "client.hpp"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtGui/QHBoxLayout>
#include <QtGui/QHideEvent>
#include <QtGui/QLabel>
#include <QtGui/QListWidget>
#include <QtGui/QPushButton>
#include <QtGui/QShowEvent>
#include <QtGui/QVBoxLayout>

namespace {

constexpr int PacketLimit = 80;
constexpr int PacketHeight = 65;

QString ui(const char* text) { return QString::fromUtf8(text); }

QLabel* label(const QString& text, QWidget* parent, const char* style = "muted") {
    auto* result = new QLabel(text, parent);
    result->setObjectName(QLatin1String(style));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

QPushButton* button(const QString& text, QWidget* parent, const char* name) {
    auto* result = new QPushButton(text, parent);
    result->setObjectName(QLatin1String(name));
    result->setMinimumHeight(41);
    return result;
}

QString packetType(int type) {
    switch (type) {
    case 0: return ui("Запрос");
    case 1: return ui("Ответ");
    case 2: return ui("Личное сообщение");
    case 3: return ui("Подтверждение");
    case 4: return ui("Анонс");
    case 5: return ui("Сообщение канала");
    case 6: return ui("Данные канала");
    case 7: return ui("Анонимный запрос");
    case 8: return ui("Маршрут");
    case 9: return ui("Проверка маршрута");
    case 10: return ui("Фрагмент пакета");
    case 11: return ui("Управление");
    case 15: return ui("Данные");
    default: return type < 0 ? ui("Не разобран") : ui("Тип %1").arg(type);
    }
}

QString signedNumber(double amount) {
    return (amount > 0 ? QLatin1String("+") : QLatin1String("")) + QString::number(amount, 'g', 4);
}

QString passage(int route, int hops, int bytes) {
    if (route < 0)
        return ui("%1 Б").arg(bytes);
    const QString kind = route == 0 || route == 1 ? ui("По сети") : ui("По маршруту");
    return ui("%1 · %2 пер. · %3 Б").arg(kind).arg(hops).arg(bytes);
}

}  // namespace

RadioAirPanel::RadioAirPanel(Client& client, QWidget* parent) : QWidget(parent), client_(client) {
    setObjectName(QLatin1String("settingsRadioAir"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(9, 7, 9, 10);
    page->setSpacing(7);
    status_ = label(QString(), this, "radioAirStatus");
    page->addWidget(status_);
    auto* controls = new QHBoxLayout;
    pause_ = button(ui("Пауза"), this, "radioAirPause");
    clear_ = button(ui("Очистить"), this, "radioAirClear");
    controls->addWidget(pause_, 1);
    controls->addWidget(clear_, 1);
    page->addLayout(controls);
    empty_ = label(ui("Пакетов пока нет."), this);
    page->addWidget(empty_);
    packets_ = new QListWidget(this);
    packets_->setObjectName(QLatin1String("radioAirPackets"));
    packets_->setSelectionMode(QAbstractItemView::NoSelection);
    packets_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    packets_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    packets_->setFocusPolicy(Qt::NoFocus);
    page->addWidget(packets_);
    page->addStretch();
    timer_ = new QTimer(this);
    timer_->setInterval(350);
    connect(timer_, SIGNAL(timeout()), this, SLOT(poll()));
    connect(pause_, SIGNAL(clicked()), this, SLOT(toggleListening()));
    connect(clear_, SIGNAL(clicked()), this, SLOT(clearLog()));
    connect(&client_, SIGNAL(changed()), this, SLOT(refreshView()), Qt::QueuedConnection);
    refreshView();
}

void RadioAirPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    connect(&client_, SIGNAL(radioPacketReceived(int,int,int,int,int,int)),
            this, SLOT(receivePacket(int,int,int,int,int,int)), Qt::UniqueConnection);
    lastReadings_.invalidate();
    timer_->start();
    poll();
}

void RadioAirPanel::hideEvent(QHideEvent* event) {
    disconnect(&client_, SIGNAL(radioPacketReceived(int,int,int,int,int,int)),
               this, SLOT(receivePacket(int,int,int,int,int,int)));
    timer_->stop();
    QWidget::hideEvent(event);
}

void RadioAirPanel::receivePacket(int snr, int rssi, int type, int hops, int bytes, int route) {
    if (paused_ || !client_.connected())
        return;
    if (pending_.size() >= PacketLimit)
        pending_.remove(0);
    pending_.append(Packet{QTime::currentTime().toString(QLatin1String("hh:mm:ss")), snr, rssi, type, hops, bytes, route});
}

void RadioAirPanel::poll() {
    if (client_.connected() && !paused_ && (!lastReadings_.isValid() || lastReadings_.elapsed() >= 10000)) {
        client_.requestRadioReadings();
        lastReadings_.start();
    }
    if (pending_.isEmpty())
        return;
    const Appearance::Palette colors = Appearance::paletteFor(this);
    for (const Packet& packet : pending_) {
        const QString text = ui("%1 · %2\nSNR %3 дБ · RSSI %4 дБм\n%5")
            .arg(packetType(packet.type), packet.time, signedNumber(packet.snr / 4.0))
            .arg(packet.rssi).arg(passage(packet.route, packet.hops, packet.bytes));
        auto* row = new QListWidgetItem(text);
        row->setSizeHint(QSize(0, PacketHeight));
        row->setForeground(colors.text);
        packets_->insertItem(0, row);
        if (packets_->count() > PacketLimit)
            delete packets_->takeItem(PacketLimit);
    }
    pending_.clear();
    refreshView();
}

void RadioAirPanel::refreshView() {
    const bool online = client_.connected();
    const int total = packets_->count();
    QString text = paused_ ? ui("Пауза") : online ? ui("Слушаю эфир") : ui("Радио не подключено");
    const auto noise = client_.radioReadings().noiseDbm;
    if (online && noise)
        text += ui(" · шум %1 дБм").arg(*noise);
    text += ui(" · пакетов %1").arg(total);
    status_->setText(text);
    empty_->setVisible(total == 0);
    packets_->setVisible(total > 0);
    packets_->setFixedHeight(total * PacketHeight + 2);
    pause_->setEnabled(online || paused_);
    pause_->setText(paused_ ? ui("Продолжить") : ui("Пауза"));
    clear_->setEnabled(total > 0 || !pending_.isEmpty());
}

void RadioAirPanel::toggleListening() {
    paused_ = !paused_;
    if (!paused_)
        lastReadings_.invalidate();
    poll();
    refreshView();
}

void RadioAirPanel::clearLog() {
    pending_.clear();
    packets_->clear();
    refreshView();
}

RadioMaintenancePanel::RadioMaintenancePanel(Client& client, QWidget* parent) : QWidget(parent), client_(client) {
    setObjectName(QLatin1String("settingsRadioMaintenance"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(9, 7, 9, 10);
    page->setSpacing(8);
    status_ = label(QString(), this, "radioMaintenanceStatus");
    status_->hide();
    page->addWidget(status_);

    confirmation_ = new QWidget(this);
    confirmation_->setObjectName(QLatin1String("settingsGroup"));
    confirmation_->setAttribute(Qt::WA_StyledBackground);
    auto* prompt = new QVBoxLayout(confirmation_);
    prompt->setContentsMargins(8, 7, 8, 7);
    explanation_ = label(QString(), confirmation_, "subtitle");
    prompt->addWidget(explanation_);
    auto* actions = new QHBoxLayout;
    confirm_ = button(QString(), confirmation_, "radioMaintenanceConfirm");
    auto* cancelButton = button(ui("Отмена"), confirmation_, "radioMaintenanceCancel");
    actions->addWidget(cancelButton, 1);
    actions->addWidget(confirm_, 1);
    prompt->addLayout(actions);
    page->addWidget(confirmation_);
    confirmation_->hide();

    reboot_ = button(ui("Перезагрузить радио"), this, "radioMaintenanceReboot");
    reset_ = button(ui("Заводской сброс радио"), this, "radioMaintenanceReset");
    erase_ = button(ui("Стереть историю сообщений"), this, "radioMaintenanceErase");
    page->addWidget(reboot_);
    page->addWidget(reset_);
    page->addWidget(erase_);
    page->addStretch();
    const QString danger = QString::fromLatin1("QPushButton:enabled { color: %1; }")
        .arg(Appearance::paletteFor(this).danger.name());
    reset_->setStyleSheet(danger);
    erase_->setStyleSheet(danger);
    connect(reboot_, SIGNAL(clicked()), this, SLOT(askReboot()), Qt::QueuedConnection);
    connect(reset_, SIGNAL(clicked()), this, SLOT(askReset()), Qt::QueuedConnection);
    connect(erase_, SIGNAL(clicked()), this, SLOT(askErase()), Qt::QueuedConnection);
    connect(confirm_, SIGNAL(clicked()), this, SLOT(confirm()), Qt::QueuedConnection);
    connect(cancelButton, SIGNAL(clicked()), this, SLOT(cancel()), Qt::QueuedConnection);
    connect(&client_, SIGNAL(changed()), this, SLOT(updateAvailability()), Qt::QueuedConnection);
    updateAvailability();
}

void RadioMaintenancePanel::askReboot() { ask(Reboot); }
void RadioMaintenancePanel::askReset() { ask(FactoryReset); }
void RadioMaintenancePanel::askErase() { ask(EraseHistory); }

void RadioMaintenancePanel::ask(Action action) {
    if (!isVisible() || pending_ != None)
        return;
    const bool erase = action == EraseHistory;
    if (erase ? !client_.canEraseLocalHistory() : (!client_.connected() || client_.busy()))
        return;
    const QString key = erase ? client_.localHistoryRadioKey() : client_.selfNode().publicKey;
    if (key.isEmpty())
        return;
    targetKey_ = key;
    pending_ = action;
    status_->hide();
    if (action == Reboot) {
        explanation_->setText(ui("Перезагрузить «%1»? Связь с радио прервётся на время запуска.").arg(client_.radioName()));
        confirm_->setText(ui("Перезагрузить"));
    } else if (action == FactoryReset) {
        explanation_->setText(ui("Сбросить «%1»? Будут удалены контакты, каналы, настройки и ключи радио.").arg(client_.radioName()));
        confirm_->setText(ui("Стереть и сбросить"));
    } else {
        explanation_->setText(ui("Стереть историю сообщений этого радио на телефоне?"));
        confirm_->setText(ui("Стереть сообщения"));
    }
    confirmation_->show();
    updateAvailability();
}

void RadioMaintenancePanel::cancel() {
    pending_ = None;
    targetKey_.clear();
    confirmation_->hide();
    updateAvailability();
}

void RadioMaintenancePanel::confirm() {
    if (!isVisible() || !confirm_->isEnabled() || pending_ == None)
        return;
    bool success = false;
    const bool erase = pending_ == EraseHistory;
    if (erase)
        success = client_.eraseLocalHistory(targetKey_);
    else if (client_.connected() && client_.selfNode().publicKey == targetKey_)
        success = client_.restartRadio(pending_ == FactoryReset);
    cancel();
    status_->setText(erase ? (success ? ui("История сообщений на телефоне стёрта.") : ui("Не удалось стереть историю."))
                           : (success ? ui("Команда отправлена радио.") : ui("Команда не отправлена.")));
    status_->show();
}

void RadioMaintenancePanel::updateAvailability() {
    const bool idle = pending_ == None;
    const bool online = client_.connected() && !client_.busy();
    reboot_->setEnabled(idle && online);
    reset_->setEnabled(idle && online);
    erase_->setEnabled(idle && client_.canEraseLocalHistory());
    const bool sameRadio = !targetKey_.isEmpty() && client_.selfNode().publicKey == targetKey_;
    confirm_->setEnabled(!idle && (pending_ == EraseHistory
        ? client_.canEraseLocalHistory() && client_.localHistoryRadioKey() == targetKey_
        : online && sameRadio));
}

void RadioMaintenancePanel::hideEvent(QHideEvent* event) {
    if (!event->spontaneous())
        cancel();
    QWidget::hideEvent(event);
}
