#include "network_node_management.hpp"

#include "appearance.hpp"
#include "client.hpp"
#include "settings_icons.hpp"

#include <QtCore/QRegExp>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QCheckBox>
#include <QtGui/QComboBox>
#include <QtGui/QHBoxLayout>
#include <QtGui/QHideEvent>
#include <QtGui/QLabel>
#include <QtGui/QLineEdit>
#include <QtGui/QPainter>
#include <QtGui/QPlainTextEdit>
#include <QtGui/QPushButton>
#include <QtGui/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString ui(const char* text) { return QString::fromUtf8(text); }

QLabel* caption(const QString& text, QWidget* parent, const char* style = "muted") {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(style));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QPushButton* action(const QString& text, QWidget* parent, const char* name) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(name));
    button->setMinimumHeight(39);
    return button;
}

QComboBox* options(QWidget* parent, const char* name) {
    auto* combo = new QComboBox(parent);
    combo->setObjectName(QLatin1String(name));
    combo->setMinimumHeight(39);
    return combo;
}

class TouchSettingCheckBox : public QCheckBox {
public:
    TouchSettingCheckBox(const QString& title, QWidget* parent) : QCheckBox(title, parent) {
        setMinimumHeight(44);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    bool hitButton(const QPoint& position) const override {
        // The Belle style normally accepts only the native indicator and label rectangle.
        // Make the entire visible settings row respond to a tap.
        return rect().contains(position);
    }
};

void clearRows(QVBoxLayout* rows) {
    while (QLayoutItem* item = rows->takeAt(0)) {
        if (QWidget* entry = item->widget()) {
            entry->hide();
            entry->deleteLater();
        }
        delete item;
    }
}

QWidget* block(QWidget* parent, QVBoxLayout*& layout) {
    auto* widget = new QWidget(parent);
    widget->setObjectName(QLatin1String("settingsGroup"));
    widget->setAttribute(Qt::WA_StyledBackground);
    layout = new QVBoxLayout(widget);
    layout->setContentsMargins(9, 7, 9, 7);
    layout->setSpacing(5);
    return widget;
}

int fieldBytes(const QString& value) {
    return value.toUtf8().size();
}

bool cliError(const QString& reply) {
    return QRegExp(QLatin1String("^(err\\b|error|unknown|\\?\\?)|\\bnot supported$"),
                   Qt::CaseInsensitive).indexIn(reply.trimmed()) >= 0;
}

bool accepted(const QString& reply) {
    return QRegExp(QLatin1String("^ok\\b"), Qt::CaseInsensitive).indexIn(reply.trimmed()) == 0;
}

QString cliValue(QString reply) {
    while (!reply.isEmpty() && reply.at(reply.size() - 1).isSpace())
        reply.chop(1);
    if (reply.startsWith(QLatin1String("> ")))
        return reply.mid(2);
    if (reply == QLatin1String(">"))
        return QString();
    return reply.trimmed();
}

QString age(quint32 seconds) {
    if (seconds < 60)
        return ui("только что");
    if (seconds < 3600)
        return ui("%1 мин назад").arg(seconds / 60);
    if (seconds < 86400)
        return ui("%1 ч назад").arg(seconds / 3600);
    return ui("%1 дн. назад").arg(seconds / 86400);
}

QString shortId(const QString& key) { return key.left(12).toLower(); }

bool hexKey(const QString& key, int length) {
    return key.size() == length &&
           QRegExp(QLatin1String("^[0-9a-f]+$"), Qt::CaseInsensitive).exactMatch(key);
}

QString roleName(int role) {
    switch (role & 3) {
    case 3: return ui("Администратор");
    case 2: return ui("Чтение и запись");
    case 1: return ui("Только чтение");
    default: return ui("Гость");
    }
}

void roleChoices(QComboBox* roles) {
    for (int role = 3; role >= 1; --role)
        roles->addItem(roleName(role), role);
    roles->setCurrentIndex(1);
}

class SignalBar : public QWidget {
public:
    SignalBar(int quarterDb, QWidget* parent)
        : QWidget(parent), quarterDb_(quarterDb) {
        setFixedHeight(9);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto palette = Appearance::paletteFor(this);
        const int signal = qBound(-80, quarterDb_, 60) + 80;
        const QColor tint = quarterDb_ < -40 ? palette.danger : palette.accent;
        painter.fillRect(QRect(0, 2, width(), 5), palette.border);
        painter.fillRect(QRect(0, 2, signal * width() / 140, 5), tint);
        painter.setPen(palette.text);
        painter.drawLine(80 * width() / 140, 1, 80 * width() / 140, 7);
    }

private:
    int quarterDb_;
};

enum SettingGroup { Identity, Radio, Repeating, Guests, Power, Password };
enum FieldKind { Plain, Number, Decimal, Toggle, OwnerInfo, Secret };
struct SettingField {
    SettingGroup group;
    const char* key;
    const char* label;
    FieldKind kind;
    double low = 0.0;
    double high = 0.0;
};

const SettingField fields[] = {
    {Identity, "name", "Имя", Plain},
    {Identity, "lat", "Широта", Decimal, -90, 90},
    {Identity, "lon", "Долгота", Decimal, -180, 180},
    {Identity, "owner.info", "Сведения о владельце", OwnerInfo},
    {Radio, "radio", "Частота, полоса, SF, CR", Plain},
    {Radio, "tx", "Мощность, дБм", Number, -9, 30},
    {Repeating, "repeat", "Ретрансляция", Toggle},
    {Repeating, "flood.max", "Предельное число переходов", Number, 0, 64},
    {Repeating, "advert.interval", "Анонс соседям, мин", Number, 60, 240},
    {Repeating, "flood.advert.interval", "Анонс по сети, ч", Number, 0, 168},
    {Repeating, "txdelay", "Задержка отправки", Plain},
    {Repeating, "direct.txdelay", "Задержка по маршруту", Plain},
    {Repeating, "rxdelay", "Задержка приёма", Plain},
    {Repeating, "af", "Коэффициент эфирного времени", Plain},
    {Guests, "guest.password", "Пароль гостя", Secret},
    {Guests, "allow.read.only", "Разрешить вход для чтения", Toggle},
    {Power, "powersaving", "Энергосбережение", Toggle},
};

QString writeCommand(const SettingField& field, QString value) {
    if (field.kind == OwnerInfo)
        value.replace(QLatin1Char('\n'), QLatin1Char('|'));
    const QString key = QLatin1String(field.key);
    if (key == QLatin1String("powersaving"))
        return key + QLatin1Char(' ') + value;
    return QLatin1String("set ") + key + QLatin1Char(' ') + value;
}

bool singleLine(const QString& text) {
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.category() == QChar::Other_Control || c == QChar::LineSeparator || c == QChar::ParagraphSeparator)
            return false;
    }
    return true;
}

