#include "client.hpp"
#include "radio_key_identity.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#ifdef Q_OS_SYMBIAN
#include <QtNetwork/QNetworkProxy>
#endif
#include <QtNetwork/QTcpSocket>
#ifdef Q_OS_SYMBIAN
#include "native_wlan.hpp"
#endif

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr qint64 kConnectTimeout = 15000;
constexpr qint64 kReplyTimeout = 12000;
constexpr qint64 kPollInterval = 5000;
constexpr qint64 kInventoryRefreshInterval = 10000;
constexpr qint64 kInitialReconnectDelay = 1000;
constexpr qint64 kMaximumReconnectDelay = 30000;
constexpr qint64 kStableLink = 30000;
constexpr qint64 kBackupReconnectWindow = 90000;
constexpr std::uint8_t kExportPrivateKey = 23;
constexpr std::uint8_t kImportPrivateKey = 24;
constexpr std::uint8_t kPrivateKeyReply = 0x0e;
constexpr std::uint8_t kCommandDisabled = 0x0f;
constexpr int kPrivateKeyBytes = 64;
constexpr qint64 kRetryGaps[] = {0, 0, 45000, 120000, 300000, 720000, 720000, 720000};
constexpr int kHistoryLimit = 400;
constexpr int kContactLimit = 4096;
constexpr int kEarlyAckLimit = 32;
constexpr int kDirectAckLimit = 4;
constexpr std::size_t kSendQueueLimit = 32;
constexpr std::size_t kEchoLimit = 24;
constexpr int kRepeatersLimit = 256;
constexpr int kRouteNameLimit = 24;
constexpr qint64 kHeardLifetime = 45000;
constexpr qint64 kEchoLifetime = 900000;
constexpr std::uint8_t kTrustedContactFlags = 0x0e;
constexpr std::uint8_t kStatsReply = 24;
constexpr std::uint8_t kTelemetryReply = 0x8b;
constexpr std::uint8_t kGetCustomVars = 40;
constexpr std::uint8_t kCustomVarsReply = 21;
constexpr qint64 kRadioGpsReplyTimeout = 2500;
constexpr int kMissingSnr = std::numeric_limits<int>::min();
constexpr int kMessageReactionLimit = 32;
constexpr std::size_t kDeferredReactionLimit = 64;
constexpr qint64 kDeferredReactionLifetime = 300000;

QString russian(const char* utf8) {
    return QString::fromUtf8(utf8);
}

QString fromWire(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size()));
}

template <class ByteArray>
QByteArray rawBytes(const ByteArray& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()));
}

RadioGpsState parseRadioGps(const meshcore::Bytes& frame) {
    auto state = RadioGpsState::Unsupported;
    std::string_view rest(reinterpret_cast<const char*>(frame.data() + 1), frame.size() - 1);
    while (!rest.empty()) {
        const auto separator = rest.find(',');
        const auto variable = rest.substr(0, separator);
        if (variable == "gps:1")
            return RadioGpsState::Enabled;
        if (variable == "gps:0" && state == RadioGpsState::Unsupported)
            state = RadioGpsState::Disabled;
        else if (variable.starts_with("gps:") && variable != "gps:0")
            state = RadioGpsState::Unknown;
        if (separator == std::string_view::npos)
            break;
        rest.remove_prefix(separator + 1);
    }
    return state;
}

void appendWireU32(meshcore::Bytes& bytes, quint32 value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool isRadioSettingsWrite(const meshcore::Bytes& command) {
    if (command.empty())
        return false;
    const auto bytes = command.size();
    switch (command.front()) {
    case 6: return bytes == 5;       // SetDeviceTime
    case 8: return bytes >= 2 && bytes <= 32; // SetAdvertName
    case 9: return bytes == 144 || bytes == 148; // AddUpdateContact
    case 11: return bytes == 11 || bytes == 12; // SetRadioParams
    case 12: return bytes == 2;      // SetRadioTxPower
    case 14: return bytes == 9;      // SetAdvertLatLon
    case 15: return bytes == 33;     // RemoveContact
    case 37: return bytes == 5;      // SetDevicePin
    case 38: return bytes >= 2 && bytes <= 5; // SetOtherParams
    case 58: return bytes == 2 || bytes == 3; // SetAutoAddConfig
    case 61: return bytes == 3;     // SetPathHashMode
    default: return false;
    }
}

bool validRadioBackupKey(const QString& publicKey) {
    const auto hex = publicKey.toLatin1();
    const auto key = QByteArray::fromHex(hex);
    return key.size() == 32 && key != QByteArray(32, '\0') && key.toHex() == hex;
}

quint32 radioBackupU32(const meshcore::Bytes& bytes, std::size_t offset) {
    quint32 value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<quint32>(bytes[offset + i]) << (8 * i);
    return value;
}

bool radioBackupCoordinates(qint32 latitude, qint32 longitude) {
    return latitude >= -90000000 && latitude <= 90000000 &&
           longitude >= -180000000 && longitude <= 180000000;
}

bool validRadioBackupSetting(const meshcore::Bytes& command) {
    if (!isRadioSettingsWrite(command))
        return false;
    switch (command.front()) {
    case 8: {
        const auto bytes = rawBytes(command).mid(1);
        return !bytes.contains('\0') && QString::fromUtf8(bytes.constData(), bytes.size()).toUtf8() == bytes;
    }
    case 11: {
        const auto frequency = radioBackupU32(command, 1);
        const auto bandwidth = radioBackupU32(command, 5);
        return frequency >= 150000 && frequency <= 2500000 && bandwidth >= 7000 && bandwidth <= 500000 &&
               command[9] >= 5 && command[9] <= 12 && command[10] >= 5 && command[10] <= 8 &&
               (command.size() == 11 || command[11] <= 1);
    }
    case 12: return static_cast<qint8>(command[1]) >= -9;
    case 14: return radioBackupCoordinates(static_cast<qint32>(radioBackupU32(command, 1)),
                                          static_cast<qint32>(radioBackupU32(command, 5)));
    case 38:
    case 58: return true;
    case 61: return command[1] == 0 && command[2] < 3;
    default: return false;
    }
}

meshcore::Bytes addUpdateContactCommand(const meshcore::Contact& contact, std::uint8_t flags) {
    meshcore::Bytes command{9};
    command.insert(command.end(), contact.key.begin(), contact.key.end());
    command.insert(command.end(), {contact.type, flags, contact.path});
    command.insert(command.end(), contact.pathHashes.begin(), contact.pathHashes.end());
    const auto nameBytes = std::min<std::size_t>(32, contact.name.size());
    command.insert(command.end(), contact.name.begin(), contact.name.begin() + nameBytes);
    command.resize(132, 0);
    appendWireU32(command, contact.lastAdvert);
    appendWireU32(command, static_cast<quint32>(contact.latitude));
    appendWireU32(command, static_cast<quint32>(contact.longitude));
    return command;
}

QString radioBackupStage(std::uint8_t command) {
    if (command == kImportPrivateKey)
        return russian("Ключ рации");
    if (command == 32)
        return russian("Каналы");
    if (command == 9)
        return russian("Контакты");
    return russian("Настройки рации");
}

QString directId(const meshcore::Prefix& prefix) {
    return QString::fromLatin1("d/") + QString::fromLatin1(rawBytes(prefix).toHex().constData());
}

QString channelId(const meshcore::Channel& channel) {
    const QByteArray identity =
        QCryptographicHash::hash(rawBytes(channel.key), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1("c/") + QString::number(channel.index) +
           QLatin1Char('/') + QString::fromLatin1(identity.constData());
}

QString unknownChannelId(std::uint8_t index) {
    return QString::fromLatin1("c/") + QString::number(index);
}

bool canChat(const meshcore::Contact& contact) {
    return contact.type == 1 || contact.type == 3;
}

bool hasNodeManagement(const meshcore::Contact& contact) {
    return contact.type == 2 || contact.type == 3 || contact.type == 4;
}

bool supportsManagementOperation(const meshcore::Contact& contact, int operation) {
    if (operation == Client::NodeNeighbours || operation == Client::NodeRouteTrace)
        return contact.type == 2;
    if (operation == meshcore::RadioConsole::Login || operation == meshcore::RadioConsole::Remote ||
        operation == Client::NodeAccess)
        return hasNodeManagement(contact);
    return true;
}

QString channelBlockId(const QString& author) {
    QString name = author.trimmed().toLower();
    if (name.isEmpty())
        return QString();
    const QString cyrillic = russian("аеёорсхіу");
    const QString latin = QLatin1String("aeeopcxiy");
    for (int i = 0; i < name.size(); ++i) {
        const int twin = cyrillic.indexOf(name.at(i));
        if (twin >= 0)
            name[i] = latin.at(twin);
    }
    return QLatin1String("n:") + name;
}

QString senderBlockId(const QString& target, const ChatMessage& message) {
    if (message.outgoing)
        return QString();
    if (target.startsWith(QLatin1String("c/")))
        return channelBlockId(message.author);
    return message.roomSigner.isEmpty() ? QString() : QLatin1String("k:") + message.roomSigner;
}

QString accepted() { return russian("Принято рацией"); }
QString waitingForAck() { return russian("Ждёт подтверждения"); }
QString delivered() { return russian("Доставлено"); }
QString unconfirmed() { return russian("Нет подтверждения"); }
QString resultUnknown() { return russian("Итог неизвестен"); }
QString idleStatus() { return russian("Подключено"); }

int checkedSendTries(int value) {
    return std::clamp(value, Client::MinimumSendTries, Client::MaximumSendTries);
}

qint64 ackWait(const meshcore::Bytes& frame) {
    const auto estimate = meshcore::parseSentTimeout(frame).value_or(0);
    return std::clamp<quint32>(estimate, 4000, 180000) * 3 / 2;
}

QString decimalMilli(quint32 millivalue) {
    return QString::number(millivalue / 1000) + QLatin1Char(',')
        + QString::number(millivalue % 1000).rightJustified(3, QLatin1Char('0'));
}

void checkPosition(qint32& latitude, qint32& longitude) {
    if (latitude < -90000000 || latitude > 90000000 || longitude < -180000000 || longitude > 180000000) {
        latitude = 0;
        longitude = 0;
    }
}

QString snrText(int quarters) {
    const int absolute = quarters < 0 ? -quarters : quarters;
    const char* fractions[] = {"", ",25", ",5", ",75"};
    const QString sign = quarters < 0 ? QLatin1String("-") : quarters > 0 ? QLatin1String("+") : QLatin1String("");
    return sign + QString::number(absolute / 4) + QLatin1String(fractions[absolute % 4]) + russian(" дБ");
}

QString hashText(const meshcore::Bytes& path, size_t offset, size_t width) {
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(path.data() + offset),
                                        static_cast<int>(width)).toHex().constData());
}

quint8 wireByte(const QByteArray& data, int offset) {
    return static_cast<quint8>(data.constData()[offset]);
}

quint32 littleEndian(const QByteArray& data, int offset, int size) {
    quint32 result = 0;
    for (int i = 0; i < size; ++i)
        result |= quint32(wireByte(data, offset + i)) << (8 * i);
    return result;
}

int telemetryFieldWidth(quint8 kind) {
    switch (kind) {
    case 0x00: case 0x01: case 0x66: case 0x68: case 0x78: case 0x8e: return 1;
    case 0x02: case 0x03: case 0x65: case 0x67: case 0x73: case 0x74: case 0x75:
    case 0x79: case 0x7d: case 0x80: case 0x84: return 2;
    case 0x87: return 3;
    case 0x64: case 0x76: case 0x82: case 0x83: case 0x85: return 4;
    case 0x71: case 0x86: return 6;
    case 0x88: return 9;
    default: return 0;
    }
}

struct TracePath {
    meshcore::Bytes hashes;
    std::uint8_t flags;
};

std::optional<TracePath> repeaterTracePath(const meshcore::Contact* node, int hashMode, bool direct) {
    if (!node || node->type != 2 || (node->path == 0xff && !direct))
        return std::nullopt;
    const int storedHops = node->path == 0xff ? 0 : node->path & 63;
    const int hops = direct ? 0 : storedHops;
    const int storedWidth = (storedHops ? node->path >> 6 : hashMode) + 1;
    if (storedWidth < 1 || storedWidth > 4 ||
        static_cast<std::size_t>(hops * storedWidth) > node->pathHashes.size())
        return std::nullopt;
    // Trace flags code hash widths of one, two or four bytes, so use two bytes for a three-byte route.
    const std::uint8_t flags = storedWidth == 4 ? 2 : storedWidth >= 2 ? 1 : 0;
    const int width = 1 << flags;
    const int roundTripHops = 2 * hops + 1;
    if (roundTripHops > 64 ||
        static_cast<std::size_t>(13 + roundTripHops * (width + 1)) > meshcore::MaxFrameSize)
        return std::nullopt;
    TracePath result{{}, flags};
    result.hashes.reserve(static_cast<std::size_t>(roundTripHops * width));
    const auto appendRelay = [&](int index) {
        const auto start = node->pathHashes.begin() + index * storedWidth;
        result.hashes.insert(result.hashes.end(), start, start + width);
    };
    for (int i = 0; i < hops; ++i)
        appendRelay(i);
    result.hashes.insert(result.hashes.end(), node->key.begin(), node->key.begin() + width);
    for (int i = hops; i > 0; --i)
        appendRelay(i - 1);
    return result;
}

}  // namespace

Client::Client(QObject* parent, ConnectionTrace trace)
    : QObject(parent), socket_(new QTcpSocket(this)), connectionTrace_(std::move(trace)), console_({
        [this](const meshcore::Bytes& command) { return write(command, Wait::Console); },
        [this] {
            if (wait_ == Wait::Console) {
                wait_ = Wait::None;
                timeout_ = 0;
            }
        },
        [this](int id, int operation, bool success, const std::string& text) {
            finishConsoleRequest(id, operation, success, {}, fromWire(text));
        },
        [this] { return clock_.elapsed(); },
        [this] { return nextTimestamp(); },
        [this](int id, int operation, bool success, const meshcore::Bytes& data, const std::string& text) {
            finishConsoleRequest(id, operation, success, rawBytes(data), fromWire(text));
        },
    }) {
    connect(socket_, SIGNAL(connected()), this, SLOT(socketConnected()), Qt::QueuedConnection);
    connect(socket_, SIGNAL(readyRead()), this, SLOT(readSocket()));
    connect(socket_, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));
    connect(socket_, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(socketError(QAbstractSocket::SocketError)));
#ifdef Q_OS_SYMBIAN
    wlan_ = new NativeWlan(socket_, this);
    connect(wlan_, SIGNAL(ready()), this, SLOT(connectRadioSocket()));
    connect(wlan_, SIGNAL(failed(QString)), this, SLOT(wlanFailed(QString)));
#endif
    socket_->setReadBufferSize(16384);
    clock_.start();
    channelAliases_.fill(-1);

    QTimer* timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(tick()));
    timer->start(250);

    QSettings settings;
    const QVariantMap blocked = settings.value(QLatin1String("ui/blockedSenders")).toMap();
    for (auto it = blocked.constBegin(); it != blocked.constEnd(); ++it)
        blocks_.insert(it.key(), it.value().toString());
    bool validTries = false;
    const int tries = settings.value(QLatin1String("ui/sendTries"), DefaultSendTries).toInt(&validTries);
    sendTries_ = validTries ? checkedSendTries(tries) : DefaultSendTries;
    radioKey_ = settings.value(QString::fromLatin1("radio/last")).toString();
    radioName_ = settings.value(QString::fromLatin1("radio/lastName")).toString();
    loadHistory();
    status_ = russian("Нет подключения");
}

void Client::traceConnection(const char* step, int value) const {
    if (connectionTrace_)
        connectionTrace_(step, value);
}