bool validValue(const SettingField& field, const QString& value) {
    const QString key = QLatin1String(field.key);
    if (key == QLatin1String("name") && value.trimmed().isEmpty())
        return false;
    if (field.kind == Secret && fieldBytes(value) > 15)
        return false;
    if (field.kind == Toggle && value != QLatin1String("on") && value != QLatin1String("off"))
        return false;
    if (field.kind == Number || field.kind == Decimal) {
        bool ok = false;
        const double number = field.kind == Number ? static_cast<double>(value.toInt(&ok)) : value.toDouble(&ok);
        const bool zeroAdvert = key == QLatin1String("advert.interval") && number == 0;
        if (!ok || !std::isfinite(number) || (!zeroAdvert && (number < field.low || number > field.high)))
            return false;
    }
    const QString command = writeCommand(field, value);
    return singleLine(command) && fieldBytes(command) <= 150;
}

QString formatRadio(const QString& value) {
    QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() != 4)
        return QString();
    for (int i = 0; i < parts.size(); ++i)
        parts[i] = parts.at(i).trimmed();
    bool frequencyOk = false;
    bool bandwidthOk = false;
    bool sfOk = false;
    bool crOk = false;
    const double frequency = parts.at(0).toDouble(&frequencyOk);
    const double bandwidth = parts.at(1).toDouble(&bandwidthOk);
    const int sf = parts.at(2).toInt(&sfOk);
    const int cr = parts.at(3).toInt(&crOk);
    if (!frequencyOk || !std::isfinite(frequency) || frequency < 150.0 || frequency > 2500.0
        || !bandwidthOk || !std::isfinite(bandwidth) || bandwidth < 7.8 || bandwidth > 500
        || !sfOk || sf < 5 || sf > 12 || !crOk || cr < 5 || cr > 8)
        return QString();
    parts[0] = QString::number(qRound(frequency * 1000.0) / 1000.0, 'g', 8);
    parts[1] = QString::number(qRound(bandwidth * 1000.0) / 1000.0, 'g', 8);
    parts[2] = QString::number(sf);
    parts[3] = QString::number(cr);
    return parts.join(QLatin1String(","));
}

}  // namespace

struct NetworkNodeManagementPanel::State {
    enum Command { NoCommand, ReadSetting, WriteSetting, TrialRadio, SaveRadio,
                   SetPermission, SetPassword, Discover, ResetNeighbours };
    struct Operation {
        Command kind;
        QString input;
        QString field;
        QString value;
    };

    State(NetworkNodeManagementPanel* owner, Client& client, const NetworkNode& node, Page page)
        : owner(owner), client(client), node(node), page(page) { }

    NetworkNodeManagementPanel* owner;
    Client& client;
    NetworkNode node;
    Page page;
    QWidget* body = nullptr;
    QLabel* status = nullptr;
    QPushButton* refresh = nullptr;
    QWidget* confirmation = nullptr;
    QLabel* confirmationText = nullptr;
    QVBoxLayout* rows = nullptr;
    QComboBox* sortOrder = nullptr;
    QPushButton* more = nullptr;
    QPushButton* search = nullptr;
    QPushButton* reset = nullptr;
    QTimer* discoveryTimer = nullptr;
    bool discovering = false;
    int totalNeighbours = -1;
    int nextOffset = 0;
    int pendingOffset = 0;
    QVector<NodeNeighbour> neighbours;
    QComboBox* group = nullptr;
    QPushButton* apply = nullptr;
    QPushButton* discard = nullptr;
    QPushButton* trial = nullptr;
    QPushButton* permanent = nullptr;
    QPushButton* passwordButton = nullptr;
    QLineEdit* password = nullptr;
    QLineEdit* frequency = nullptr;
    QComboBox* radioChoices[3] = {};
    QMap<QString, QString> values;
    QMap<QString, QString> draft;
    QStringList unknown;
    QComboBox* aclEntries = nullptr;
    QComboBox* aclRoles = nullptr;
    QComboBox* grantContact = nullptr;
    QComboBox* grantRoles = nullptr;
    QPushButton* aclChange = nullptr;
    QPushButton* aclRemove = nullptr;
    QPushButton* grant = nullptr;
    QVector<NodeAccessEntry> acl;
    bool aclLoaded = false;
    int requestId = 0;
    QVector<Operation> commands;
    Operation operation{NoCommand, QString(), QString(), QString()};
    Operation pendingConfirm{NoCommand, QString(), QString(), QString()};

    bool available() const {
        return page == Neighbours ? client.nodeSignedIn(node.id)
                                  : client.nodeConsoleAuthorized(node.id);
    }

    bool idle() const {
        return requestId == 0 && !discovering && commands.isEmpty() &&
               pendingConfirm.kind == NoCommand && owner->isEnabled();
    }

    void note(const QString& message) {
        status->setText(message);
    }

    NetworkNode byPrefix(const QString& prefix) const {
        if (prefix.isEmpty())
            return NetworkNode();
        NetworkNode match;
        const QVector<NetworkNode> contacts = client.nodes();
        for (const NetworkNode& contact : contacts) {
            if (shortId(contact.publicKey) == prefix) {
                if (!match.id.isEmpty())
                    return NetworkNode();
                match = contact;
            }
        }
        return match;
    }

    QString selfPrefix() const {
        return shortId(client.selfNode().publicKey);
    }

    int selectedGroup() const {
        return group->itemData(group->currentIndex()).toInt();
    }

    bool mayEdit(const NodeAccessEntry& entry) const {
        const QString own = selfPrefix();
        return !own.isEmpty() && entry.prefix != own && hexKey(entry.prefix, 12);
    }

    bool requestList(int offset) {
        if (!owner->isVisible() || !available())
            return false;
        pendingOffset = offset;
        requestId = client.nextConsoleRequestId();
        const bool sent = page == Neighbours
            ? client.requestNodeNeighbours(requestId, node.id, sortOrder->itemData(sortOrder->currentIndex()).toInt(), offset)
            : client.requestNodeAccess(requestId, node.id);
        if (!sent) {
            requestId = 0;
            note(ui("Радио занято. Повторите запрос."));
        } else {
            note(ui("Ожидание ответа узла…"));
        }
        owner->updateAvailability();
        return sent;
    }

    bool sendNext() {
        if (commands.isEmpty())
            return false;
        if (!owner->isVisible() || !client.nodeConsoleAuthorized(node.id)) {
            commands.clear();
            note(ui("Войдите с паролем администратора."));
            owner->updateAvailability();
            return false;
        }
        operation = commands.first();
        commands.remove(0);
        requestId = client.nextConsoleRequestId();
        const bool sent = client.runConsole(requestId, node.id, operation.input);
        if (operation.kind == SetPassword ||
            operation.field == QLatin1String("guest.password"))
            operation.input.fill(QLatin1Char('\0'));
        if (!sent) {
            requestId = 0;
            operation = Operation{NoCommand, QString(), QString(), QString()};
            commands.clear();
            note(ui("Радио занято. Повторите запрос."));
        } else {
            note(ui("Ожидание ответа узла…"));
        }
        owner->updateAvailability();
        return sent;
    }

    void send(const Operation& item) {
        commands.append(item);
        sendNext();
    }

    void confirm(const Operation& item, const QString& description) {
        pendingConfirm = item;
        confirmationText->setText(description);
        confirmation->show();
        owner->updateAvailability();
    }

    void showNeighbours() {
        clearRows(rows);
        const auto colors = Appearance::paletteFor(owner);
        for (const NodeNeighbour& neighbour : neighbours) {
            const NetworkNode known = byPrefix(neighbour.prefix);
            const QString name = known.title.isEmpty() ? neighbour.prefix : known.title;
            auto* item = new QWidget(body);
            item->setObjectName(QLatin1String("settingsGroup"));
            item->setAttribute(Qt::WA_StyledBackground);
            auto* layout = new QHBoxLayout(item);
            layout->setContentsMargins(8, 6, 8, 6);
            layout->setSpacing(5);
            auto* details = new QVBoxLayout;
            details->setSpacing(2);
            if (known.id.isEmpty()) {
                details->addWidget(caption(name, item, "subtitle"));
            } else {
                auto* open = action(name, item, "networkNeighbourOpen");
                open->setProperty("networkNodeId", known.id);
                open->setIcon(SettingsIcons::icon(SettingsIcons::Chevron, colors.muted, 16));
                open->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                open->setToolTip(name);
                connect(open, SIGNAL(clicked()), owner, SLOT(chooseNeighbour()), Qt::QueuedConnection);
                details->addWidget(open);
            }
            details->addWidget(caption(ui("%1 · %2").arg(neighbour.prefix, age(neighbour.heardSeconds)), item));
            layout->addLayout(details, 1);
            auto* signal = new QWidget(item);
            signal->setFixedWidth(70);
            auto* gauge = new QVBoxLayout(signal);
            gauge->setContentsMargins(1, 0, 1, 0);
            gauge->setSpacing(1);
            const QString db = (neighbour.snrQuarterDb > 0 ? QLatin1String("+") : QLatin1String("")) +
                               QString::number(neighbour.snrQuarterDb / 4.0, 'f', 2);
            auto* value = caption(ui("%1 дБ").arg(db), signal);
            value->setAlignment(Qt::AlignRight);
            gauge->addWidget(value);
            gauge->addWidget(new SignalBar(neighbour.snrQuarterDb, signal));
            layout->addWidget(signal);
            rows->addWidget(item);
        }
        if (totalNeighbours == 0)
            rows->addWidget(caption(ui("Список соседей пуст."), body));
    }

    void showAccess() {
        const QString previous = aclEntries->itemData(aclEntries->currentIndex()).toString();
        aclEntries->clear();
        grantContact->clear();
        grantContact->addItem(ui("Выбрать контакт…"), QString());
        QSet<QString> taken;
        const NetworkNode self = client.selfNode();
        for (const NodeAccessEntry& entry : acl) {
            const bool own = entry.prefix == shortId(self.publicKey);
            const NetworkNode known = own ? self : byPrefix(entry.prefix);
            QString name = known.title.isEmpty() ? entry.prefix : known.title;
            if (own)
                name += ui(" (вы)");
            aclEntries->addItem(ui("%1 · %2").arg(name, roleName(entry.permissions)), entry.prefix);
            taken.insert(entry.prefix);
        }
        const int current = aclEntries->findData(previous);
        aclEntries->setCurrentIndex(qMax(0, current));
        QVector<NetworkNode> contacts = client.nodes();
        std::sort(contacts.begin(), contacts.end(), [](const NetworkNode& a, const NetworkNode& b) {
            return QString::localeAwareCompare(a.title, b.title) < 0;
        });
        const QString own = shortId(self.publicKey);
        for (const NetworkNode& contact : contacts) {
            const QString prefix = shortId(contact.publicKey);
            if (contact.role == 1 && prefix != own && !taken.contains(prefix) && hexKey(contact.publicKey, 64))
                grantContact->addItem(contact.title.isEmpty() ? prefix : contact.title, contact.publicKey.toLower());
        }
        owner->chooseAccessRow(aclEntries->currentIndex());
    }