void Client::connectTo(const QString& host, quint16 port) {
    const QString address = host.trimmed();
    if (address.isEmpty() || !port || address.contains(QLatin1Char('/')) || address.contains(QLatin1Char(' '))) {
        status_ = russian("Укажите адрес и порт от 1 до 65535");
        emit changed();
        return;
    }

    traceConnection("TCP: reset previous session");
    finish(russian("Подключение к %1…").arg(address));
    traceConnection("TCP: session reset");
    host_ = address;
    port_ = port;
    wantsConnection_ = true;
    reconnectDelay_ = kInitialReconnectDelay;
    openLink(false);
}

void Client::openLink(bool retrying) {
    resetRadioGpsSession();
    reconnectAt_ = 0;
    retryingConnection_ = retrying;
    directReceived_ = channelReceived_ = publicReceived_ = dataReceived_ = groupHeard_ = 0;
    battery_.clear();
    batteryMillivolts_ = 0;
    queueStatus_ = russian("не прочитана");
    publicStatus_ = russian("ожидается список каналов");
    if (retrying)
        status_ = russian("Переподключение к %1…").arg(host_);
    timeout_ = clock_.elapsed() + kConnectTimeout;
    logFirstReply_ = true;
#ifdef Q_OS_SYMBIAN
    if (wlanRecovery_) {
        timeout_ = clock_.elapsed() + 30000;
        status_ = russian("Восстанавливаю Wi-Fi…");
        traceConnection("TCP: open saved WLAN");
        emit changed();
        if (wantsConnection_ && timeout_ > 0)
            wlan_->open();
        return;
    }
#endif
    connectRadioSocket();
}

void Client::connectRadioSocket() {
    if (!wantsConnection_ || timeout_ <= 0 || !pendingSocketFailure_.isEmpty() ||
        socket_->state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    timeout_ = clock_.elapsed() + kConnectTimeout;
#ifdef Q_OS_SYMBIAN
    // A MeshCore radio or bridge is a direct TCP peer on the local network.
    traceConnection("TCP: configure direct connection");
    socket_->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
#endif
    traceConnection(retryingConnection_ ? "TCP: retry connectToHost" : "TCP: enter connectToHost");
    socket_->connectToHost(host_, port_);
    traceConnection("TCP: returned from connectToHost, state", static_cast<int>(socket_->state()));
    emit changed();
}

void Client::wlanFailed(const QString& reason) {
    if (!closing_ && wantsConnection_ && (ready_ || timeout_ > 0)) {
        traceConnection("TCP: saved WLAN unavailable");
        deferSocketFailure(reason, true);
    }
}

void Client::disconnectFromRadio() {
    finish(russian("Отключено"));
}

void Client::setAutoReconnect(bool enabled) {
    autoReconnect_ = enabled;
    if (!enabled && reconnecting())
        finish(lastFailure_);
}

void Client::retryConnectionNow() {
    if (!canRetryConnectionNow())
        return;
    reconnectAt_ = qMax<qint64>(1, clock_.elapsed());
    // Reuse the timer path outside signal callbacks from the socket and settings panel.
    QTimer::singleShot(0, this, SLOT(tick()));
    emit changed();
}

void Client::setWlanRecovery(bool enabled) {
#ifdef Q_OS_SYMBIAN
    if (enabled && !wlanRecovery_ && ready_)
        wlan_->rememberActive();
#endif
    wlanRecovery_ = enabled;
}

int Client::sendTries() const { return sendTries_; }

void Client::setSendTries(int tries) {
    sendTries_ = checkedSendTries(tries);
    QSettings().setValue(QLatin1String("ui/sendTries"), sendTries_);
}

qint64 Client::sendTriesSpanMs(int tries) {
    qint64 span = 0;
    const int limit = checkedSendTries(tries);
    for (int attempt = 1; attempt < limit; ++attempt)
        span += kRetryGaps[attempt];
    return span;
}

void Client::refresh() {
    if (!ready_)
        return;
    refreshNeeded_ = true;
    proceed();
}

void Client::requestMessages() {
    if (!ready_)
        return;
    manualSync_ = true;
    syncNeeded_ = true;
    proceed();
}

bool Client::connected() const { return ready_; }

bool Client::connecting() const {
    return !ready_ && (reconnecting() || socket_->state() != QAbstractSocket::UnconnectedState || timeout_ > 0);
}

bool Client::reconnecting() const { return reconnectAt_ > 0 || retryingConnection_; }

int Client::reconnectAttempt() const { return reconnectAttempt_; }

bool Client::canRetryConnectionNow() const {
    return reconnectAt_ > clock_.elapsed() && !retryingConnection_ && wantsConnection_ && autoReconnect_ &&
           !closing_ && pendingSocketFailure_.isEmpty() && socket_->state() == QAbstractSocket::UnconnectedState;
}

bool Client::busy() const { return wait_ != Wait::None; }
QString Client::status() const { return status_; }
QString Client::radioName() const { return radioName_; }
QString Client::radioParameters() const { return parameters_.isEmpty() ? russian("Подключите рацию.") : parameters_; }
QString Client::radioParametersSummary() const {
    return ready_ && self_.frequencyKhz > 0
        ? russian("%1 МГц · %2 дБм").arg(decimalMilli(self_.frequencyKhz)).arg(self_.powerDbm)
        : QString();
}
const meshcore::SelfInfo& Client::radioIdentity() const { return self_; }
int Client::radioProtocolVersion() const { return ready_ ? radioProtocolVersion_ : 0; }
std::optional<bool> Client::radioRepeatEnabled() const { return ready_ ? radioRepeatEnabled_ : std::nullopt; }
int Client::radioPathHashMode() const { return ready_ && radioPathHashModeKnown_ ? pathHashMode_ : -1; }
const RadioReadings& Client::radioReadings() const { return radioReadings_; }
bool Client::radioReadingsPending() const { return radioReadingsStage_ >= 0; }
RadioGpsState Client::radioGpsState() const {
    return ready_ && !closing_ ? radioGpsState_ : RadioGpsState::Unknown;
}
quint64 Client::radioSessionToken() const { return radioSessionToken_; }
QString Client::batteryInfo() const { return battery_.isEmpty() ? russian("Показания ещё не запрошены.") : battery_; }
int Client::batteryMillivolts() const { return batteryMillivolts_; }
int Client::lastBatteryMillivolts() const { return lastBatteryMillivolts_; }

QString Client::radioInfo() const {
    return russian("Рация: %1\nПрошивка: %2\nPublic: %3\n\n%4\n\n"
                   "Получено за сеанс\nЛичные: %5\nКаналы: %6 (Public: %7)\n"
                   "Двоичные кадры: %8\nГрупповые пакеты в эфире: %9")
        .arg(radioName_, firmware_, publicStatus_, radioParameters())
        .arg(directReceived_).arg(channelReceived_).arg(publicReceived_).arg(dataReceived_).arg(groupHeard_)
        + russian("\nЧтение очереди: %1").arg(queueStatus_);
}

NetworkNode Client::selfNode() const {
    const bool identified = std::any_of(self_.key.begin(), self_.key.end(),
                                       [](std::uint8_t byte) { return byte != 0; });
    NetworkNode node{directId(meshcore::prefix(self_.key)), radioName_, 1, -1, 0,
                     self_.latitude, self_.longitude, identified ? radioKey_ : QString()};
    checkPosition(node.latitude, node.longitude);
    return node;
}

qint32 Client::ownLatitude() const { return selfNode().latitude; }
qint32 Client::ownLongitude() const { return selfNode().longitude; }

QVector<NetworkNode> Client::nodes() const {
    QVector<NetworkNode> result;
    result.reserve(static_cast<int>(contacts_.size()));
    for (const auto& contact : contacts_) {
        NetworkNode node{directId(meshcore::prefix(contact.key)), fromWire(contact.name), contact.type,
                         contact.path == 255 ? -1 : contact.path & 63, contact.lastAdvert,
                         contact.latitude, contact.longitude,
                         QString::fromLatin1(rawBytes(contact.key).toHex().constData()),
                         ready_ && hasNodeManagement(contact) && console_.authorized(contact.key)};
        checkPosition(node.latitude, node.longitude);
        if (node.title.isEmpty())
            node.title = targetTitle(node.id);
        result.append(node);
    }
    return result;
}

QMap<QString, ChatMessage> Client::latestMessages() const {
    QMap<QString, ChatMessage> result;
    for (auto it = history_.constEnd(); it != history_.constBegin();) {
        const auto& entry = *--it;
        if (!result.contains(entry.target) && !blocks_.contains(senderBlockId(entry.target, entry.message)))
            result.insert(entry.target, displayMessage(entry));
    }
    return result;
}

void Client::markRead(const QString& id) {
    if (unread_.remove(id)) {
        for (auto& entry : history_) {
            if (entry.target == id)
                entry.message.unread = false;
        }
        saveHistory();
        emit changed();
    }
}

QString Client::roomAuthor(const QString& prefix) const {
    const QByteArray bytes = QByteArray::fromHex(prefix.toLatin1());
    if (bytes.size() != 4)
        return QString();
    const meshcore::Contact* match = nullptr;
    for (const auto& contact : contacts_) {
        const auto* key = reinterpret_cast<const char*>(contact.key.data());
        if (!std::equal(bytes.constBegin(), bytes.constEnd(), key))
            continue;
        if (match && match->key != contact.key)
            return QString();
        match = &contact;
    }
    return match ? fromWire(match->name) : QString();
}

QMap<QString, QString> Client::blockedContacts() const {
    QMap<QString, QString> result;
    for (auto it = blocks_.constBegin(); it != blocks_.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("k:")))
            continue;
        const QString id = it.key().mid(2);
        const QString name = roomAuthor(id);
        result.insert(id, name.isEmpty() ? it.value() : name);
    }
    return result;
}

QStringList Client::blockedChannelNames() const {
    QStringList result;
    for (auto it = blocks_.constBegin(); it != blocks_.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("n:")))
            result.append(it.value());
    }
    return result;
}

int Client::senderMessageCount(const QString& id) const {
    if (id.isEmpty())
        return 0;
    return static_cast<int>(std::count_if(history_.constBegin(), history_.constEnd(), [&](const auto& entry) {
        return senderBlockId(entry.target, entry.message) == id;
    }));
}

int Client::blockedContactMessageCount(const QString& id) const {
    return senderMessageCount(QLatin1String("k:") + id);
}

int Client::blockedChannelMessageCount(const QString& name) const {
    return senderMessageCount(channelBlockId(name));
}

void Client::setSenderBlocked(const QString& id, const QString& name, bool blocked) {
    if (id.isEmpty() || blocks_.contains(id) == blocked)
        return;
    if (blocked)
        blocks_.insert(id, name);
    else
        blocks_.remove(id);
    QVariantMap saved;
    for (auto it = blocks_.constBegin(); it != blocks_.constEnd(); ++it)
        saved.insert(it.key(), it.value());
    QSettings settings;
    settings.setValue(QLatin1String("ui/blockedSenders"), saved);
    settings.sync();
    if (blocked) {
        for (auto& entry : history_) {
            if (entry.message.unread && senderBlockId(entry.target, entry.message) == id)
                entry.message.unread = false;
        }
        refreshUnread();
        saveHistory();
    }
    emit changed();
}

void Client::unblockContact(const QString& id) {
    setSenderBlocked(QLatin1String("k:") + id, QString(), false);
}

void Client::unblockChannelName(const QString& name) {
    setSenderBlocked(channelBlockId(name), QString(), false);
}

QVector<ChatTarget> Client::targets() const {
    QVector<ChatTarget> result;
    QMap<QString, bool> listed;
    const auto append = [&](const QString& id) {
        if (listed.contains(id))
            return;
        listed.insert(id, true);
        result.append(ChatTarget{id, targetTitle(id), id.startsWith(QLatin1String("c/")),
                                 unread_.value(id)});
    };
    for (const auto& channel : channels_) {
        if (channelAliases_[channel.index] == channel.index)
            append(channelId(channel));
    }
    for (const auto& contact : contacts_) {
        if (canChat(contact))
            append(directId(meshcore::prefix(contact.key)));
    }
    for (const auto& entry : history_) {
        if (!blocks_.contains(senderBlockId(entry.target, entry.message)))
            append(entry.target);
    }
    return result;
}

QVector<ChatMessage> Client::messages(const QString& id) const {
    QVector<ChatMessage> result;
    int pendingPosition = 0;
    for (int i = 0; i < history_.size(); ++i) {
        const auto& entry = history_.at(i);
        if (entry.target == id) {
            result.append(displayMessage(entry));
            if (i < pendingHistoryPosition_)
                ++pendingPosition;
        }
    }
    if (!pendingRetry_ && !pendingId_.isEmpty() && pendingId_ == id)
        result.insert(pendingPosition, pendingMessage_);
    // The radio hands up its receive queue in order. Its peers' clocks may disagree.
    return result;
}

ChatMessage Client::displayMessage(const StoredMessage& row) const {
    ChatMessage result = row.message;
    result.blockId = senderBlockId(row.target, row.message);
    result.blocked = blocks_.contains(result.blockId);
    result.canReact = !result.blocked && canReact(row);
    const bool channel = row.target.startsWith(QLatin1String("c/"));
    for (const auto& reaction : row.reactions) {
        if (reaction.emoji.isEmpty() ||
                (channel && !reaction.own && blocks_.contains(channelBlockId(reaction.actor))))
            continue;
        QString author;
        if (!reaction.own) {
            author = reaction.actor.simplified();
            if (!channel && reaction.actor.startsWith(QLatin1String("d/"))) {
                const QString prefix = reaction.actor.mid(2).toUpper();
                author = targetNames_.value(reaction.actor).simplified();
                if ((author.isEmpty() || author == prefix) && !row.message.outgoing &&
                        row.target == reaction.actor) {
                    const QString recorded = row.message.author.simplified();
                    if (!recorded.isEmpty())
                        author = recorded;
                }
                if (author.isEmpty())
                    author = prefix;
            }
        }
        auto matching = std::find_if(result.reactions.begin(), result.reactions.end(), [&](const auto& summary) {
            return summary.emoji == reaction.emoji;
        });
        if (matching == result.reactions.end()) {
            result.reactions.append(ChatReaction{reaction.emoji, 1, reaction.own,
                                                 reaction.own ? QStringList() : QStringList(author)});
        } else {
            ++matching->count;
            matching->own = matching->own || reaction.own;
            if (!reaction.own)
                matching->authors.append(author);
        }
    }
    return result;
}

bool Client::canReact(const StoredMessage& row) const {
    if (!ready_ || closing_ || row.message.text.isEmpty())
        return false;
    if (const auto channel = channelForId(row.target)) {
        if (self_.name.empty() || self_.name.size() > 32 ||
            std::none_of(channel->key.begin(), channel->key.end(), [](auto byte) { return byte != 0; })) {
            return false;
        }
    } else {
        const auto contact = contactForId(row.target);
        if (!contact || contact->type != 1)
            return false;
    }
    if (!row.message.outgoing)
        return true;
    const auto& delivery = row.message.delivery;
    return delivery == accepted() || delivery == waitingForAck() || delivery == delivered() ||
           delivery == unconfirmed() || delivery == russian("Повторная отправка");
}