    void showRadio(const QString& value) {
        const QStringList parts = value.split(QLatin1Char(','));
        if (parts.size() != 4)
            return;
        rows->addWidget(caption(ui("Частота, МГц"), body, "subtitle"));
        frequency = new QLineEdit(parts.at(0).trimmed(), body);
        frequency->setObjectName(QLatin1String("networkRemoteRadioFrequency"));
        frequency->setProperty("settingName", QLatin1String("radio"));
        frequency->setMinimumHeight(38);
        rows->addWidget(frequency);
        connect(frequency, SIGNAL(textChanged(QString)), owner, SLOT(settingEdited()));
        const char* const titles[] = {"Полоса, кГц", "SF", "CR"};
        const char* const names[] = {"networkRemoteRadioBandwidth", "networkRemoteRadioSF", "networkRemoteRadioCR"};
        const char* const samples[] = {"7.8,10.4,15.6,20.8,31.25,41.7,62.5,125,250,500",
                                       "5,6,7,8,9,10,11,12", "5,6,7,8"};
        for (int i = 0; i < 3; ++i) {
            rows->addWidget(caption(ui(titles[i]), body, "subtitle"));
            QComboBox* select = radioChoices[i] = options(body, names[i]);
            select->setProperty("settingName", QLatin1String("radio"));
            select->addItems(QString::fromLatin1(samples[i]).split(QLatin1Char(',')));
            QString entry = parts.at(i + 1).trimmed();
            bool numeric = false;
            const double number = entry.toDouble(&numeric);
            if (numeric && std::isfinite(number))
                entry = QString::number(number);
            int chosen = select->findText(entry);
            if (chosen == -1) {
                select->addItem(entry);
                chosen = select->count() - 1;
            }
            select->setCurrentIndex(chosen);
            rows->addWidget(select);
            connect(select, SIGNAL(currentIndexChanged(int)), owner, SLOT(settingEdited()));
        }
    }

    void showSettings() {
        clearRows(rows);
        password = nullptr;
        frequency = nullptr;
        const int current = selectedGroup();
        if (current == Password) {
            rows->addWidget(caption(ui("Новый пароль администратора · до 15 байт"), body, "subtitle"));
            password = new QLineEdit(body);
            password->setEchoMode(QLineEdit::Password);
            password->setObjectName(QLatin1String("networkManagementPassword"));
            password->setMinimumHeight(38);
            rows->addWidget(password);
            connect(password, SIGNAL(textChanged(QString)), owner, SLOT(updateAvailability()));
            return;
        }
        bool shown = false;
        for (const SettingField& field : fields) {
            const QString key = QLatin1String(field.key);
            if (field.group != current || !values.contains(key))
                continue;
            shown = true;
            const QString value = draft.contains(key) ? draft.value(key) : values.value(key);
            const QString title = ui(field.label);
            if (key == QLatin1String("radio")) {
                showRadio(value);
                continue;
            }
            if (field.kind == Toggle) {
                auto* toggle = new TouchSettingCheckBox(title, body);
                toggle->setChecked(value == QLatin1String("on"));
                toggle->setProperty("settingName", key);
                toggle->setObjectName(key);
                rows->addWidget(toggle);
                connect(toggle, SIGNAL(toggled(bool)), owner, SLOT(settingEdited()));
                continue;
            }
            rows->addWidget(caption(title, body, "subtitle"));
            if (field.kind == OwnerInfo) {
                auto* edit = new QPlainTextEdit(value, body);
                edit->setFixedHeight(65);
                edit->setProperty("settingName", key);
                edit->setObjectName(key);
                rows->addWidget(edit);
                connect(edit, SIGNAL(textChanged()), owner, SLOT(settingEdited()));
            } else {
                auto* edit = new QLineEdit(value, body);
                edit->setMinimumHeight(38);
                edit->setProperty("settingName", key);
                edit->setObjectName(key);
                if (field.kind == Secret)
                    edit->setEchoMode(QLineEdit::Password);
                rows->addWidget(edit);
                connect(edit, SIGNAL(textChanged(QString)), owner, SLOT(settingEdited()));
            }
        }
        if (!shown)
            rows->addWidget(caption(ui("Запросите параметры этой группы у узла."), body));
    }
};