bool Client::react(const QString& targetId, const ChatMessage& message, const QString& emoji) {
    if (reactionRequest_ || !pendingSocketFailure_.isEmpty())
        return false;
    const auto utf8 = emoji.toUtf8();
    const std::string_view wireEmoji(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    if (!meshcore::validReactionEmoji(wireEmoji))
        return false;
    for (const auto& row : history_) {
        if (row.target == targetId && row.message.timestamp == message.timestamp && row.message.text == message.text &&
            row.message.outgoing == message.outgoing && row.message.author == message.author && canReact(row)) {
            const auto own = std::find_if(row.reactions.begin(), row.reactions.end(), [](const auto& entry) {
                return entry.own && !entry.emoji.isEmpty();
            });
            if ((own == row.reactions.end() && emoji.isEmpty()) || (own != row.reactions.end() && own->emoji == emoji))
                return true;
            reactionRequest_ = PendingReaction{targetId, row.reactionHash, ReactionEntry{emoji, {}, 0, true}, {}};
            proceed();
            return true;
        }
    }
    return false;
}

void Client::startReaction() {
    auto& pending = *reactionRequest_;
    bool available = false;
    for (const auto& row : history_) {
        if (appliesTo(pending, row) && canReact(row)) {
            available = true;
            break;
        }
    }
    if (!available) {
        reactionRequest_.reset();
        status_ = russian("Сообщение для реакции недоступно");
        emit changed();
        return;
    }
    pending.reaction.stamp = nextTimestamp();
    const auto emoji = pending.reaction.emoji.toUtf8();
    meshcore::Reaction reaction{pending.target, {emoji.constData(), static_cast<std::size_t>(emoji.size())},
                               self_.name, pending.reaction.stamp};
    meshcore::Bytes command;
    if (const auto channel = channelForId(pending.id))
        command = meshcore::sendChannelReaction(channel->index, reaction);
    else if (const auto contact = contactForId(pending.id); contact && contact->type == 1)
        command = meshcore::sendDirectReaction(meshcore::prefix(contact->key), reaction);
    if (command.empty()) {
        reactionRequest_.reset();
        status_ = russian("Реакцию не удалось подготовить");
        emit changed();
        return;
    }
    write(command, Wait::Reaction);
}

void Client::onReactionResult(const meshcore::Bytes& frame) {
    if (!reactionRequest_)
        return;
    const bool channel = reactionRequest_->id.startsWith(QLatin1String("c/"));
    const bool accepted = channel ? frame[0] == meshcore::response::Ok : meshcore::parseSentTag(frame).has_value();
    if (!accepted && frame[0] != meshcore::response::Error && frame[0] != 0x0f)
        return;
    auto pending = std::move(*reactionRequest_);
    reactionRequest_.reset();
    status_ = accepted ? russian("Реакция принята рацией") : russian("Рация отклонила реакцию");
    if (accepted)
        takeReaction(std::move(pending), false);
    wait_ = Wait::None;
    timeout_ = 0;
    proceed();
    emit changed();
}

bool Client::appliesTo(const PendingReaction& reaction, const StoredMessage& row) const {
    return row.target == reaction.id && row.reactionHash == reaction.target &&
           (reaction.recipient.isEmpty() || row.message.author == reaction.recipient);
}

bool Client::applyReaction(const PendingReaction& reaction, StoredMessage& row) {
    auto& entries = row.reactions;
    const auto& entry = reaction.reaction;
    const auto sameActor = [&](const auto& known) {
        return known.own == entry.own && known.actor == entry.actor;
    };
    for (const auto& known : entries) {
        if (!sameActor(known))
            continue;
        if ((!reaction.replace && known.emoji == entry.emoji) ||
            (reaction.replace && entry.stamp != 0 && known.stamp > entry.stamp) ||
            (reaction.replace && entry.stamp == known.stamp && known.emoji == entry.emoji)) {
            return false;
        }
    }
    bool changed = false;
    if (reaction.replace) {
        for (int i = entries.size() - 1; i >= 0; --i) {
            if (sameActor(entries.at(i))) {
                entries.remove(i);
                changed = true;
            }
        }
    }
    if (!entry.emoji.isEmpty() || entry.stamp != 0) {
        if (entries.size() >= kMessageReactionLimit)
            entries.remove(0);
        entries.append(entry);
        changed = true;
    }
    return changed;
}

void Client::takeReaction(PendingReaction reaction, bool deferIfMissing) {
    for (int i = history_.size() - 1; i >= 0; --i) {
        auto& row = history_[i];
        if (appliesTo(reaction, row)) {
            if (applyReaction(reaction, row)) {
                saveHistory();
                emit changed();
            }
            return;
        }
    }
    if (deferIfMissing) {
        const auto now = clock_.elapsed();
        while (!deferredReactions_.empty() && (deferredReactions_.size() >= kDeferredReactionLimit ||
               now - deferredReactions_.front().heardAt >= kDeferredReactionLifetime)) {
            deferredReactions_.pop_front();
        }
        reaction.heardAt = now;
        deferredReactions_.push_back(std::move(reaction));
    }
}

void Client::adoptReactions(StoredMessage& row) {
    const auto now = clock_.elapsed();
    for (auto it = deferredReactions_.begin(); it != deferredReactions_.end();) {
        if (now - it->heardAt >= kDeferredReactionLifetime) {
            it = deferredReactions_.erase(it);
        } else if (appliesTo(*it, row)) {
            applyReaction(*it, row);
            it = deferredReactions_.erase(it);
        } else {
            ++it;
        }
    }
}

void Client::onChannelData(const meshcore::Bytes& frame) {
    ++dataReceived_;
    if (wait_ == Wait::Contacts || wait_ == Wait::Channel || !ready_) {
        if (deferredChannelData_.size() < kDeferredReactionLimit) {
            deferredChannelData_.push_back(frame);
            --dataReceived_;
        }
        return;
    }
    const auto reaction = meshcore::parseChannelReaction(frame);
    if (!reaction)
        return;
    const auto channel = channelForIndex(frame[4]);
    if (!channel)
        return;
    takeReaction(PendingReaction{channelId(*channel), reaction->target,
        ReactionEntry{fromWire(reaction->emoji), fromWire(reaction->by), reaction->stamp, false}, {}});
}

QString Client::targetTitle(const QString& id) const {
    const QString title = targetNames_.value(id);
    if (!title.isEmpty())
        return title;
    if (id.startsWith(QLatin1String("c/")))
        return russian("Канал %1").arg(id.section(QLatin1Char('/'), 1, 1).toInt() + 1);
    return id.mid(2).toUpper();
}

const meshcore::Contact* Client::contactForId(const QString& id) const {
    const meshcore::Contact* result = nullptr;
    for (const auto& contact : contacts_) {
        if (directId(meshcore::prefix(contact.key)) != id)
            continue;
        if (result && result->key != contact.key)
            return nullptr;
        result = &contact;
    }
    return result;
}

const meshcore::Channel* Client::channelForIndex(int index) const {
    if (channelAliases_[index] >= 0)
        index = channelAliases_[index];
    const auto channel = std::find_if(channels_.begin(), channels_.end(), [index](const auto& item) {
        return item.index == index;
    });
    return channel == channels_.end() ? nullptr : &*channel;
}

const meshcore::Channel* Client::channelForId(const QString& id) const {
    const int index = id.section(QLatin1Char('/'), 1, 1).toInt();
    if (index < 0 || index >= 256 || !id.startsWith(QLatin1String("c/")))
        return nullptr;
    for (const auto& channel : channels_) {
        if (channel.index == index && channelId(channel) == id)
            return channelForIndex(index);
    }
    return nullptr;
}

int Client::maxTextBytes(const QString& id) const {
    if (channelForId(id))
        return static_cast<int>(meshcore::channelLimit(radioNameBytes_));
    if (const auto contact = contactForId(id); contact && canChat(*contact))
        return contact->type == 3 ? 151 : static_cast<int>(meshcore::DirectLimit);
    return 0;
}

quint32 Client::nextTimestamp() {
    const auto now = static_cast<quint32>(QDateTime::currentDateTime().toTime_t());
    lastOutgoingTimestamp_ = std::max(now, lastOutgoingTimestamp_ + 1);
    return lastOutgoingTimestamp_;
}

bool Client::send(const QString& id, const QString& text) {
    if (!ready_ || closing_ || text.trimmed().isEmpty() || outgoing_.size() >= kSendQueueLimit)
        return false;
    const QByteArray utf8 = text.toUtf8();
    const int limit = maxTextBytes(id);
    if (!limit || utf8.size() > limit || utf8.contains('\0')) {
        status_ = limit ? russian("Текст должен занимать от 1 до %1 байт UTF-8").arg(limit)
                        : russian("Обновите список контактов и каналов");
        emit changed();
        return false;
    }
    const auto channel = channelForId(id);
    outgoing_.push_back(TextRequest{channel ? channelId(*channel) : id, text});
    proceed();
    return true;
}

bool Client::sendNext() {
    int directPending = 0;
    for (const auto& row : history_) {
        if (row.deadline > 0)
            ++directPending;
    }
    auto queued = outgoing_.begin();
    for (; queued != outgoing_.end(); ++queued) {
        if (queued->id.startsWith(QLatin1String("c/")))
            break;
        if (directPending < kDirectAckLimit && std::none_of(history_.begin(), history_.end(), [&](const auto& row) {
                return row.deadline > 0 && row.target == queued->id;
            })) {
            break;
        }
    }
    if (queued == outgoing_.end())
        return false;
    const TextRequest request = *queued;
    outgoing_.erase(queued);
    const QByteArray utf8 = request.text.toUtf8();
    const std::string_view body(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const auto timestamp = nextTimestamp();
    pendingId_ = request.id;
    pendingMessage_ = ChatMessage{radioName_, request.text, timestamp, true, russian("Отправка…")};
    pendingHistoryPosition_ = history_.size();
    pendingKey_ = {};
    pendingEcho_.clear();
    meshcore::Bytes command;
    if (const auto channel = channelForId(request.id)) {
        command = meshcore::sendChannel(channel->index, body, timestamp, radioNameBytes_);
        std::string prefixed = self_.name + ": ";
        prefixed.append(body);
        pendingEcho_ = meshcore::groupTextPayload(channel->key, timestamp, prefixed);
        pendingEchoFrom_ = clock_.elapsed();
    } else if (const auto contact = contactForId(request.id); contact && canChat(*contact)) {
        pendingKey_ = contact->key;
        command = meshcore::sendDirect(meshcore::prefix(contact->key), body, timestamp);
    }
    if (command.empty()) {
        settleSend(russian("Контакт или канал недоступен"));
        return false;
    }
    status_ = pendingMessage_.delivery;
    if (write(command, Wait::Send))
        emit changed();
    return true;
}

bool Client::advertise() {
    if (!ready_ || closing_)
        return false;
    advertNeeded_ = true;
    proceed();
    return true;
}

bool Client::requestBattery() {
    if (!ready_ || closing_)
        return false;
    batteryNeeded_ = !radioReadingsPending();
    proceed();
    return true;
}

bool Client::requestRadioReadings() {
    if (!ready_ || closing_)
        return false;
    if (radioReadingsPending())
        return true;
    nextRadioReadings_ = {};
    radioReadingsStage_ = 0;
    proceed();
    emit changed();
    return true;
}

bool Client::requestRadioGps() {
    if (!ready_ || closing_ || !pendingSocketFailure_.isEmpty())
        return false;
    if (radioGpsState_ == RadioGpsState::Unknown && wait_ != Wait::RadioGps) {
        radioGpsNeeded_ = true;
        proceed();
    }
    return true;
}

bool Client::applyRadioSettings(int requestId, const std::vector<meshcore::Bytes>& commands,
                               bool readBackIdentity) {
    if (!ready_ || closing_ || wait_ == Wait::RestartRadio || radioSettingsTransaction_ ||
        radioBackupTransaction_ || radioBackupKeyRequest_ ||
        requestId <= 0 || commands.empty() || commands.size() > 8 ||
        !std::all_of(commands.begin(), commands.end(), isRadioSettingsWrite))
        return false;
    radioSettingsTransaction_ = RadioSettingsTransaction{requestId, commands, readBackIdentity};
    proceed();
    return true;
}

bool Client::restartRadio(bool factoryReset) {
    if (!ready_ || busy() || closing_ || radioSettingsTransaction_ || radioBackupTransaction_ ||
        radioBackupKeyRequest_ || console_.busy())
        return false;
    const meshcore::Bytes command = factoryReset ? meshcore::Bytes{51, 'r', 'e', 's', 'e', 't'}
                                                 : meshcore::Bytes{19, 'r', 'e', 'b', 'o', 'o', 't'};
    if (!write(command, Wait::RestartRadio))
        return false;
    status_ = russian("Команда отправлена. Ожидание перезапуска рации…");
    emit changed();
    return true;
}

QSet<QString> Client::trustedContactKeys() const {
    QSet<QString> keys;
    if (ready_) {
        for (const auto& contact : contacts_) {
            if (contact.flags & kTrustedContactFlags)
                keys.insert(QString::fromLatin1(rawBytes(contact.key).toHex().constData()));
        }
    }
    return keys;
}

bool Client::setTrustedContact(int requestId, const QString& publicKey, bool enabled) {
    if (!ready_ || radioProtocolVersion_ < 5 || publicKey.size() != 64)
        return false;
    const QByteArray key = QByteArray::fromHex(publicKey.toLatin1());
    if (key.size() != 32)
        return false;
    const auto contact = std::find_if(contacts_.begin(), contacts_.end(), [&](const meshcore::Contact& entry) {
        return rawBytes(entry.key) == key;
    });
    if (contact == contacts_.end() || (contact->type != 1 && (enabled || !(contact->flags & kTrustedContactFlags))))
        return false;
    const auto flags = static_cast<std::uint8_t>(enabled ? contact->flags | kTrustedContactFlags
                                                        : contact->flags & ~kTrustedContactFlags);
    return applyRadioSettings(requestId, {addUpdateContactCommand(*contact, flags)}, false);
}

QVector<RadioChannel> Client::radioChannels() const {
    QVector<RadioChannel> result;
    if (!ready_)
        return result;
    result.reserve(channelCount_);
    auto configured = channels_.begin();
    for (int slot = 0; slot < channelCount_; ++slot) {
        RadioChannel row{slot, QString(), QString()};
        if (configured != channels_.end() && configured->index == slot) {
            row.title = fromWire(configured->name);
            row.key = QString::fromLatin1(rawBytes(configured->key).toHex().constData());
            ++configured;
        }
        result.append(row);
    }
    return result;
}

int Client::radioBackupMessageCount() const { return history_.size(); }

std::optional<RadioBackupSnapshot> Client::radioBackupSnapshot() const {
    if (!ready_ || closing_ || !pendingSocketFailure_.isEmpty() || radioBackupTransaction_ ||
        radioSettingsTransaction_ || channelChangeSlot_ != -1 || !radioContactsComplete_ ||
        !radioChannelsComplete_ || wait_ == Wait::Contacts || wait_ == Wait::Channel ||
        inventoryChanged_ || !validRadioBackupKey(radioKey_))
        return std::nullopt;
    RadioBackupSnapshot snapshot;
    snapshot.publicKey = radioKey_;
    snapshot.identity = self_;
    snapshot.contacts = contacts_;
    snapshot.repeatEnabled = radioRepeatEnabled_;
    snapshot.pathHashMode = radioPathHashMode();
    snapshot.maxChannels = channelCount_;
    snapshot.maxContacts = radioMaxContacts_;
    for (const auto& channel : channels_) {
        snapshot.channels.append({channel.index, fromWire(channel.name),
            QString::fromLatin1(rawBytes(channel.key).toHex().constData())});
    }
    return snapshot;
}

bool Client::requestRadioBackupKey(int requestId, const QString& expectedTarget) {
    if (!ready_ || closing_ || !pendingSocketFailure_.isEmpty() || requestId <= 0 ||
        !validRadioBackupKey(expectedTarget) || expectedTarget != radioKey_ || radioBackupKeyRequest_ ||
        radioBackupTransaction_ || radioSettingsTransaction_ || channelChangeSlot_ != -1 ||
        console_.busy() || wait_ == Wait::RestartRadio)
        return false;
    radioBackupKeyRequest_ = requestId;
    radioBackupKeyTarget_ = expectedTarget;
    QTimer::singleShot(0, this, SLOT(tick()));
    return true;
}

bool Client::restoreRadioBackup(int requestId, const RadioBackupRestore& copy) {
    if (!ready_ || closing_ || !pendingSocketFailure_.isEmpty() || requestId <= 0 ||
        !validRadioBackupKey(copy.expectedTarget) || copy.expectedTarget != radioKey_ ||
        !validRadioBackupKey(copy.sourcePublicKey) || radioBackupKeyRequest_ || radioBackupTransaction_ ||
        radioSettingsTransaction_ || channelChangeSlot_ != -1 || wait_ == Wait::RestartRadio ||
        !radioContactsComplete_ || !radioChannelsComplete_ || console_.busy() ||
        wait_ == Wait::Contacts || wait_ == Wait::Channel || inventoryChanged_ || refreshNeeded_ ||
        copy.settings.size() > 7 || copy.channels.size() > 256 || copy.contacts.size() > kContactLimit)
        return false;
    if (!copy.privateKey.isEmpty() &&
        !radioKeyMatchesPublic(copy.privateKey, QByteArray::fromHex(copy.sourcePublicKey.toLatin1())))
        return false;

    RadioBackupTransaction transaction;
    transaction.id = requestId;
    transaction.target = copy.expectedTarget;
    transaction.source = copy.sourcePublicKey;
    transaction.host = host_;
    transaction.port = port_;
    const bool changeKey = !copy.privateKey.isEmpty() && copy.sourcePublicKey != radioKey_;
    if (changeKey)
        transaction.writes.push_back({kImportPrivateKey});
    QSet<int> settingTypes;
    for (auto command : copy.settings) {
        if (!validRadioBackupSetting(command) || settingTypes.contains(command.front()))
            return false;
        settingTypes.insert(command.front());
        if (command.front() == 12 && self_.maxPowerDbm > 0 && static_cast<qint8>(command[1]) > self_.maxPowerDbm)
            command[1] = static_cast<std::uint8_t>(self_.maxPowerDbm);
        if (command.front() == 11) {
            if (!radioRepeatEnabled_ && command.size() == 12)
                command.pop_back();
            else if (radioRepeatEnabled_ && command.size() == 11)
                command.push_back(*radioRepeatEnabled_ ? 1 : 0);
        }
        if ((command.front() == 61 && !radioPathHashModeKnown_) ||
            (command.front() == 58 && radioProtocolVersion_ < 8)) {
            transaction.skipped = russian("Часть настроек недоступна на этой прошивке");
            continue;
        }
        transaction.writes.push_back(std::move(command));
    }

    QSet<int> occupiedSlots;
    QSet<QByteArray> channelKeys;
    for (const auto& existing : channels_) {
        occupiedSlots.insert(existing.index);
        channelKeys.insert(rawBytes(existing.key));
    }
    int omittedChannels = 0;
    for (const auto& channel : copy.channels) {
        const auto title = channel.title.toUtf8();
        const auto hex = channel.key.toLatin1();
        const auto key = QByteArray::fromHex(hex);
        if (channel.slot < 0 || channel.slot > 255 || title.size() > 31 || title.contains('\0') ||
            key.size() != 16 || key.toHex() != hex || (title.isEmpty() && key == QByteArray(16, '\0')))
            return false;
        if (channelKeys.contains(key))
            continue;
        int slot = channel.slot;
        if (slot >= channelCount_ || occupiedSlots.contains(slot)) {
            slot = 0;
            while (slot < channelCount_ && occupiedSlots.contains(slot))
                ++slot;
        }
        if (slot == channelCount_) {
            ++omittedChannels;
            continue;
        }
        std::array<std::uint8_t, 16> wireKey;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(key.constData()), wireKey.size(), wireKey.begin());
        transaction.writes.push_back(meshcore::setChannel(static_cast<std::uint8_t>(slot),
            {title.constData(), static_cast<std::size_t>(title.size())}, wireKey));
        occupiedSlots.insert(slot);
        channelKeys.insert(key);
    }
    if (omittedChannels) {
        if (!transaction.skipped.isEmpty())
            transaction.skipped += russian(". ");
        transaction.skipped += russian("Нет места для каналов: %1").arg(omittedChannels);
    }

    QSet<QByteArray> contactKeys;
    for (const auto& contact : contacts_)
        contactKeys.insert(rawBytes(contact.key));
    int contactCount = contactKeys.size();
    contactKeys.insert(QByteArray::fromHex(copy.sourcePublicKey.toLatin1()));
    contactKeys.insert(rawBytes(self_.key));
    int omittedContacts = 0;
    for (const auto& contact : copy.contacts) {
        const auto key = rawBytes(contact.key);
        const auto name = rawBytes(contact.name);
        const int pathWidth = (contact.path >> 6) + 1;
        if (key == QByteArray(32, '\0') || contact.type > 4 || contact.type == 0 ||
            name.size() > 32 || name.contains('\0') ||
            QString::fromUtf8(name.constData(), name.size()).toUtf8() != name ||
            !radioBackupCoordinates(contact.latitude, contact.longitude) ||
            (contact.path != 0xff && (pathWidth > 3 || pathWidth * (contact.path & 63) > 64)))
            return false;
        if (contactKeys.contains(key))
            continue;
        contactKeys.insert(key);
        if (radioMaxContacts_ > 0 && contactCount >= radioMaxContacts_) {
            ++omittedContacts;
            continue;
        }
        ++contactCount;
        transaction.writes.push_back(addUpdateContactCommand(contact, contact.flags));
    }
    if (omittedContacts) {
        if (!transaction.skipped.isEmpty())
            transaction.skipped += russian(". ");
        transaction.skipped += russian("Нет места для контактов: %1").arg(omittedContacts);
    }
    if (changeKey) {
        transaction.key = copy.privateKey;
        transaction.key.detach();
    }
    radioBackupTransaction_ = std::move(transaction);
    QTimer::singleShot(0, this, SLOT(tick()));
    return true;
}

bool Client::setRadioChannel(int slot, const QString& name, const QString& keyHex) {
    if (!ready_ || closing_ || slot < 0 || slot >= channelCount_ || channelChangeSlot_ != -1 ||
        radioBackupKeyRequest_ || radioBackupTransaction_)
        return false;
    const QByteArray title = name.trimmed().toUtf8();
    const QByteArray hex = keyHex.trimmed().toLatin1().toLower();
    const QByteArray secret = QByteArray::fromHex(hex);
    const bool clearing = name.isEmpty() && keyHex.isEmpty();
    if (!clearing && (title.isEmpty() || hex.size() != 32 || secret.size() != 16 || secret.toHex() != hex))
        return false;
    std::array<std::uint8_t, 16> key{};
    if (!clearing)
        std::copy_n(secret.constData(), key.size(), key.begin());
    channelChangeCommand_ = meshcore::setChannel(static_cast<std::uint8_t>(slot),
        {title.constData(), static_cast<std::size_t>(title.size())}, key);
    if (channelChangeCommand_.empty())
        return false;
    channelChangeSlot_ = slot;
    proceed();
    return true;
}

void Client::finishChannelChange(bool success, const QString& explanation) {
    const int slot = channelChangeSlot_;
    channelChangeSlot_ = -1;
    channelChangeReload_ = false;
    std::fill(channelChangeCommand_.begin(), channelChangeCommand_.end(), 0);
    channelChangeCommand_.clear();
    if (slot >= 0)
        emit channelResult(slot, success, explanation);
}

int Client::nextConsoleRequestId() {
    if (consoleRequestId_ == std::numeric_limits<int>::max())
        consoleRequestId_ = 0;
    return ++consoleRequestId_;
}

bool Client::runConsole(int requestId, const QString& nodeId, const QString& command) {
    return submitConsole(requestId, nodeId.isEmpty() ? meshcore::RadioConsole::Local : meshcore::RadioConsole::Remote,
                         nodeId, command);
}

bool Client::loginConsole(int requestId, const QString& nodeId, const QString& password) {
    return !nodeId.isEmpty() && submitConsole(requestId, meshcore::RadioConsole::Login, nodeId, password);
}

bool Client::nodeSignedIn(const QString& nodeId) const {
    const auto* contact = ready_ && !closing_ && !nodeId.isEmpty() ? contactForId(nodeId) : nullptr;
    return contact && console_.signedIn(contact->key);
}

bool Client::nodeConsoleAuthorized(const QString& nodeId) const {
    const auto* contact = ready_ && !closing_ && !nodeId.isEmpty() ? contactForId(nodeId) : nullptr;
    return contact && hasNodeManagement(*contact) && console_.authorized(contact->key);
}

bool Client::requestNodeNeighbours(int requestId, const QString& nodeId, int order, int offset) {
    return !nodeId.isEmpty() && order >= 0 && order <= 3 && offset >= 0 && offset <= 65535 &&
           submitConsole(requestId, NodeNeighbours, nodeId, {}, order, offset);
}

bool Client::requestNodeAccess(int requestId, const QString& nodeId) {
    return !nodeId.isEmpty() && submitConsole(requestId, NodeAccess, nodeId, {});
}

bool Client::requestNodeRouteReset(int requestId, const QString& nodeId) {
    return !nodeId.isEmpty() && submitConsole(requestId, NodeRouteReset, nodeId, {});
}

bool Client::canTraceNodeRoute(const QString& nodeId, bool direct) const {
    return ready_ && !closing_ && repeaterTracePath(contactForId(nodeId), pathHashMode_, direct).has_value();
}

bool Client::requestNodeRouteTrace(int requestId, const QString& nodeId, bool direct) {
    return canTraceNodeRoute(nodeId, direct) && submitConsole(requestId, NodeRouteTrace, nodeId, {}, 0, 0, direct);
}

bool Client::requestNodeTelemetry(int requestId, const QString& nodeId) {
    return !nodeId.isEmpty() && submitConsole(requestId, NodeTelemetry, nodeId, {});
}

bool Client::logOutNode(int requestId, const QString& nodeId) {
    return nodeSignedIn(nodeId) && submitConsole(requestId, NodeLogout, nodeId, {});
}

std::optional<NodeNeighboursPage> Client::parseNodeNeighbours(const QByteArray& data) {
    constexpr int rowSize = 11;
    if (data.size() < 4)
        return std::nullopt;
    const int total = static_cast<int>(littleEndian(data, 0, 2));
    const int count = static_cast<int>(littleEndian(data, 2, 2));
    if (count > total || count > 10 || count > (data.size() - 4) / rowSize)
        return std::nullopt;
    NodeNeighboursPage result{total, {}};
    result.rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int at = 4 + i * rowSize;
        result.rows.append(NodeNeighbour{QString::fromLatin1(data.mid(at, 6).toHex().constData()),
                                         littleEndian(data, at + 6, 4),
                                         static_cast<qint8>(wireByte(data, at + 10))});
    }
    return result;
}

QVector<NodeAccessEntry> Client::parseNodeAccess(const QByteArray& data) {
    QVector<NodeAccessEntry> result;
    for (int at = 0; at + 7 <= data.size(); at += 7) {
        const int permissions = wireByte(data, at + 6);
        if (!permissions) // MeshCore pads the encrypted reply with zero bytes.
            break;
        result.append(NodeAccessEntry{QString::fromLatin1(data.mid(at, 6).toHex().constData()), permissions});
    }
    return result;
}

NodeTelemetryReadings Client::parseNodeTelemetry(const QByteArray& data) {
    NodeTelemetryReadings result;
    for (int at = 0; at + 2 <= data.size();) {
        const int channel = wireByte(data, at++);
        const quint8 kind = wireByte(data, at++);
        const int width = telemetryFieldWidth(kind);
        if ((channel == 0 && kind == 0) || width == 0 || width > data.size() - at)
            break;
        if (channel == 1 && (kind == 0x67 || kind == 0x74)) {
            const auto value = static_cast<qint16>((quint16(wireByte(data, at)) << 8) | wireByte(data, at + 1));
            if (kind == 0x67)
                result.temperatureTenthsC = value;
            else
                result.batteryMillivolts = value * 10;
        }
        at += width;
    }
    return result;
}

std::optional<NodeRouteTraceResult> Client::parseNodeRouteTrace(const QByteArray& data) {
    if (data.size() < 6)
        return std::nullopt;
    NodeRouteTraceResult result{static_cast<int>(littleEndian(data, 0, 4)), {}};
    result.snrQuarters.reserve(data.size() - 4);
    for (int i = 4; i < data.size(); ++i)
        result.snrQuarters.append(static_cast<qint8>(wireByte(data, i)));
    return result;
}

bool Client::submitConsole(int requestId, int operation, const QString& nodeId, const QString& input,
                           int order, int offset, bool directTrace) {
    if (!ready_ || closing_ || consoleRequest_ || console_.busy())
        return false;
    if (!nodeId.isEmpty()) {
        const auto contact = contactForId(nodeId);
        if (!contact || !supportsManagementOperation(*contact, operation))
            return false;
        if (operation == NodeNeighbours && !console_.signedIn(contact->key))
            return false;
        if (operation == NodeAccess && !console_.authorized(contact->key))
            return false;
    }
    consoleRequest_ = ConsoleRequest{requestId, operation, nodeId, input, order, offset, directTrace};
    proceed();
    return true;
}

void Client::startConsole() {
    ConsoleRequest request = std::move(*consoleRequest_);
    consoleRequest_.reset();
    const auto contact = request.nodeId.isEmpty() ? nullptr : contactForId(request.nodeId);
    if (!request.nodeId.isEmpty() && (!contact || !supportsManagementOperation(*contact, request.operation))) {
        request.input.fill(QLatin1Char('\0'));
        finishConsoleRequest(request.id, request.operation, false, {}, russian("Узел недоступен"));
        return;
    }
    if (request.operation == NodeRouteTrace) {
        const auto path = repeaterTracePath(contact, pathHashMode_, request.directTrace);
        if (!path) {
            finishConsoleRequest(request.id, request.operation, false, {}, russian("Маршрут для проверки недоступен"));
            return;
        }
        console_.startManagement(request.id, request.operation, contact->key, 0, 0, path->hashes, path->flags);
        return;
    }
    if (request.operation >= NodeNeighbours) {
        console_.startManagement(request.id, request.operation, contact->key, request.order, request.offset);
        return;
    }
    QByteArray input = request.input.toUtf8();
    request.input.fill(QLatin1Char('\0'));
    console_.start(request.id, request.operation, contact ? &contact->key : nullptr,
                   {input.constData(), static_cast<std::size_t>(input.size())});
    input.fill('\0');
}

void Client::finishConsoleRequest(int id, int operation, bool success, const QByteArray& data, const QString& message) {
    if (operation >= NodeNeighbours) {
        if (operation == NodeRouteReset && success)
            refreshNeeded_ = true;
        emit nodeManagementResult(id, operation, success, data, success ? QString() : message);
    } else {
        emit consoleResult(id, operation, success, message);
    }
    if (operation == meshcore::RadioConsole::Login || operation == NodeLogout)
        emit changed();
}

void Client::socketConnected() {
    if (closing_ || !pendingSocketFailure_.isEmpty() || !wantsConnection_ || ready_ ||
        wait_ != Wait::None || timeout_ == 0 || socket_->state() != QAbstractSocket::ConnectedState)
        return;

    traceConnection("TCP: connected");
    status_ = russian("Читаю данные рации…");
    traceConnection("MeshCore: send device query");
    if (write(meshcore::deviceQuery(), Wait::Device)) {
        traceConnection("MeshCore: device query queued");
        emit changed();
    }
}

void Client::readSocket() {
    while (pendingSocketFailure_.isEmpty() && socket_->bytesAvailable() > 0) {
        QByteArray chunk = socket_->read(4096);
        if (chunk.isEmpty())
            break;
        if (logFirstReply_) {
            logFirstReply_ = false;
            traceConnection("TCP: first reply bytes", static_cast<int>(chunk.size()));
        }
        auto frames = decoder_.feed(
            {reinterpret_cast<const std::uint8_t*>(chunk.constData()),
             static_cast<std::size_t>(chunk.size())},
            clock_.elapsed());
        if (wait_ == Wait::RadioBackupKey)
            chunk.fill('\0');
        for (auto& frame : frames) {
            if (!pendingSocketFailure_.isEmpty() || socket_->state() != QAbstractSocket::ConnectedState)
                return;
            frameReceived(frame);
            if (!frame.empty() && frame.front() == kPrivateKeyReply)
                std::fill(frame.begin(), frame.end(), 0);
        }
    }
}