NetworkNodeManagementPanel::NetworkNodeManagementPanel(Client& client, const NetworkNode& node, Page page, QWidget* parent)
    : QWidget(parent), state_(new State(this, client, node, page)) {
    State& s = *state_;
    const char* names[] = {"networkNodeNeighbours", "networkNodeSettings", "networkNodeAccess"};
    setObjectName(QLatin1String(names[page]));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 1, 4, 6);
    layout->setSpacing(7);
    layout->addWidget(caption(node.title.isEmpty() ? shortId(node.publicKey) : node.title, this, "subtitle"));
    s.status = caption(QString(), this);
    s.status->setObjectName(QLatin1String("networkManagementStatus"));
    layout->addWidget(s.status);
    s.refresh = action(ui("Запросить"), this, "networkManagementRefresh");
    connect(s.refresh, SIGNAL(clicked()), this, SLOT(refresh()), Qt::QueuedConnection);
    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    if (page == Neighbours || page == Settings) {
        auto* group = options(this, page == Neighbours ? "networkNeighbourOrder" : "networkNodeSettingGroup");
        if (page == Neighbours) {
            s.sortOrder = group;
            group->addItem(ui("Недавние"), 0);
            group->addItem(ui("Лучший сигнал"), 2);
            group->addItem(ui("Слабый сигнал"), 3);
            group->addItem(ui("Давние"), 1);
        } else {
            s.group = group;
            group->addItem(ui("Имя и координаты"), Identity);
            group->addItem(ui("Радио"), Radio);
            if (node.role == 2)
                group->addItem(ui("Ретрансляция"), Repeating);
            if (node.role == 2 || node.role == 3)
                group->addItem(ui("Гости"), Guests);
            group->addItem(ui("Питание"), Power);
            group->addItem(ui("Пароль администратора"), Password);
        }
        connect(group, SIGNAL(activated(int)), this, SLOT(changeGroup(int)), Qt::QueuedConnection);
        header->addWidget(group, 1);
    }
    header->addWidget(s.refresh, page == Access ? 1 : 0);
    layout->addLayout(header);
    if (page == Neighbours) {
        auto* discovery = new QHBoxLayout;
        s.search = action(ui("Найти соседей"), this, "networkNeighbourSearch");
        s.reset = action(ui("Сбросить"), this, "networkNeighbourReset");
        discovery->addWidget(s.search, 1);
        discovery->addWidget(s.reset);
        layout->addLayout(discovery);
        connect(s.search, SIGNAL(clicked()), this, SLOT(searchNeighbours()), Qt::QueuedConnection);
        connect(s.reset, SIGNAL(clicked()), this, SLOT(resetNeighbours()), Qt::QueuedConnection);
        s.discoveryTimer = new QTimer(this);
        s.discoveryTimer->setSingleShot(true);
        connect(s.discoveryTimer, SIGNAL(timeout()), this, SLOT(finishNeighbourSearch()));
    }
    QVBoxLayout* confirmation = nullptr;
    s.confirmation = block(this, confirmation);
    s.confirmationText = caption(QString(), s.confirmation);
    confirmation->addWidget(s.confirmationText);
    auto* choices = new QHBoxLayout;
    auto* cancel = action(ui("Отмена"), s.confirmation, "networkManagementCancel");
    auto* confirm = action(ui("Подтвердить"), s.confirmation, "networkManagementConfirm");
    choices->addWidget(cancel);
    choices->addWidget(confirm);
    confirmation->addLayout(choices);
    layout->addWidget(s.confirmation);
    s.confirmation->hide();
    connect(cancel, SIGNAL(clicked()), this, SLOT(dismissAction()), Qt::QueuedConnection);
    connect(confirm, SIGNAL(clicked()), this, SLOT(confirmAction()), Qt::QueuedConnection);

    s.body = block(this, s.rows);
    layout->addWidget(s.body);
    if (page == Neighbours) {
        s.rows->addWidget(caption(ui("Запрос вернёт соседей, которых слышит этот репитер."), s.body));
        s.more = action(ui("Загрузить ещё"), this, "networkNeighbourMore");
        layout->addWidget(s.more);
        connect(s.more, SIGNAL(clicked()), this, SLOT(loadMore()), Qt::QueuedConnection);
        layout->addWidget(caption(ui("Шкала SNR от −20 до +15 дБ с отметкой нуля."), this));
    } else if (page == Settings) {
        auto* write = new QHBoxLayout;
        s.apply = action(ui("Сохранить"), this, "networkSettingsApply");
        s.discard = action(ui("Отменить правки"), this, "networkSettingsDiscard");
        write->addWidget(s.apply);
        write->addWidget(s.discard);
        layout->addLayout(write);
        s.trial = action(ui("Пробовать радио 10 мин"), this, "networkSettingsRadioTrial");
        s.permanent = action(ui("Сохранить радио постоянно"), this, "networkSettingsRadioPermanent");
        s.passwordButton = action(ui("Изменить пароль"), this, "networkSettingsChangePassword");
        layout->addWidget(s.trial);
        layout->addWidget(s.permanent);
        layout->addWidget(s.passwordButton);
        connect(s.apply, SIGNAL(clicked()), this, SLOT(applySettings()), Qt::QueuedConnection);
        connect(s.discard, SIGNAL(clicked()), this, SLOT(clearDraft()), Qt::QueuedConnection);
        connect(s.trial, SIGNAL(clicked()), this, SLOT(applySettings()), Qt::QueuedConnection);
        connect(s.permanent, SIGNAL(clicked()), this, SLOT(keepRadioSettings()), Qt::QueuedConnection);
        connect(s.passwordButton, SIGNAL(clicked()), this, SLOT(changePassword()), Qt::QueuedConnection);
        s.showSettings();
    } else {
        s.aclEntries = options(s.body, "networkAccessEntries");
        s.aclRoles = options(s.body, "networkAccessRole");
        s.aclChange = action(ui("Изменить роль"), s.body, "networkAccessChange");
        s.aclRemove = action(ui("Убрать"), s.body, "networkAccessRemove");
        s.rows->addWidget(caption(ui("Кто может войти"), s.body, "subtitle"));
        s.rows->addWidget(s.aclEntries);
        s.rows->addWidget(s.aclRoles);
        auto* entries = new QHBoxLayout;
        entries->addWidget(s.aclChange, 1);
        entries->addWidget(s.aclRemove);
        s.rows->addLayout(entries);
        s.rows->addWidget(caption(ui("Дать роль контакту"), s.body, "subtitle"));
        s.grantContact = options(s.body, "networkAccessContact");
        s.grantRoles = options(s.body, "networkAccessGrantRole");
        s.grant = action(ui("Дать роль"), s.body, "networkAccessGrant");
        s.rows->addWidget(s.grantContact);
        s.rows->addWidget(s.grantRoles);
        s.rows->addWidget(s.grant);
        roleChoices(s.grantRoles);
        connect(s.aclEntries, SIGNAL(currentIndexChanged(int)), this, SLOT(chooseAccessRow(int)));
        connect(s.aclRoles, SIGNAL(currentIndexChanged(int)), this, SLOT(updateAvailability()));
        connect(s.grantContact, SIGNAL(currentIndexChanged(int)), this, SLOT(updateAvailability()));
        connect(s.aclChange, SIGNAL(clicked()), this, SLOT(changeRole()), Qt::QueuedConnection);
        connect(s.aclRemove, SIGNAL(clicked()), this, SLOT(removeRole()), Qt::QueuedConnection);
        connect(s.grant, SIGNAL(clicked()), this, SLOT(grantRole()), Qt::QueuedConnection);
        s.showAccess();
    }
    layout->addStretch(1);
    connect(&client, SIGNAL(changed()), this, SLOT(updateAvailability()), Qt::QueuedConnection);
    connect(&client, SIGNAL(nodeManagementResult(int,int,bool,QByteArray,QString)),
            this, SLOT(onBinaryResult(int,int,bool,QByteArray,QString)), Qt::QueuedConnection);
    connect(&client, SIGNAL(consoleResult(int,int,bool,QString)),
            this, SLOT(onConsoleResult(int,int,bool,QString)), Qt::QueuedConnection);
    if (s.available())
        s.note(ui("Запросы выполняются через радио."));
    else
        s.note(page == Neighbours ? ui("Сначала войдите в узел.") : ui("Войдите с паролем администратора."));
    updateAvailability();
}