void Client::socketDisconnected() {
    if (!closing_ && (ready_ || timeout_ > 0)) {
        traceConnection("TCP: disconnected");
        deferSocketFailure(russian("Соединение закрыто"), true);
    }
}

void Client::socketError(QAbstractSocket::SocketError error) {
    if (closing_ || (!ready_ && timeout_ == 0))
        return;
    traceConnection("TCP: socket error", static_cast<int>(error));
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        deferSocketFailure(russian("Рация или мост отклонили подключение"), true);
        break;
    case QAbstractSocket::HostNotFoundError:
        deferSocketFailure(russian("Адрес не найден"));
        break;
    case QAbstractSocket::SocketAccessError:
        deferSocketFailure(russian("Телефон запретил доступ к сети"));
        break;
    case QAbstractSocket::RemoteHostClosedError:
        deferSocketFailure(russian("Рация или мост закрыли соединение"), true);
        break;
    default:
        deferSocketFailure(russian("Ошибка сети (%1)").arg(static_cast<int>(error)),
                           error != QAbstractSocket::UnsupportedSocketOperationError);
        break;
    }
}

void Client::deferSocketFailure(const QString& status, bool retryable) {
    if (!pendingSocketFailure_.isEmpty())
        return;
    pendingSocketFailure_ = status;
    retrySocketFailure_ = retryable;
    // Symbian can emit a failure before its socket active object finishes.
    QTimer::singleShot(0, this, SLOT(applySocketFailure()));
}

void Client::applySocketFailure() {
    if (!pendingSocketFailure_.isEmpty()) {
        const QString status = pendingSocketFailure_;
        traceConnection("TCP: handle failure outside socket callback");
        finish(status, retrySocketFailure_);
    }
}

void Client::tick() {
    if (!pendingSocketFailure_.isEmpty())
        return;
    const qint64 now = clock_.elapsed();
    if (radioBackupTransaction_ && radioBackupTransaction_->reconnectUntil > 0 &&
        now >= radioBackupTransaction_->reconnectUntil) {
        finish(russian("За 90 секунд не удалось подтвердить ключ после перезапуска"));
        return;
    }
    if (reconnectAt_ > 0 && now >= reconnectAt_) {
        if (wantsConnection_ && !closing_ && socket_->state() == QAbstractSocket::UnconnectedState)
            openLink(true);
        return;
    }
    if (timeout_ > 0 && now >= timeout_) {
        traceConnection("TCP: timeout while waiting for stage", static_cast<int>(wait_));
        if (wait_ == Wait::RadioGps) {
            finishRadioGps(RadioGpsState::Unknown);
            return;
        }
        if (wait_ == Wait::OwnTelemetry) {
            wait_ = Wait::None;
            timeout_ = 0;
            advanceRadioReadings();
            proceed();
            emit changed();
            return;
        }
        if (wait_ == Wait::RadioBackupReboot) {
            finish(russian("Ожидание рации после перезапуска"), true);
            return;
        }
        finish(wait_ == Wait::None ? russian("Не удалось подключиться за 15 секунд")
                                  : russian("Рация не ответила вовремя"), true);
        return;
    }
    console_.tick();
    if (ready_ && wait_ == Wait::None && now >= nextPoll_)
        syncNeeded_ = true;
    proceed();
}

bool Client::write(const meshcore::Bytes& body, Wait wait) {
    auto packet = meshcore::frame(body);
    if (packet.empty()) {
        pendingId_.clear();
        deferSocketFailure(russian("Не удалось сформировать команду"));
        return false;
    }
    wait_ = wait;
    timeout_ = clock_.elapsed() + kReplyTimeout;
    auto bytes = rawBytes(packet);
    const qint64 written = socket_->write(bytes);
    if (body.front() == kImportPrivateKey) {
        bytes.fill('\0');
        std::fill(packet.begin(), packet.end(), 0);
    }
    if (written != static_cast<qint64>(packet.size())) {
        deferSocketFailure(russian("Команду не удалось записать в TCP-соединение"), true);
        return false;
    }
    return true;
}

bool Client::sameRadioBackupEndpoint() const {
    return radioBackupTransaction_ && host_ == radioBackupTransaction_->host && port_ == radioBackupTransaction_->port;
}

void Client::completeRadioBackupKey(int result, const QByteArray& key, const QString& text) {
    const int request = radioBackupKeyRequest_;
    radioBackupKeyRequest_ = 0;
    radioBackupKeyTarget_.clear();
    if (request > 0)
        emit radioBackupKeyResult(request, result, key, text);
}

void Client::completeRadioBackup(const QString& confirmedPublicKey, const QString& error) {
    if (!radioBackupTransaction_)
        return;
    auto transaction = std::move(*radioBackupTransaction_);
    radioBackupTransaction_.reset();
    transaction.key.fill('\0');
    QString result = transaction.failure;
    for (const auto& problem : {error, transaction.skipped}) {
        if (!problem.isEmpty()) {
            if (!result.isEmpty())
                result += russian(". ");
            result += problem;
        }
    }
    const bool success = result.isEmpty() && !confirmedPublicKey.isEmpty();
    if (success) {
        result = russian("Копия восстановлена на рации");
    } else if (transaction.acknowledged > 0) {
        result += russian(". Подтверждено команд: %1 из %2")
            .arg(static_cast<int>(transaction.acknowledged)).arg(static_cast<int>(transaction.writes.size()));
    }
    status_ = result;
    emit radioBackupRestored(transaction.id, success, confirmedPublicKey, result);
}

bool Client::advanceRadioBackup() {
    if (radioBackupKeyRequest_) {
        if (radioKey_ == radioBackupKeyTarget_) {
            write({kExportPrivateKey}, Wait::RadioBackupKey);
            return true;
        }
        completeRadioBackupKey(BackupKeyFailed, {}, russian("Подключена другая рация"));
    }
    if (!radioBackupTransaction_)
        return false;
    auto& transaction = *radioBackupTransaction_;
    if (!sameRadioBackupEndpoint() || radioKey_ != transaction.target) {
        completeRadioBackup({}, russian("Подключена другая рация"));
        return false;
    }
    if (transaction.verifying)
        return true;

    const int total = static_cast<int>(transaction.writes.size());
    const int done = static_cast<int>(transaction.acknowledged);
    if (transaction.failure.isEmpty() && done < total) {
        const auto& next = transaction.writes[done];
        const auto code = next.front();
        if (code == kImportPrivateKey) {
            meshcore::Bytes command{kImportPrivateKey};
            const auto* begin = reinterpret_cast<const std::uint8_t*>(transaction.key.constData());
            command.insert(command.end(), begin, begin + transaction.key.size());
            transaction.key.fill('\0');
            write(command, Wait::RadioBackupWrite);
            std::fill(command.begin(), command.end(), 0);
        } else {
            write(next, Wait::RadioBackupWrite);
        }
        emit radioBackupProgress(transaction.id, radioBackupStage(code), done, total);
        return true;
    }
    if (transaction.keyAccepted) {
        if (write({19, 'r', 'e', 'b', 'o', 'o', 't'}, Wait::RadioBackupReboot)) {
            transaction.verifying = true;
            transaction.reconnectUntil = clock_.elapsed() + kBackupReconnectWindow;
        }
        emit radioBackupProgress(transaction.id, russian("Перезапуск и проверка ключа"), done, total);
        return true;
    }
    if (done == 0 && !transaction.failure.isEmpty()) {
        completeRadioBackup({}, QString());
        return false;
    }
    if (total == 0) {
        completeRadioBackup(radioKey_);
        return false;
    }
    transaction.verifying = true;
    write(meshcore::deviceQuery(), Wait::Device);
    emit radioBackupProgress(transaction.id, russian("Проверка рации"), done, total);
    return true;
}

void Client::readRadioBackupReply(const meshcore::Bytes& frame) {
    const auto code = frame.front();
    const bool rejected = code == meshcore::response::Error || code == kCommandDisabled;
    if (wait_ == Wait::RadioBackupKey) {
        if (!rejected && code != kPrivateKeyReply)
            return;
        wait_ = Wait::None;
        timeout_ = 0;
        if (radioKey_ != radioBackupKeyTarget_) {
            completeRadioBackupKey(BackupKeyFailed, {}, russian("Подключена другая рация"));
        } else if (code == kCommandDisabled || (code == meshcore::response::Error && frame.size() == 2 && frame[1] == 1)) {
            completeRadioBackupKey(BackupKeyUnavailable, {}, russian("Прошивка запрещает чтение закрытого ключа"));
        } else if (code == kPrivateKeyReply && frame.size() == 1 + kPrivateKeyBytes) {
            QByteArray key(reinterpret_cast<const char*>(frame.data() + 1), kPrivateKeyBytes);
            if (radioKeyMatchesPublic(key, QByteArray::fromHex(radioKey_.toLatin1())))
                completeRadioBackupKey(BackupKeyRead, key, QString());
            else
                completeRadioBackupKey(BackupKeyFailed, {}, russian("Ключ в ответе не соответствует рации"));
            key.fill('\0');
        } else {
            completeRadioBackupKey(BackupKeyFailed, {}, russian("Рация не вернула закрытый ключ"));
        }
    } else if (wait_ == Wait::RadioBackupReboot) {
        if (rejected) {
            wait_ = Wait::None;
            timeout_ = 0;
            completeRadioBackup({}, russian("Рация отклонила перезапуск. Новый ключ не проверен"));
        } else if (code == meshcore::response::Ok && frame.size() == 1) {
            deferSocketFailure(russian("Рация перезапускается"), true);
            return;
        } else {
            return;
        }
    } else {
        if (!rejected && !(code == meshcore::response::Ok && frame.size() == 1))
            return;
        if (!radioBackupTransaction_)
            return;
        auto& transaction = *radioBackupTransaction_;
        const auto command = transaction.writes[transaction.acknowledged].front();
        wait_ = Wait::None;
        timeout_ = 0;
        if (rejected) {
            transaction.failure = russian("Рация отклонила запись: %1").arg(radioBackupStage(command));
            if (code == kCommandDisabled)
                transaction.failure += russian(" (запрещено прошивкой)");
            else if (frame.size() == 2 && frame[1] == 3)
                transaction.failure += russian(" (память заполнена)");
        } else {
            ++transaction.acknowledged;
            if (command == kImportPrivateKey)
                transaction.keyAccepted = true;
        }
    }
    proceed();
    emit changed();
}

void Client::updateRadioIdentity(const meshcore::SelfInfo& device) {
    if (self_.key != device.key)
        resetRadioGpsSession();
    radioName_ = fromWire(device.name);
    radioNameBytes_ = device.name.size();
    self_ = device;
    parameters_ = russian("Частота: %1\nПолоса: %2\nМощность: %3 дБм (макс. %4 дБм)\nSF: %5 · CR: %6")
        .arg(device.frequencyKhz == 0 ? russian("—") : decimalMilli(device.frequencyKhz) + russian(" МГц"))
        .arg(device.bandwidthHz == 0 ? russian("—") : decimalMilli(device.bandwidthHz) + russian(" кГц"))
        .arg(device.powerDbm).arg(device.maxPowerDbm)
        .arg(device.spreadingFactor ? QString::number(device.spreadingFactor) : russian("—"))
        .arg(device.codingRate >= 5 && device.codingRate <= 8
             ? QLatin1String("4/") + QString::number(device.codingRate) : russian("—"));
    QSettings settings;
    settings.setValue(QLatin1String("radio/last"), radioKey_);
    settings.setValue(QLatin1String("radio/lastName"), radioName_);
}

void Client::completeRadioSetting(bool success, const QString& text) {
    if (!radioSettingsTransaction_)
        return;
    const int id = radioSettingsTransaction_->id;
    refreshNeeded_ |= radioSettingsTransaction_->refreshContacts;
    radioSettingsTransaction_.reset();
    status_ = text;
    emit radioSettingsResult(id, success, text);
}

void Client::readRadioSettingsReply(const meshcore::Bytes& frame) {
    namespace response = meshcore::response;
    if (!radioSettingsTransaction_)
        return;
    auto& transaction = *radioSettingsTransaction_;
    const auto code = frame.front();
    const bool rejected = code == response::Error || code == 0x0f;
    if (wait_ == Wait::RadioSettingIdentity) {
        const auto identity = meshcore::parseSelf(frame);
        if (!identity && !rejected && code != response::SelfInfo)
            return;
        const bool confirmed = identity && identity->key == self_.key;
        if (confirmed)
            updateRadioIdentity(*identity);
        const QString result = !transaction.error.isEmpty() ? transaction.error
            : confirmed ? russian("Сохранено на рации")
                        : russian("Рация приняла изменения, но не вернула настройки");
        const bool success = transaction.error.isEmpty() && confirmed;
        wait_ = Wait::None;
        timeout_ = 0;
        completeRadioSetting(success, result);
    } else {
        if (code != response::Ok && !rejected)
            return;
        if (code == response::Ok) {
            const auto& command = transaction.commands[transaction.acknowledged++];
            transaction.refreshContacts |= command.front() == 9 || command.front() == 15;
            if (command.front() == 9) {
                for (auto& contact : contacts_) {
                    if (std::equal(contact.key.begin(), contact.key.end(), command.begin() + 1)) {
                        contact.flags = command[34];
                        break;
                    }
                }
            } else if (command.front() == 11 && command.size() == 12) {
                radioRepeatEnabled_ = command[11] != 0;
            } else if (command.front() == 61) {
                pathHashMode_ = command[2];
            }
        } else {
            transaction.error = transaction.acknowledged == 0
                ? russian("Рация отклонила изменение")
                : russian("Рация приняла изменений: %1 из %2")
                    .arg(static_cast<int>(transaction.acknowledged)).arg(static_cast<int>(transaction.commands.size()));
        }
        wait_ = Wait::None;
        timeout_ = 0;
    }
    proceed();
    emit changed();
}

void Client::advanceRadioReadings() {
    ++radioReadingsStage_;
    if (radioReadingsStage_ == 1 && radioProtocolVersion_ < 8)
        radioReadingsStage_ = 3;
    if (radioReadingsStage_ == 3 && radioProtocolVersion_ < 5)
        radioReadingsStage_ = 4;
    if (radioReadingsStage_ >= 4) {
        radioReadingsStage_ = -1;
        const bool hasReadings = nextRadioReadings_.batteryMillivolts || nextRadioReadings_.uptimeSeconds ||
                                 nextRadioReadings_.noiseDbm || nextRadioReadings_.temperatureTenthsC;
        if (hasReadings)
            nextRadioReadings_.capturedAt = QDateTime::currentDateTime().toTime_t();
        radioReadings_ = nextRadioReadings_;
        status_ = hasReadings ? russian("Показания рации обновлены") : russian("Рация не вернула показания");
    }
}

void Client::finishRadioGps(RadioGpsState state) {
    radioGpsState_ = state;
    wait_ = Wait::None;
    timeout_ = 0;
    proceed();
    emit changed();
}

void Client::resetRadioGpsSession() {
    ++radioSessionToken_;
    radioGpsState_ = RadioGpsState::Unknown;
    radioGpsNeeded_ = false;
}

void Client::frameReceived(const meshcore::Bytes& frame) {
    using namespace meshcore;
    if (frame.empty())
        return;
    const auto code = frame.front();

    if (wait_ == Wait::OwnTelemetry && code == kTelemetryReply && frame.size() >= 8 &&
        std::equal(self_.key.begin(), self_.key.begin() + 6, frame.begin() + 2)) {
        const auto values = parseNodeTelemetry(rawBytes(frame).mid(8));
        nextRadioReadings_.temperatureTenthsC = values.temperatureTenthsC;
        if (!nextRadioReadings_.batteryMillivolts && values.batteryMillivolts && *values.batteryMillivolts > 0)
            nextRadioReadings_.batteryMillivolts = values.batteryMillivolts;
        wait_ = Wait::None;
        timeout_ = 0;
        advanceRadioReadings();
        proceed();
        emit changed();
        return;
    }

    if (console_.accept(frame)) {
        if (wait_ == Wait::Sync && (code == response::ContactMessage || code == response::ContactMessageV3))
            write(syncNext(), Wait::Sync);
        else
            proceed();
        return;
    }
    if (code == push::RadioLog) {
        onRadioLog(frame);
        return;
    }

    if (code == push::MessageWaiting) {
        syncNeeded_ = true;
        proceed();
        return;
    }
    if (const auto tag = parseConfirmedTag(frame)) {
        if (acknowledge(*tag)) {
            proceed();
        } else if (*tag != 0 && wait_ == Wait::Send && pendingId_.startsWith(QLatin1String("d/")) &&
            earlyAcks_.size() < kEarlyAckLimit && !earlyAcks_.contains(*tag)) {
            earlyAcks_.append(*tag);
        }
        return;
    }
    if (code == push::NewContact || code == push::ContactDeleted || code == push::PathUpdated) {
        inventoryChanged_ = true;
        proceed();
    }
    if (code >= 0x80)
        return;

    if (code == response::ChannelData) {
        onChannelData(frame);
        if (wait_ == Wait::Sync)
            write(syncNext(), Wait::Sync);
        return;
    }

    if (wait_ != Wait::Sync) {
        if (auto incoming = parseIncoming(frame)) {
            if (wait_ == Wait::Contacts || wait_ == Wait::Channel || !ready_) {
                if (deferredIncoming_.size() < 96)
                    deferredIncoming_.push_back(std::move(*incoming));
            } else {
                onMessage(*incoming);
            }
            return;
        }
    }

    switch (wait_) {
    case Wait::Device:
        if (const auto count = parseChannelCount(frame)) {
            traceConnection("MeshCore: device query answered");
            channelCount_ = *count;
            radioProtocolVersion_ = frame[1];
            radioMaxContacts_ = frame[1] >= 3 && frame[2] != 0 ? frame[2] * 2 : -1;
            radioRepeatEnabled_ = frame.size() > 80 ? std::optional<bool>(frame[80] != 0) : std::nullopt;
            // DeviceInfo appends the path hash mode after the repeat-enabled flag.
            radioPathHashModeKnown_ = frame.size() > 81 && frame[81] <= 3;
            pathHashMode_ = radioPathHashModeKnown_ ? frame[81] : 0;
            firmware_ = fromWire(parseFirmware(frame)).trimmed();
            if (firmware_.isEmpty())
                firmware_ = russian("протокол %1").arg(frame[1]);
            write(appStart(), Wait::Identity);
        } else if (code == response::Error || code == response::DeviceInfo) {
            traceConnection("MeshCore: invalid device query reply");
            deferSocketFailure(russian("Рация не поддерживает MeshCore Device Query"));
        }
        break;
    case Wait::Identity:
        if (const auto device = parseSelf(frame)) {
            traceConnection("MeshCore: radio identity received");
            const QString key = QString::fromLatin1(rawBytes(device->key).toHex().constData());
            if (radioBackupTransaction_ && radioBackupTransaction_->verifying) {
                const auto& transaction = *radioBackupTransaction_;
                const auto& expected = transaction.keyAccepted ? transaction.source : transaction.target;
                if (!sameRadioBackupEndpoint() || key != expected)
                    completeRadioBackup({}, russian("Рация вернула другой ключ. Восстановление не подтверждено"));
            }
            if (key != radioKey_) {
                lastBatteryMillivolts_ = 0;
                radioKey_ = key;
                contacts_.clear();
                channels_.clear();
                loadHistory();
            }
            updateRadioIdentity(*device);
            traceConnection("MeshCore: request contacts");
            requestContacts();
        } else if (code == response::Error || code == response::SelfInfo) {
            traceConnection("MeshCore: invalid radio identity reply");
            deferSocketFailure(russian("Рация отклонила запуск сессии"));
        }
        break;
    case Wait::Contacts:
        if (code == response::ContactsStart) {
            timeout_ = clock_.elapsed() + kReplyTimeout;
        } else if (const auto contact = parseContact(frame)) {
            if (nextContacts_.size() < kContactLimit)
                nextContacts_.push_back(*contact);
            else
                radioContactsOverflow_ = true;
            timeout_ = clock_.elapsed() + kReplyTimeout;
        } else if (code == response::ContactsEnd || code == response::Error) {
            if (!ready_)
                traceConnection("MeshCore: contact list complete, size", static_cast<int>(nextContacts_.size()));
            radioContactsComplete_ = code == response::ContactsEnd && !radioContactsOverflow_;
            if (code == response::ContactsEnd) {
                contacts_.swap(nextContacts_);
                for (const auto& contact : contacts_)
                    targetNames_.insert(directId(prefix(contact.key)), fromWire(contact.name));
            }
            nextContacts_.clear();
            nextChannels_.clear();
            channelIndex_ = 0;
            radioChannelsComplete_ = true;
            loadNextChannel();
        }
        break;
    case Wait::Channel:
        if (const auto channel = parseChannel(frame)) {
            if (channel->index != channelIndex_)
                return;
            if (channel->configured)
                nextChannels_.push_back(*channel);
        } else if (code != response::Error) {
            return;
        } else if (frame.size() != 2 || frame[1] != 2) {
            radioChannelsComplete_ = false;
        }
        ++channelIndex_;
        loadNextChannel();
        break;
    case Wait::Sync:
        if (const auto message = parseIncoming(frame)) {
            onMessage(*message);
        } else if (code == response::NoMoreMessages || code == response::Error) {
            wait_ = Wait::None;
            timeout_ = 0;
            queueStatus_ = code == response::Error ? russian("рация ответила ошибкой")
                : russian("получено %1").arg(directReceived_ + channelReceived_ + dataReceived_ - syncStartCount_);
            status_ = currentManualSync_ ? russian("Очередь: %1").arg(queueStatus_) : idleStatus();
            proceed();
            emit changed();
            break;
        } else if (code != response::ContactMessage && code != response::ChannelMessage &&
                   code != response::ContactMessageV3 && code != response::ChannelMessageV3) {
            break;
        }
        if (wait_ == Wait::Sync)
            write(syncNext(), Wait::Sync);
        break;
    case Wait::Send:
        onSendResult(frame);
        break;
    case Wait::Reaction:
        onReactionResult(frame);
        break;
    case Wait::ResetPath:
        if (code == response::Ok || code == response::Error)
            sendRetry();
        break;
    case Wait::Advert:
    case Wait::Battery:
        if (wait_ == Wait::Advert && (code == response::Ok || code == response::Error || code == 0x0f)) {
            status_ = code == response::Ok ? russian("Анонс принят рацией") : russian("Рация отклонила анонс");
        } else if (wait_ == Wait::Battery && (code == response::Battery || code == response::Error || code == 0x0f)) {
            const auto reading = parseBattery(frame);
            batteryMillivolts_ = reading && reading->millivolts != 65535 ? static_cast<int>(reading->millivolts) : 0;
            if (batteryMillivolts_ > 0) {
                lastBatteryMillivolts_ = batteryMillivolts_;
                nextRadioReadings_.batteryMillivolts = batteryMillivolts_;
            }
            battery_ = russian("Рация не вернула показания.");
            if (reading) {
                battery_ = russian("Аккумулятор: %1").arg(reading->millivolts > 0 && reading->millivolts != 65535
                    ? decimalMilli(reading->millivolts) + russian(" В") : russian("—"));
                if (reading->totalKb)
                    battery_ += russian("\nПамять: %1 из %2 КБ").arg(reading->usedKb).arg(reading->totalKb);
            }
            if (radioReadingsStage_ == 0)
                advanceRadioReadings();
            else
                status_ = reading ? russian("Показания рации обновлены") : battery_;
        } else {
            break;
        }
        wait_ = Wait::None;
        timeout_ = 0;
        proceed();
        emit changed();
        break;
    case Wait::Console:
        break;
    case Wait::SetChannel:
        if (code == response::Ok) {
            channelChangeReload_ = true;
            nextChannels_.clear();
            channelIndex_ = 0;
            loadNextChannel();
        } else if (code == response::Error || code == 0x0f) {
            wait_ = Wait::None;
            timeout_ = 0;
            finishChannelChange(false, russian("Рация отклонила изменение канала"));
            proceed();
        }
        break;
    case Wait::RadioStats: {
        const auto expected = static_cast<std::uint8_t>(radioReadingsStage_ - 1);
        if (code == kStatsReply && frame.size() >= 2 && frame[1] == expected) {
            const QByteArray bytes = rawBytes(frame);
            if (expected == 0 && bytes.size() >= 11) {
                const auto mv = littleEndian(bytes, 2, 2);
                if (!nextRadioReadings_.batteryMillivolts && mv > 0 && mv != 65535)
                    nextRadioReadings_.batteryMillivolts = static_cast<int>(mv);
                nextRadioReadings_.uptimeSeconds = littleEndian(bytes, 4, 4);
            } else if (expected == 1 && bytes.size() >= 14) {
                nextRadioReadings_.noiseDbm = static_cast<qint16>(littleEndian(bytes, 2, 2));
                nextRadioReadings_.transmitSeconds = littleEndian(bytes, 6, 4);
                nextRadioReadings_.receiveSeconds = littleEndian(bytes, 10, 4);
            }
        } else if (code != response::Error && code != 0x0f) {
            break;
        }
        advanceRadioReadings();
        wait_ = Wait::None;
        timeout_ = 0;
        proceed();
        emit changed();
        break;
    }
    case Wait::OwnTelemetry:
        if (code == response::Error || code == 0x0f) {
            advanceRadioReadings();
            wait_ = Wait::None;
            timeout_ = 0;
            proceed();
            emit changed();
        }
        break;
    case Wait::RadioGps:
        if (code == kCustomVarsReply) {
            finishRadioGps(parseRadioGps(frame));
        } else if (code == response::Error || code == kCommandDisabled) {
            const bool unsupported = code == kCommandDisabled || frame.size() == 1 || frame[1] == 1;
            finishRadioGps(unsupported ? RadioGpsState::Unsupported : RadioGpsState::Unknown);
        }
        break;
    case Wait::RadioSetting:
    case Wait::RadioSettingIdentity:
        readRadioSettingsReply(frame);
        break;
    case Wait::RadioBackupKey:
    case Wait::RadioBackupWrite:
    case Wait::RadioBackupReboot:
        readRadioBackupReply(frame);
        break;
    case Wait::RestartRadio:
        if (code == response::Error || code == 0x0f) {
            status_ = russian("Рация отклонила команду перезапуска");
            wait_ = Wait::None;
            timeout_ = 0;
            proceed();
            emit changed();
        }
        break;
    case Wait::None:
        break;
    }
}

void Client::requestContacts() {
    nextContacts_.clear();
    radioContactsOverflow_ = false;
    status_ = russian("Читаю контакты и каналы…");
    if (write(meshcore::getContacts(), Wait::Contacts))
        emit changed();
}

void Client::loadNextChannel() {
    if (channelIndex_ < channelCount_) {
        write(meshcore::getChannel(static_cast<std::uint8_t>(channelIndex_)), Wait::Channel);
        return;
    }

    channels_.swap(nextChannels_);
    nextChannels_.clear();
    assignPublicAliases();
    for (const auto& channel : channels_) {
        targetNames_.insert(channelId(channel), channel.name.empty()
            ? russian("Канал %1").arg(channel.index + 1) : fromWire(channel.name));
    }
    bool historyChanged = false;
    for (const auto& channel : channels_) {
        if (channelAliases_[channel.index] == channel.index)
            continue;
        const auto alias = channelId(channel);
        const auto canonical = channelId(*channelForIndex(channel.index));
        for (auto& row : history_) {
            if (row.target == alias) {
                row.target = canonical;
                historyChanged = true;
            }
        }
        if (const int count = unread_.take(alias)) {
            unread_[canonical] += count;
            historyChanged = true;
        }
        for (auto& watch : echoWatches_) {
            if (watch.id == alias)
                watch.id = canonical;
        }
        for (auto& request : outgoing_) {
            if (request.id == alias)
                request.id = canonical;
        }
        for (auto& reaction : deferredReactions_) {
            if (reaction.id == alias)
                reaction.id = canonical;
        }
        if (reactionRequest_ && reactionRequest_->id == alias)
            reactionRequest_->id = canonical;
    }
    if (historyChanged)
        saveHistory();

    wait_ = Wait::None;
    timeout_ = 0;
    nextInventoryRefresh_ = clock_.elapsed() + kInventoryRefreshInterval;
    if (!ready_) {
        traceConnection("MeshCore: first session ready");
        readySince_ = clock_.elapsed();
    }
    ready_ = true;
    retryingConnection_ = false;
    reconnectAttempt_ = 0;
    while (!deferredIncoming_.empty() && ready_) {
        auto incoming = std::move(deferredIncoming_.front());
        deferredIncoming_.pop_front();
        onMessage(incoming);
    }
    while (!deferredChannelData_.empty() && ready_) {
        auto frame = std::move(deferredChannelData_.front());
        deferredChannelData_.pop_front();
        onChannelData(frame);
    }
    if (channelChangeReload_)
        finishChannelChange(true, russian("Канал сохранён на рации"));
    if (radioBackupTransaction_ && radioBackupTransaction_->verifying) {
        const QString issue = radioContactsComplete_ && radioChannelsComplete_ ? QString()
            : russian("Не удалось перечитать контакты или каналы рации");
        completeRadioBackup(radioKey_, issue);
    }
    if (ready_ && !busy())
        syncQueue();
}

void Client::assignPublicAliases() {
    const auto factoryKey = QByteArray::fromHex("8b3387e9c5cdea6ac9e5edbaa115cd72");
    const meshcore::Channel* publicChannel = nullptr;
    channelAliases_.fill(-1);
    for (const auto& channel : channels_) {
        channelAliases_[channel.index] = channel.index;
        if (fromWire(channel.name).compare(QLatin1String("Public"), Qt::CaseInsensitive) == 0 &&
            (!publicChannel || (rawBytes(publicChannel->key) != factoryKey && rawBytes(channel.key) == factoryKey))) {
            publicChannel = &channel;
        }
    }
    publicStatus_ = russian("нет среди каналов рации");
    if (!publicChannel)
        return;
    const bool hasKey = std::any_of(publicChannel->key.begin(), publicChannel->key.end(),
                                    [](auto byte) { return byte != 0; });
    int aliases = 0;
    if (hasKey) {
        for (const auto& channel : channels_) {
            if (channel.index != publicChannel->index && channel.key == publicChannel->key) {
                channelAliases_[channel.index] = publicChannel->index;
                ++aliases;
            }
        }
    }
    publicStatus_ = russian("слот %1, %2").arg(publicChannel->index)
        .arg(!hasKey ? russian("пустой ключ") : rawBytes(publicChannel->key) == factoryKey
             ? russian("стандартный ключ") : russian("другой ключ"));
    if (aliases)
        publicStatus_ += russian(", совпадающих слотов: %1").arg(aliases);
}

void Client::syncQueue() {
    syncNeeded_ = false;
    currentManualSync_ = manualSync_;
    manualSync_ = false;
    syncStartCount_ = directReceived_ + channelReceived_ + dataReceived_;
    nextPoll_ = clock_.elapsed() + kPollInterval;
    status_ = russian("Получаю сообщения…");
    if (write(meshcore::syncNext(), Wait::Sync))
        emit changed();
}