NetworkNodeManagementPanel::~NetworkNodeManagementPanel() = default;

void NetworkNodeManagementPanel::hideEvent(QHideEvent* event) {
    State& s = *state_;
    if (!event->spontaneous()) {
        s.requestId = 0;
        s.commands.clear();
        s.discovering = false;
        if (s.discoveryTimer)
            s.discoveryTimer->stop();
        s.pendingConfirm.input.fill(QLatin1Char('\0'));
        s.pendingConfirm.kind = State::NoCommand;
        s.confirmation->hide();
        if (s.password)
            s.password->clear();
    }
    QWidget::hideEvent(event);
}

void NetworkNodeManagementPanel::refresh() {
    State& s = *state_;
    if (!s.idle() || !s.available())
        return;
    if (s.page != Settings) {
        s.requestList(0);
        return;
    }
    const int group = s.selectedGroup();
    if (group == Password)
        return;
    s.unknown.clear();
    for (const SettingField& field : fields) {
        if (field.group != group)
            continue;
        const QString key = QLatin1String(field.key);
        const QString command = key == QLatin1String("powersaving") ? key : QLatin1String("get ") + key;
        s.commands.append({State::ReadSetting, command, key, QString()});
    }
    s.sendNext();
}

void NetworkNodeManagementPanel::loadMore() {
    State& s = *state_;
    if (s.idle() && s.nextOffset > 0 && s.nextOffset < s.totalNeighbours)
        s.requestList(s.nextOffset);
}

void NetworkNodeManagementPanel::changeGroup(int) {
    State& s = *state_;
    if (s.page == Neighbours) {
        if (s.idle()) {
            s.totalNeighbours = -1;
            s.nextOffset = 0;
            s.neighbours.clear();
            s.showNeighbours();
            s.requestList(0);
        }
    } else if (s.page == Settings) {
        s.showSettings();
        s.note(ui("Запросите параметры или сохраните изменения."));
    }
    updateAvailability();
}

void NetworkNodeManagementPanel::chooseNeighbour() {
    if (isVisible() && sender())
        emit openNode(sender()->property("networkNodeId").toString());
}

void NetworkNodeManagementPanel::searchNeighbours() {
    State& s = *state_;
    if (s.idle() && s.client.nodeConsoleAuthorized(s.node.id))
        s.send({State::Discover, QLatin1String("discover.neighbors"), QString(), QString()});
}

void NetworkNodeManagementPanel::resetNeighbours() {
    State& s = *state_;
    if (s.idle() && s.client.nodeConsoleAuthorized(s.node.id)) {
        // Firmware uses an empty prefix after this space to match all neighbours.
        s.confirm({State::ResetNeighbours, QLatin1String("neighbor.remove "), QString(), QString()},
                  ui("Узел очистит список соседей и запросит их снова. Сбросить список?"));
    }
}

void NetworkNodeManagementPanel::finishNeighbourSearch() {
    State& s = *state_;
    s.discovering = false;
    s.sortOrder->setCurrentIndex(0);
    if (!s.requestList(0)) {
        s.note(ui("Не удалось прочитать список после поиска."));
        updateAvailability();
    }
}

void NetworkNodeManagementPanel::settingEdited() {
    State& s = *state_;
    QObject* editor = sender();
    const QString key = editor->property("settingName").toString();
    QString value;
    if (key == QLatin1String("radio") && s.frequency) {
        QStringList parts;
        parts.append(s.frequency->text());
        for (const QComboBox* select : s.radioChoices)
            parts.append(select->currentText());
        value = parts.join(QLatin1String(","));
    } else if (auto* line = qobject_cast<QLineEdit*>(editor))
        value = line->text();
    else if (auto* text = qobject_cast<QPlainTextEdit*>(editor))
        value = text->toPlainText();
    else if (auto* toggle = qobject_cast<QCheckBox*>(editor))
        value = toggle->isChecked() ? QLatin1String("on") : QLatin1String("off");
    if (s.values.value(key) == value)
        s.draft.remove(key);
    else
        s.draft.insert(key, value);
    updateAvailability();
}

void NetworkNodeManagementPanel::clearDraft() {
    State& s = *state_;
    if (!s.idle())
        return;
    s.draft.clear();
    s.showSettings();
    s.note(ui("Правки отменены."));
    updateAvailability();
}

void NetworkNodeManagementPanel::applySettings() {
    State& s = *state_;
    if (!s.idle() || !s.available())
        return;
    if (sender() == s.trial) {
        const QString value = formatRadio(s.draft.value(QLatin1String("radio")));
        if (!value.isEmpty())
            s.send({State::TrialRadio, QLatin1String("tempradio ") + value + QLatin1String(",10"), QString(), QString()});
        return;
    }
    QVector<State::Operation> changes;
    const int group = s.selectedGroup();
    for (const SettingField& field : fields) {
        const QString key = QLatin1String(field.key);
        if (field.group != group || key == QLatin1String("radio") || !s.draft.contains(key))
            continue;
        const QString value = s.draft.value(key);
        if (!validValue(field, value)) {
            s.note(ui("Проверьте значение «%1».").arg(ui(field.label)));
            return;
        }
        changes.append({State::WriteSetting, writeCommand(field, value), key, value});
    }
    if (!changes.isEmpty()) {
        s.commands = changes;
        s.sendNext();
    }
}

void NetworkNodeManagementPanel::keepRadioSettings() {
    State& s = *state_;
    const QString key = QLatin1String("radio");
    const QString value = formatRadio(s.draft.value(key));
    if (s.idle() && s.available() && !value.isEmpty()) {
        s.confirm({State::SaveRadio, QLatin1String("set radio ") + value, key, value},
                  ui("После перезапуска узел будет работать с новыми параметрами. Для связи с ним потребуется настроить своё радио на них."));
    }
}

void NetworkNodeManagementPanel::changePassword() {
    State& s = *state_;
    if (!s.password || !s.idle() || !s.available())
        return;
    const QString value = s.password->text();
    if (value.isEmpty() || fieldBytes(value) > 15 || !singleLine(value))
        return;
    s.confirm({State::SetPassword, QLatin1String("password ") + value, QString(), QString()},
              ui("Старый пароль администратора перестанет действовать. Изменить пароль?"));
}

void NetworkNodeManagementPanel::chooseAccessRow(int index) {
    State& s = *state_;
    if (!s.aclRoles)
        return;
    s.aclRoles->blockSignals(true);
    s.aclRoles->clear();
    if (index >= 0 && index < s.acl.size()) {
        const int role = s.acl.at(index).permissions & 3;
        roleChoices(s.aclRoles);
        if (role == 0)
            s.aclRoles->addItem(roleName(role), role);
        s.aclRoles->setCurrentIndex(s.aclRoles->findData(role));
    }
    s.aclRoles->blockSignals(false);
    updateAvailability();
}

void NetworkNodeManagementPanel::grantRole() {
    State& s = *state_;
    if (!s.idle() || !s.available() || !s.aclLoaded)
        return;
    const QString key = s.grantContact->itemData(s.grantContact->currentIndex()).toString();
    const int role = s.grantRoles->itemData(s.grantRoles->currentIndex()).toInt();
    if (!hexKey(key, 64) || shortId(key) == s.selfPrefix() || s.selfPrefix().isEmpty() || role < 1 || role > 3)
        return;
    s.send({State::SetPermission, QLatin1String("setperm ") + key + QLatin1Char(' ') + QString::number(role),
            QString(), QString()});
}

void NetworkNodeManagementPanel::changeRole() {
    State& s = *state_;
    const int index = s.aclEntries->currentIndex();
    if (!s.idle() || !s.available() || index < 0 || index >= s.acl.size())
        return;
    const NodeAccessEntry& entry = s.acl.at(index);
    const QString key = s.byPrefix(entry.prefix).publicKey;
    const int role = s.aclRoles->itemData(s.aclRoles->currentIndex()).toInt();
    if (!s.mayEdit(entry) || !hexKey(key, 64) || role < 1 || role > 3 || role == (entry.permissions & 3))
        return;
    const int permissions = (entry.permissions & ~3) | role;
    s.send({State::SetPermission, QLatin1String("setperm ") + key + QLatin1Char(' ') + QString::number(permissions),
            QString(), QString()});
}

void NetworkNodeManagementPanel::removeRole() {
    State& s = *state_;
    const int index = s.aclEntries->currentIndex();
    if (!s.idle() || !s.available() || index < 0 || index >= s.acl.size())
        return;
    const NodeAccessEntry& entry = s.acl.at(index);
    if (!s.mayEdit(entry))
        return;
    s.confirm({State::SetPermission, QLatin1String("setperm ") + entry.prefix + QLatin1String(" 0"),
               QString(), QString()}, ui("Клиент потеряет роль на этом узле. Убрать из списка?"));
}

void NetworkNodeManagementPanel::confirmAction() {
    State& s = *state_;
    if (s.pendingConfirm.kind == State::NoCommand)
        return;
    State::Operation command = std::move(s.pendingConfirm);
    s.pendingConfirm = State::Operation{State::NoCommand, QString(), QString(), QString()};
    s.confirmation->hide();
    if (s.idle() && s.available())
        s.send(command);
    updateAvailability();
}

void NetworkNodeManagementPanel::dismissAction() {
    State& s = *state_;
    s.pendingConfirm.input.fill(QLatin1Char('\0'));
    s.pendingConfirm = State::Operation{State::NoCommand, QString(), QString(), QString()};
    s.confirmation->hide();
    updateAvailability();
}

void NetworkNodeManagementPanel::updateAvailability() {
    State& s = *state_;
    const bool ready = s.available() && s.idle();
    s.refresh->setEnabled(ready);
    if (s.page == Neighbours) {
        s.sortOrder->setEnabled(ready);
        s.more->setVisible(s.nextOffset > 0 && s.nextOffset < s.totalNeighbours);
        s.more->setEnabled(ready);
        const bool admin = s.client.nodeConsoleAuthorized(s.node.id);
        s.search->setVisible(admin);
        s.reset->setVisible(admin);
        s.search->setEnabled(ready);
        s.reset->setEnabled(ready);
        return;
    }
    s.body->setEnabled(ready);
    if (s.page == Settings) {
        s.group->setEnabled(ready);
        const int group = s.selectedGroup();
        s.refresh->setVisible(group != Password);
        bool pending = false;
        for (const SettingField& field : fields) {
            const QString key = QLatin1String(field.key);
            if (field.group == group && key != QLatin1String("radio") && s.draft.contains(key))
                pending = true;
        }
        s.apply->setVisible(group != Password && pending);
        s.apply->setEnabled(ready);
        s.discard->setVisible(group != Password && !s.draft.isEmpty());
        s.discard->setEnabled(ready);
        const QString radio = s.draft.value(QLatin1String("radio"));
        const bool radioPending = group == Radio && s.draft.contains(QLatin1String("radio"));
        const bool radioValid = radioPending && !formatRadio(radio).isEmpty();
        s.trial->setVisible(radioPending);
        s.trial->setEnabled(ready && radioValid);
        s.permanent->setVisible(radioPending);
        s.permanent->setEnabled(ready && radioValid);
        s.passwordButton->setVisible(group == Password);
        const QString password = s.password ? s.password->text() : QString();
        s.passwordButton->setEnabled(ready && !password.isEmpty() && fieldBytes(password) <= 15 && singleLine(password));
    } else {
        s.body->setVisible(s.aclLoaded);
        const int index = s.aclEntries->currentIndex();
        const bool selected = index >= 0 && index < s.acl.size();
        const bool mayEdit = ready && selected && s.mayEdit(s.acl.at(index));
        const bool fullKey = mayEdit && hexKey(s.byPrefix(s.acl.at(index).prefix).publicKey, 64);
        s.aclRoles->setEnabled(fullKey);
        s.aclChange->setEnabled(fullKey && (s.acl.at(index).permissions & 3) !=
                                s.aclRoles->itemData(s.aclRoles->currentIndex()).toInt());
        s.aclRemove->setEnabled(mayEdit);
        s.grant->setEnabled(ready && s.grantContact->currentIndex() > 0 && !s.selfPrefix().isEmpty());
    }
}