void Client::proceed() {
    if (!ready_ || busy() || closing_ || !pendingSocketFailure_.isEmpty())
        return;
    if (advanceRadioBackup())
        return;
    if (radioSettingsTransaction_) {
        const auto& transaction = *radioSettingsTransaction_;
        const bool accepted = transaction.error.isEmpty();
        if (accepted && transaction.acknowledged < transaction.commands.size()) {
            write(transaction.commands[transaction.acknowledged], Wait::RadioSetting);
            return;
        }
        if (transaction.readBackIdentity && transaction.acknowledged > 0) {
            write(meshcore::appStart(), Wait::RadioSettingIdentity);
            return;
        }
        const QString text = accepted ? russian("Сохранено на рации") : transaction.error;
        completeRadioSetting(accepted, text);
        if (busy() || !ready_ || closing_)
            return;
    }
    if (syncNeeded_) {
        syncQueue();
    } else if (consoleRequest_ && console_.canStart(consoleRequest_->operation)) {
        startConsole();
    } else if (!channelChangeCommand_.empty()) {
        auto command = std::move(channelChangeCommand_);
        channelChangeCommand_.clear();
        write(command, Wait::SetChannel);
        std::fill(command.begin(), command.end(), 0);
    } else if (advertNeeded_) {
        advertNeeded_ = false;
        write(meshcore::advertise(), Wait::Advert);
    } else if (radioReadingsStage_ == 0 || batteryNeeded_) {
        batteryNeeded_ = false;
        write(meshcore::getBattery(), Wait::Battery);
    } else if (radioReadingsStage_ == 1 || radioReadingsStage_ == 2) {
        write({56, static_cast<std::uint8_t>(radioReadingsStage_ - 1)}, Wait::RadioStats);
    } else if (radioReadingsStage_ == 3) {
        if (write({39, 0, 0, 0}, Wait::OwnTelemetry))
            timeout_ = clock_.elapsed() + 2500;
    } else if (radioGpsNeeded_) {
        radioGpsNeeded_ = false;
        if (write({kGetCustomVars}, Wait::RadioGps))
            timeout_ = clock_.elapsed() + kRadioGpsReplyTimeout;
    } else if (reactionRequest_) {
        startReaction();
    } else if (retryExpired() || sendNext()) {
        return;
    } else if (refreshNeeded_ || (inventoryChanged_ && clock_.elapsed() >= nextInventoryRefresh_)) {
        refreshNeeded_ = false;
        inventoryChanged_ = false;
        requestContacts();
    }
}

bool Client::acknowledge(quint32 tag) {
    if (tag == 0)
        return false;
    StoredMessage* match = nullptr;
    for (auto& row : history_) {
        if (row.ackTags.contains(tag)) {
            if (match)
                return false;
            match = &row;
        }
    }
    if (!match)
        return false;
    match->deadline = 0;
    match->retryAt = 0;
    match->ackTags.clear();
    match->message.delivery = delivered();
    saveHistory();
    emit changed();
    return true;
}

Client::StoredMessage* Client::pendingReceipt() {
    if (!pendingRetry_)
        return nullptr;
    for (auto& row : history_) {
        if (row.message.outgoing && row.target == pendingId_ && row.message.timestamp == pendingMessage_.timestamp)
            return &row;
    }
    return nullptr;
}

bool Client::retryExpired() {
    const auto now = clock_.elapsed();
    bool changedHistory = false;
    for (auto& row : history_) {
        if ((!row.deadline && !row.retryAt) || row.deadline > now)
            continue;
        const auto contact = contactForId(row.target);
        if (row.attemptLimit == 0)
            row.attemptLimit = sendTries_;
        row.deadline = 0;
        if (row.attempts < row.attemptLimit && contact && contact->type == 1 && contact->key == row.recipient) {
            if (row.retryAt > now)
                continue;
            int awaitingConfirmation = 0;
            bool recipientBusy = false;
            for (const auto& active : history_) {
                if (active.deadline > now) {
                    ++awaitingConfirmation;
                    recipientBusy = recipientBusy || active.target == row.target;
                }
            }
            if (recipientBusy || awaitingConfirmation >= kDirectAckLimit) {
                row.retryAt = now;
                continue;
            }
            ++row.attempts;
            row.retryAt = 0;
            row.message.delivery = russian("Повторная отправка");
            pendingId_ = row.target;
            pendingMessage_ = row.message;
            pendingKey_ = row.recipient;
            pendingRetry_ = true;
            earlyAcks_.clear();
            const bool flood = row.flood;
            saveHistory();
            if (flood)
                sendRetry();
            else
                write(meshcore::resetPath(pendingKey_), Wait::ResetPath);
            emit changed();
            return true;
        }
        row.retryAt = 0;
        row.message.delivery = unconfirmed();
        changedHistory = true;
    }
    if (changedHistory) {
        saveHistory();
        emit changed();
    }
    return false;
}

void Client::sendRetry() {
    const auto row = pendingReceipt();
    if (!row || row->message.delivery == delivered()) {
        pendingRetry_ = false;
        pendingId_.clear();
        wait_ = Wait::None;
        timeout_ = 0;
        proceed();
        return;
    }
    const QByteArray utf8 = pendingMessage_.text.toUtf8();
    if (row->attempts < row->attemptLimit) {
        // Stagger radios sharing the same backoff so their retries do not line up.
        const uint spread = 80 + qHash(rawBytes(self_.key) + QByteArray::number(row->message.timestamp) +
                                     QByteArray::number(row->attempts)) % 41;
        row->retryAt = clock_.elapsed() + kRetryGaps[row->attempts] * spread / 100;
    }
    write(meshcore::sendDirect(meshcore::prefix(pendingKey_),
         {utf8.constData(), static_cast<std::size_t>(utf8.size())}, pendingMessage_.timestamp,
         static_cast<std::uint8_t>(row->attempts - 1)), Wait::Send);
}

const meshcore::Contact* Client::relayForHash(const QString& hash) const {
    const meshcore::Contact* match = nullptr;
    const meshcore::Contact* repeater = nullptr;
    int matches = 0;
    int repeaters = 0;
    for (const auto& contact : contacts_) {
        const QString key = QString::fromLatin1(rawBytes(meshcore::prefix(contact.key)).toHex().constData());
        if (!key.startsWith(hash))
            continue;
        match = &contact;
        ++matches;
        if (contact.type == 2 || contact.type == 3) {
            repeater = &contact;
            ++repeaters;
        }
    }
    return repeaters == 1 ? repeater : matches == 1 ? match : nullptr;
}

QString Client::relayName(const QString& hash) const {
    const auto node = relayForHash(hash);
    return node && !node->name.empty() ? fromWire(node->name) : QLatin1String("#") + hash.toUpper();
}

void Client::pruneEchoes(qint64 now) {
    while (!heard_.empty() && now - heard_.front().at > kHeardLifetime)
        heard_.pop_front();
    while (!echoWatches_.empty() && now - echoWatches_.front().sentAt > kEchoLifetime)
        echoWatches_.pop_front();
}

bool Client::addEcho(ChannelWatch& watch, const HeardSample& sample) {
    const auto& packet = sample.packet;
    if (packet.hops == 0 || sample.at < watch.sentAt || packet.payload != watch.payload)
        return false;
    const auto hash = hashText(packet.path, 0, packet.hashWidth);
    const int snr = packet.hops == 1 ? packet.snrQuarterDb : kMissingSnr;
    auto relay = watch.relays.find(hash);
    if (relay == watch.relays.end()) {
        if (const auto node = relayForHash(hash)) {
            for (auto known = watch.relays.begin(); known != watch.relays.end(); ++known) {
                if ((known.key().startsWith(hash) || hash.startsWith(known.key())) && relayForHash(known.key()) == node) {
                    relay = known;
                    break;
                }
            }
        }
    }
    if (relay != watch.relays.end()) {
        if (relay.value() >= snr)
            return false;
        relay.value() = snr;
        return true;
    }
    if (watch.relays.size() >= kRepeatersLimit) {
        if (watch.overflow)
            return false;
        watch.overflow = true;
        return true;
    }
    watch.relays.insert(hash, snr);
    return true;
}

bool Client::showEcho(const ChannelWatch& watch) {
    const int count = watch.relays.size();
    const int last = count % 10;
    const int lastTwo = count % 100;
    const auto unit = lastTwo >= 11 && lastTwo <= 14 ? russian("ретрансляторов")
        : last == 1 ? russian("ретранслятор") : last >= 2 && last <= 4 ? russian("ретранслятора")
        : russian("ретрансляторов");
    const QString number = watch.overflow ? russian("не менее %1").arg(count) : QString::number(count);
    QString detail = russian("Вас услышали · %1 %2").arg(number, unit);
    int names = 0;
    for (auto relay = watch.relays.constBegin(); relay != watch.relays.constEnd() && names < kRouteNameLimit;
         ++relay, ++names) {
        detail += QLatin1Char('\n') + relayName(relay.key());
        if (relay.value() != kMissingSnr)
            detail += russian(" · SNR ") + snrText(relay.value());
    }
    if (count > names)
        detail += russian("\nЕщё %1").arg(count - names);
    for (auto& row : history_) {
        if (row.message.outgoing && row.target == watch.id && row.message.timestamp == watch.stamp) {
            row.message.relayCount = count;
            row.message.relayCountPartial = watch.overflow;
            row.message.routeDetails = detail;
            saveHistory();
            return true;
        }
    }
    return false;
}

void Client::onRadioLog(const meshcore::Bytes& frame) {
    if (frame.size() > 3 && receivers(SIGNAL(radioPacketReceived(int,int,int,int,int,int))) > 0) {
        const int route = frame[3] & 3;
        const std::size_t descriptor = route == 0 || route == 3 ? 8 : 4;
        int type = -1;
        int hops = -1;
        if (descriptor < frame.size()) {
            const int count = frame[descriptor] & 63;
            const int hashWidth = (frame[descriptor] >> 6) + 1;
            const auto pathBytes = static_cast<std::size_t>(count * hashWidth);
            if (pathBytes <= 64 && descriptor + 1 + pathBytes < frame.size()) {
                type = (frame[3] >> 2) & 15;
                hops = count;
            }
        }
        emit radioPacketReceived(static_cast<qint8>(frame[1]), static_cast<qint8>(frame[2]),
                                 type, hops, static_cast<int>(frame.size() - 3), type >= 0 ? route : -1);
    }
    if (frame.size() > 3 && ((frame[3] >> 2) & 15) == 5)
        ++groupHeard_;
    auto packet = meshcore::parseGroupEcho(frame);
    if (!packet)
        return;
    const auto now = clock_.elapsed();
    pruneEchoes(now);
    if (heard_.size() >= kEchoLimit)
        heard_.pop_front();
    heard_.push_back(HeardSample{std::move(*packet), now});
    bool changedRoutes = false;
    for (auto& watch : echoWatches_) {
        if (addEcho(watch, heard_.back()))
            changedRoutes = showEcho(watch) || changedRoutes;
    }
    if (changedRoutes)
        emit changed();
}

QString Client::receiveRoute(const meshcore::Incoming& message, const meshcore::Channel* channel) const {
    QString result;
    const int hops = message.path == 255 ? -1 : message.path & 63;
    if (hops >= 0)
        result = russian("Переходов: %1").arg(hops);
    else if (!message.channel)
        result = russian("Прямой маршрут");
    if (!message.snrQuarterDb)
        return result;
    if (!result.isEmpty())
        result += russian(" · ");
    result += russian("SNR ") + snrText(*message.snrQuarterDb);
    if (!channel || hops <= 0)
        return result;
    const auto payload = meshcore::groupTextPayload(channel->key, message.timestamp, message.text);
    const auto now = clock_.elapsed();
    const meshcore::HeardGroup* match = nullptr;
    const int width = (message.path >> 6) + 1;
    for (const auto& sample : heard_) {
        const auto& packet = sample.packet;
        if (now - sample.at > kHeardLifetime || packet.hops != hops || packet.hashWidth != width ||
            packet.snrQuarterDb != *message.snrQuarterDb || packet.payload != payload) {
            continue;
        }
        if (match && match->path != packet.path)
            return result;
        match = &packet;
    }
    if (match) {
        result += QLatin1Char('\n');
        for (int i = 0; i < hops; ++i) {
            if (i)
                result += russian(" → ");
            result += relayName(hashText(match->path, i * width, width));
        }
    }
    return result;
}

void Client::onMessage(const meshcore::Incoming& message) {
    QString id;
    QString author;
    QString signer;
    const meshcore::Channel* channel = nullptr;
    const QString wireText = fromWire(message.text);
    QString text = wireText;
    if (text.isEmpty())
        return;
    if (message.channel) {
        ++channelReceived_;
        channel = channelForIndex(message.channelIndex);
        id = channel ? channelId(*channel) : unknownChannelId(message.channelIndex);
        if (channel && fromWire(channel->name).compare(QLatin1String("Public"), Qt::CaseInsensitive) == 0)
            ++publicReceived_;
        const int separator = text.indexOf(QLatin1String(": "));
        if (separator > 0 && separator + 2 < text.size()) {
            author = text.left(separator);
            text = text.mid(separator + 2);
        }
        if (!author.isEmpty()) {
            const auto body = text.toUtf8();
            if (const auto reaction = meshcore::parseChannelTextReaction(
                    {body.constData(), static_cast<std::size_t>(body.size())})) {
                takeReaction(PendingReaction{id, reaction->target,
                    ReactionEntry{fromWire(reaction->emoji), author, message.timestamp, false},
                    fromWire(reaction->to), false});
                return;
            }
        }
    } else {
        id = directId(message.sender);
        author = targetTitle(id);
        if (message.signer) {
            signer = QString::fromLatin1(rawBytes(*message.signer).toHex().constData());
            const QString name = roomAuthor(signer);
            author = name.isEmpty() ? signer : name;
        }
        if (message.textType == 1) {
            const auto contact = contactForId(id);
            if (contact && contact->type == 1) {
                if (const auto reaction = meshcore::parseDirectReaction(message.text)) {
                    takeReaction(PendingReaction{id, reaction->target,
                        ReactionEntry{fromWire(reaction->emoji), id, message.timestamp, false}, {}});
                }
            }
            return;
        }
        ++directReceived_;
    }
    for (const auto& entry : history_) {
        if (entry.target == id && !entry.message.outgoing && entry.message.timestamp == message.timestamp &&
            ((entry.message.text == text && entry.message.roomSigner == signer &&
              (!signer.isEmpty() || entry.message.author == author)) ||
             (message.channel && entry.message.author.isEmpty() && entry.message.text == wireText))) {
            return;
        }
    }
    ChatMessage received{author, text, message.timestamp, false, QString()};
    received.roomSigner = signer;
    received.unread = !blocks_.contains(senderBlockId(id, received));
    received.hops = message.path == 255 ? -1 : message.path & 63;
    received.routeDetails = receiveRoute(message, channel);
    addMessage(id, received);
    if (received.unread) {
        const auto* contact = message.channel ? nullptr : contactForId(id);
        emit incomingChatMessage(id, received.text, !signer.isEmpty() || (contact && contact->type == 3));
        emit incomingMessage();
    }
    emit changed();
}

void Client::settleSend(const QString& delivery) {
    if (pendingId_.isEmpty())
        return;
    if (pendingRetry_) {
        if (auto row = pendingReceipt(); row && row->message.delivery != delivered()) {
            row->message.delivery = delivery;
            row->deadline = 0;
            row->retryAt = 0;
            row->ackTags.clear();
            saveHistory();
        }
    } else {
        pendingMessage_.delivery = delivery;
        addMessage(pendingId_, pendingMessage_);
    }
    pendingId_.clear();
    pendingRetry_ = false;
    earlyAcks_.clear();
    pendingEcho_.clear();
}

void Client::onSendResult(const meshcore::Bytes& frame) {
    const auto code = frame.front();
    const bool channel = pendingId_.startsWith(QLatin1String("c/"));
    const auto tag = meshcore::parseSentTag(frame);
    if (code != meshcore::response::Error && !(channel && code == meshcore::response::Ok) && !(!channel && tag))
        return;
    if (pendingRetry_) {
        if (auto row = pendingReceipt(); row && row->message.delivery != delivered()) {
            if (tag && *tag != 0 && earlyAcks_.contains(*tag)) {
                row->message.delivery = delivered();
                row->deadline = 0;
                row->retryAt = 0;
                row->ackTags.clear();
            } else if (tag && *tag != 0) {
                if (!row->ackTags.contains(*tag))
                    row->ackTags.append(*tag);
                row->deadline = clock_.elapsed() + ackWait(frame);
                row->flood = frame[1] != 0;
                row->message.delivery = waitingForAck();
            } else if (tag && !row->retryAt) {
                row->message.delivery = accepted();
            } else {
                // A late acknowledgement for an earlier attempt can still arrive.
                row->deadline = clock_.elapsed() + 3000;
            }
            saveHistory();
        }
    } else if (code == meshcore::response::Error) {
        pendingMessage_.delivery = russian("Рация отклонила сообщение (%1)").arg(frame.size() > 1 ? frame[1] : 0);
        addMessage(pendingId_, pendingMessage_);
    } else if (channel || *tag == 0) {
        pendingMessage_.delivery = accepted();
        addMessage(pendingId_, pendingMessage_);
        if (channel && !pendingEcho_.empty()) {
            pruneEchoes(clock_.elapsed());
            if (echoWatches_.size() >= kEchoLimit)
                echoWatches_.pop_front();
            echoWatches_.push_back(ChannelWatch{pendingId_, pendingMessage_.timestamp,
                                               std::move(pendingEcho_), pendingEchoFrom_, {}});
            auto& watch = echoWatches_.back();
            for (const auto& sample : heard_)
                addEcho(watch, sample);
            if (!watch.relays.isEmpty())
                showEcho(watch);
        }
    } else if (earlyAcks_.contains(*tag)) {
        pendingMessage_.delivery = delivered();
        addMessage(pendingId_, pendingMessage_);
    } else {
        pendingMessage_.delivery = waitingForAck();
        const auto deadline = clock_.elapsed() + ackWait(frame);
        addMessage(pendingId_, pendingMessage_, *tag, deadline, frame[1] != 0);
    }
    pendingId_.clear();
    pendingRetry_ = false;
    pendingEcho_.clear();
    earlyAcks_.clear();
    wait_ = Wait::None;
    timeout_ = 0;
    status_ = idleStatus();
    proceed();
    emit changed();
}

void Client::finish(const QString& status, bool retryable) {
    if (closing_)
        return;
    closing_ = true;
    resetRadioGpsSession();
    pendingSocketFailure_.clear();
    retrySocketFailure_ = false;
    if (!pendingId_.isEmpty())
        settleSend(wait_ == Wait::ResetPath ? unconfirmed() : resultUnknown());
    if (!outgoing_.empty()) {
        for (const auto& request : outgoing_) {
            ChatMessage message{radioName_, request.text, nextTimestamp(), true,
                                russian("Не отправлено: соединение закрыто")};
            history_.append(StoredMessage{request.id, message});
        }
        if (history_.size() > kHistoryLimit) {
            history_.remove(0, history_.size() - kHistoryLimit);
            refreshUnread();
        }
        outgoing_.clear();
        saveHistory();
    }
    if (consoleRequest_) {
        const int id = consoleRequest_->id;
        const int operation = consoleRequest_->operation;
        consoleRequest_->input.fill(QLatin1Char('\0'));
        consoleRequest_.reset();
        finishConsoleRequest(id, operation, false, {}, russian("Соединение закрыто"));
    }
    console_.close();
    finishChannelChange(false, russian("Соединение закрыто, результат изменения канала неизвестен"));
    completeRadioSetting(false, russian("Соединение закрыто, результат изменений неизвестен"));
    completeRadioBackupKey(BackupKeyFailed, {}, status);
    const bool verifyingBackupAfterReboot = retryable && wantsConnection_ && sameRadioBackupEndpoint() &&
        radioBackupTransaction_->reconnectUntil > clock_.elapsed();
    if (radioBackupTransaction_ && !verifyingBackupAfterReboot) {
        QString error = status;
        const auto& transaction = *radioBackupTransaction_;
        if (wait_ == Wait::RadioBackupWrite && transaction.acknowledged < transaction.writes.size()) {
            error += russian(". Результат записи неизвестен: %1")
                .arg(radioBackupStage(transaction.writes[transaction.acknowledged].front()));
        }
        if (transaction.keyAccepted && transaction.reconnectUntil == 0) {
            error += russian(". Рация приняла ключ. Подтвердить его после перезапуска не удалось");
        }
        completeRadioBackup({}, error);
    }

    const bool shouldReconnect = verifyingBackupAfterReboot || (retryable && autoReconnect_ && wantsConnection_);
    if (!shouldReconnect)
        invalidatePending();
    earlyAcks_.clear();
    ready_ = false;
    radioContactsComplete_ = false;
    radioChannelsComplete_ = false;
    wait_ = Wait::None;
    timeout_ = 0;
    syncNeeded_ = false;
    refreshNeeded_ = false;
    inventoryChanged_ = false;
    manualSync_ = false;
    advertNeeded_ = false;
    batteryNeeded_ = false;
    radioReadingsStage_ = -1;
    radioReadings_ = {};
    nextRadioReadings_ = {};
    heard_.clear();
    echoWatches_.clear();
    deferredIncoming_.clear();
    deferredChannelData_.clear();
    deferredReactions_.clear();
    reactionRequest_.reset();
    decoder_.reset();
    nextContacts_.clear();
    nextChannels_.clear();
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        traceConnection("TCP: abort active socket");
        socket_->abort();
        traceConnection("TCP: active socket aborted");
    }
#ifdef Q_OS_SYMBIAN
    wlan_->reset();
#endif
    batteryMillivolts_ = 0;
    status_ = status;
    reconnectAt_ = 0;
    retryingConnection_ = false;
    if (shouldReconnect) {
        const auto now = clock_.elapsed();
        if (readySince_ > 0 && now - readySince_ >= kStableLink)
            reconnectDelay_ = kInitialReconnectDelay;
        lastFailure_ = status;
        status_ += russian(" · переподключение через %1 с").arg(reconnectDelay_ / 1000);
        reconnectAt_ = now + reconnectDelay_;
        reconnectDelay_ = std::min(kMaximumReconnectDelay, reconnectDelay_ * 2);
        if (reconnectAttempt_ < std::numeric_limits<int>::max())
            ++reconnectAttempt_;
    } else {
        wantsConnection_ = false;
        reconnectAttempt_ = 0;
        lastBatteryMillivolts_ = 0;
    }
    readySince_ = 0;
    closing_ = false;
    emit changed();
}

void Client::addMessage(const QString& target, const ChatMessage& message,
                        quint32 tag, qint64 deadline, bool flood) {
    StoredMessage row{target, message};
    row.deadline = deadline;
    if (tag != 0) {
        row.ackTags.append(tag);
        row.recipient = pendingKey_;
        row.flood = flood;
    }
    const auto utf8 = message.text.toUtf8();
    row.reactionHash = meshcore::reactionHash({utf8.constData(), static_cast<std::size_t>(utf8.size())}, message.timestamp);
    adoptReactions(row);
    const bool pendingSend = message.outgoing && !pendingRetry_ && target == pendingId_;
    if (pendingSend)
        history_.insert(qMin(pendingHistoryPosition_, history_.size()), row);
    else
        history_.append(row);
    const int overflow = history_.size() - kHistoryLimit;
    if (overflow > 0) {
        history_.remove(0, overflow);
        pendingHistoryPosition_ = qMax(0, pendingHistoryPosition_ - overflow);
    }
    refreshUnread();
    saveHistory();
}

void Client::refreshUnread() {
    unread_.clear();
    for (const auto& row : history_) {
        if (row.message.unread)
            ++unread_[row.target];
    }
}

void Client::invalidatePending() {
    bool historyChanged = false;
    for (auto& entry : history_) {
        entry.ackTags.clear();
        if (entry.deadline || entry.retryAt) {
            entry.deadline = 0;
            entry.retryAt = 0;
            entry.message.delivery = unconfirmed();
            historyChanged = true;
        }
    }
    if (historyChanged)
        saveHistory();
}

void Client::loadHistory() {
    history_.clear();
    lastOutgoingTimestamp_ = 0;
    unread_.clear();
    targetNames_.clear();
    if (radioKey_.size() != 64)
        return;
    QSettings settings;
    const QByteArray content =
        settings.value(QString::fromLatin1("radios/%1/history").arg(radioKey_)).toByteArray();
    if (content.size() > 2 * 1024 * 1024)
        return;

    QDataStream stream(content);
    stream.setVersion(QDataStream::Qt_4_6);
    quint32 version = 0;
    quint32 count = 0;
    stream >> version >> count;
    if (version < 1 || version > 4 || count > kHistoryLimit)
        return;
    QVector<StoredMessage> records;
    QMap<QString, QString> names;
    quint32 lastOutgoingTimestamp = 0;
    for (quint32 i = 0; i < count; ++i) {
        StoredMessage record{};
        QString targetName;
        bool unread;
        stream >> record.target >> targetName >> record.message.author >> record.message.text
               >> record.message.timestamp >> record.message.outgoing >> record.message.delivery
               >> unread;
        if (version >= 2) {
            qint32 relayCount = 0;
            qint32 hops = -1;
            stream >> relayCount >> hops >> record.message.routeDetails;
            record.message.relayCount = relayCount > 0 ? relayCount : -1;
            record.message.hops = hops;
        }
        if (version >= 3) {
            quint16 reactions = 0;
            stream >> record.message.relayCountPartial >> reactions;
            if (reactions > kMessageReactionLimit) {
                unread_.clear();
                return;
            }
            for (quint16 r = 0; r < reactions; ++r) {
                ReactionEntry entry{};
                stream >> entry.emoji >> entry.actor >> entry.stamp >> entry.own;
                if (entry.own)
                    lastOutgoingTimestamp = std::max(lastOutgoingTimestamp, entry.stamp);
                record.reactions.append(entry);
            }
        }
        if (version >= 4)
            stream >> record.message.roomSigner;
        if (stream.status() != QDataStream::Ok) {
            unread_.clear();
            return;
        }
        if (record.message.delivery == waitingForAck() || record.message.delivery == russian("Повторная отправка"))
            record.message.delivery = unconfirmed();
        if (!targetName.isEmpty())
            names.insert(record.target, targetName);
        if (record.message.outgoing)
            lastOutgoingTimestamp = std::max(lastOutgoingTimestamp, record.message.timestamp);
        if (version == 1 && !record.message.outgoing && record.message.author.isEmpty() &&
            record.target.startsWith(QLatin1String("c/"))) {
            int delimiter = record.message.text.indexOf(QLatin1String(": "));
            if (delimiter > 0 && delimiter + 2 < record.message.text.size()) {
                record.message.author = record.message.text.left(delimiter);
                record.message.text = record.message.text.mid(delimiter + 2);
            }
        }
        record.message.unread = unread && !record.message.outgoing &&
            !blocks_.contains(senderBlockId(record.target, record.message));
        const auto body = record.message.text.toUtf8();
        record.reactionHash = meshcore::reactionHash({body.constData(), static_cast<std::size_t>(body.size())},
                                                    record.message.timestamp);
        records.append(record);
    }
    history_.swap(records);
    refreshUnread();
    targetNames_.swap(names);
    lastOutgoingTimestamp_ = lastOutgoingTimestamp;
}

QString Client::localHistoryRadioKey() const {
    return !history_.isEmpty() && radioKey_.size() == 64 ? radioKey_ : QString();
}

bool Client::canLoadRadioBackupHistory(const QString& key) const {
    return !closing_ && pendingSocketFailure_.isEmpty() && !connecting() && !reconnecting() &&
           key == radioKey_ && validRadioBackupKey(key) && !radioBackupTransaction_ &&
           pendingId_.isEmpty() && outgoing_.empty() &&
           std::none_of(history_.constBegin(), history_.constEnd(), [](const StoredMessage& message) {
               return message.deadline > 0 || message.retryAt > 0 || !message.ackTags.isEmpty();
           });
}

bool Client::reloadRadioBackupHistory(const QString& key) {
    if (!canLoadRadioBackupHistory(key))
        return false;
    loadHistory();
    for (const auto& contact : contacts_)
        targetNames_.insert(directId(meshcore::prefix(contact.key)), fromWire(contact.name));
    QSet<QString> activeChannels;
    QMap<QString, QString> channelsByHash;
    for (const auto& channel : channels_) {
        const QString id = channelId(channel);
        targetNames_.insert(id, channel.name.empty()
            ? russian("Канал %1").arg(channel.index + 1) : fromWire(channel.name));
        if (!ready_)
            continue;
        activeChannels.insert(id);
        const auto* canonical = channelForIndex(channel.index);
        const QString hash = id.mid(id.lastIndexOf(QLatin1Char('/')) + 1);
        const QString target = canonical ? channelId(*canonical) : id;
        const auto previous = channelsByHash.constFind(hash);
        if (previous == channelsByHash.constEnd())
            channelsByHash.insert(hash, target);
        else if (previous.value() != target)
            channelsByHash[hash].clear();
    }
    bool remapped = false;
    for (auto& record : history_) {
        if (!record.target.startsWith(QLatin1String("c/")) || activeChannels.contains(record.target))
            continue;
        const QString hash = record.target.mid(record.target.lastIndexOf(QLatin1Char('/')) + 1);
        const QString target = channelsByHash.value(hash);
        if (!target.isEmpty()) {
            record.target = target;
            remapped = true;
        }
    }
    if (remapped) {
        refreshUnread();
        saveHistory();
    }
    emit changed();
    return true;
}

bool Client::canEraseLocalHistory() const {
    return !closing_ && pendingId_.isEmpty() && outgoing_.empty() && !localHistoryRadioKey().isEmpty();
}

void Client::reloadStoredHistory() {
    if (connected() || connecting() || reconnecting())
        return;
    QSettings settings;
    settings.setFallbacksEnabled(false);
    const QString restoredKey = settings.value(QLatin1String("radio/last")).toString();
    if (restoredKey != radioKey_) {
        radioKey_ = restoredKey;
        self_ = {};
        contacts_.clear();
        channels_.clear();
        channelAliases_.fill(-1);
        firmware_.clear();
        parameters_.clear();
        battery_.clear();
        batteryMillivolts_ = lastBatteryMillivolts_ = 0;
        channelCount_ = 0;
        publicStatus_.clear();
        queueStatus_.clear();
        directReceived_ = channelReceived_ = publicReceived_ = dataReceived_ = groupHeard_ = 0;
    }
    radioName_ = settings.value(QLatin1String("radio/lastName")).toString();
    blocks_.clear();
    const QVariantMap blocked = settings.value(QLatin1String("ui/blockedSenders")).toMap();
    for (auto it = blocked.constBegin(); it != blocked.constEnd(); ++it)
        blocks_.insert(it.key(), it.value().toString());
    loadHistory();
    emit changed();
}

bool Client::eraseLocalHistory(const QString& expectedRadioKey) {
    if (!canEraseLocalHistory() || radioKey_ != expectedRadioKey)
        return false;
    QSettings settings;
    settings.remove(QString::fromLatin1("radios/%1/history").arg(radioKey_));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;
    history_.clear();
    unread_.clear();
    pendingHistoryPosition_ = 0;
    earlyAcks_.clear();
    deferredReactions_.clear();
    echoWatches_.clear();
    emit changed();
    return true;
}

void Client::saveHistory() {
    if (radioKey_.size() != 64)
        return;

    QByteArray content;
    QDataStream stream(&content, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream << quint32(4) << quint32(history_.size());

    for (const auto& record : history_) {
        stream << record.target << targetTitle(record.target) << record.message.author
               << record.message.text << record.message.timestamp << record.message.outgoing
               << record.message.delivery << record.message.unread
               << qint32(record.message.relayCount) << qint32(record.message.hops) << record.message.routeDetails
               << record.message.relayCountPartial << quint16(record.reactions.size());
        for (const auto& reaction : record.reactions)
            stream << reaction.emoji << reaction.actor << reaction.stamp << reaction.own;
        stream << record.message.roomSigner;
    }
    QSettings settings;
    settings.setValue(QString::fromLatin1("radios/%1/history").arg(radioKey_), content);
    settings.sync();
}