void NetworkNodeManagementPanel::onBinaryResult(int requestId, int operation, bool success,
                                               const QByteArray& data, const QString& error) {
    State& s = *state_;
    if (!isVisible() || requestId != s.requestId || s.requestId == 0)
        return;
    if ((s.page == Neighbours && operation != Client::NodeNeighbours) ||
        (s.page == Access && operation != Client::NodeAccess) || s.page == Settings)
        return;
    s.requestId = 0;
    if (!success) {
        s.note(error.isEmpty() ? ui("Узел не ответил.") : error);
    } else if (s.page == Neighbours) {
        const auto parsed = Client::parseNodeNeighbours(data);
        if (!parsed) {
            s.note(ui("Узел прислал неверный список соседей."));
            updateAvailability();
            return;
        }
        if (s.pendingOffset == 0)
            s.neighbours.clear();
        QSet<QString> prefixes;
        for (const NodeNeighbour& row : s.neighbours)
            prefixes.insert(row.prefix);
        for (const NodeNeighbour& row : parsed->rows) {
            if (!prefixes.contains(row.prefix)) {
                prefixes.insert(row.prefix);
                s.neighbours.append(row);
            }
        }
        s.totalNeighbours = parsed->total;
        s.nextOffset = parsed->rows.isEmpty() ? 0 : s.pendingOffset + parsed->rows.size();
        s.note(ui("Соседи · %1 из %2").arg(s.neighbours.size()).arg(s.totalNeighbours));
        s.showNeighbours();
    } else {
        s.acl = Client::parseNodeAccess(data);
        s.aclLoaded = true;
        s.showAccess();
        s.note(s.acl.isEmpty() ? ui("Записей нет. Доступны гостевые роли.")
                              : ui("Список доступа · %1").arg(s.acl.size()));
    }
    s.refresh->setText(ui("Обновить"));
    updateAvailability();
}

void NetworkNodeManagementPanel::onConsoleResult(int requestId, int, bool success, const QString& reply) {
    State& s = *state_;
    if (!isVisible() || requestId != s.requestId || s.requestId == 0 || s.operation.kind == State::NoCommand)
        return;
    s.requestId = 0;
    const State::Operation op = s.operation;
    s.operation = State::Operation{State::NoCommand, QString(), QString(), QString()};
    const bool refused = success && cliError(reply);
    if (!success || refused) {
        if (refused && op.kind == State::ReadSetting) {
            s.unknown.append(op.field);
        } else {
            s.commands.clear();
            const bool secret = op.kind == State::SetPassword || op.field == QLatin1String("guest.password");
            s.note(secret ? ui("Не удалось изменить пароль.") :
                   reply.isEmpty() ? ui("Узел не ответил.") : ui("Ответ узла · %1").arg(reply));
            if (s.page == Settings && s.selectedGroup() != Password)
                s.showSettings();
            updateAvailability();
            return;
        }
    } else {
        switch (op.kind) {
        case State::ReadSetting: {
            QString value = cliValue(reply);
            if (op.field == QLatin1String("owner.info"))
                value.replace(QLatin1Char('|'), QLatin1Char('\n'));
            const auto field = std::find_if(std::begin(fields), std::end(fields), [&op](const SettingField& item) {
                return op.field == QLatin1String(item.key);
            });
            if (field != std::end(fields) && field->kind == Toggle)
                value = value.toLower();
            const bool radio = op.field == QLatin1String("radio");
            if (radio)
                value = formatRadio(value);
            if (field != std::end(fields) && (!radio || !value.isEmpty()) &&
                (field->kind != Toggle || value == QLatin1String("on") || value == QLatin1String("off"))) {
                s.values.insert(op.field, value);
                if (s.draft.contains(op.field) && s.draft.value(op.field) == value)
                    s.draft.remove(op.field);
            } else {
                s.unknown.append(op.field);
            }
            break;
        }
        case State::WriteSetting:
        case State::SaveRadio:
            s.values.insert(op.field, op.value);
            s.draft.remove(op.field);
            break;
        case State::TrialRadio:
            s.note(ui("Временные параметры отправлены на 10 минут."));
            updateAvailability();
            return;
        case State::SetPermission:
            if (!s.requestList(0))
                s.note(ui("Роль изменена. Обновите список доступа."));
            updateAvailability();
            return;
        case State::SetPassword:
            if (s.password)
                s.password->clear();
            s.note(ui("Пароль изменён."));
            updateAvailability();
            return;
        case State::ResetNeighbours:
            if (accepted(reply)) {
                s.neighbours.clear();
                s.totalNeighbours = 0;
                s.nextOffset = 0;
                s.showNeighbours();
                s.send({State::Discover, QLatin1String("discover.neighbors"), QString(), QString()});
                return;
            }
            s.note(ui("Узел не подтвердил сброс соседей."));
            updateAvailability();
            return;
        case State::Discover:
            if (accepted(reply)) {
                s.discovering = true;
                s.discoveryTimer->start(12000);
                s.note(ui("Репитер ищет соседей · ожидаем 12 секунд."));
            } else {
                s.note(ui("Узел не подтвердил поиск соседей."));
            }
            updateAvailability();
            return;
        case State::NoCommand:
            break;
        }
    }
    if (!s.commands.isEmpty()) {
        s.sendNext();
        return;
    }
    if (s.page == Settings) {
        s.showSettings();
        if (op.kind == State::ReadSetting) {
            s.note(s.unknown.isEmpty() ? ui("Параметры получены.") :
                   ui("Узел не поддерживает · %1").arg(s.unknown.join(QLatin1String(", "))));
        } else {
            s.note(ui("Параметры сохранены на узле."));
        }
    }
    updateAvailability();
}
