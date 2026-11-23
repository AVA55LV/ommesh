#include "demo_radio.hpp"

#include "client.hpp"
#include "meshcore.hpp"

#include <QtCore/QBasicTimer>
#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QTimerEvent>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <deque>

namespace {

using meshcore::Bytes;
using meshcore::Key;
namespace response = meshcore::response;
namespace push = meshcore::push;

constexpr int ServiceInterval = 40;
constexpr std::size_t QueueLimit = 32;
constexpr std::uint32_t SampleTimestamp = 1776240000;
constexpr std::int32_t SelfLatitude = 54990000;
constexpr std::int32_t SelfLongitude = 73366600;
constexpr std::string_view SelfName = "Демо-станция";
constexpr std::array<std::uint8_t, 16> PublicKey{
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

constexpr Key fakeKey(std::uint8_t seed) {
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(seed + i * 13);
    }
    return key;
}

constexpr Key SelfKey = fakeKey(0xe8);

struct FixtureContact {
    Key key;
    std::uint8_t type;
    std::string_view name;
    std::int32_t latitude;
    std::int32_t longitude;
};

constexpr std::array<FixtureContact, 4> Contacts{{
    {fakeKey(0x21), 1, "Алиса", 54995700, 73377900},
    {fakeKey(0x53), 1, "Борис", 54986200, 73350400},
    {fakeKey(0x70), 2, "Демо-репитер", 54999700, 73354100},
    {fakeKey(0xa3), 3, "Демо-комната", 54985000, 73383000},
}};

QString objectName() { return QLatin1String("ommeshDemoRadio"); }
QString lastRadioKey() { return QLatin1String("radio/last"); }
QString lastRadioName() { return QLatin1String("radio/lastName"); }

QByteArray byteArray(std::span<const std::uint8_t> data) {
    return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
}

void putU32(Bytes& frame, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        frame[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint32_t unixTime() {
    return QDateTime::currentDateTime().toTime_t();
}

Bytes deviceInfo() {
    Bytes frame(80, 0);
    frame[0] = response::DeviceInfo;
    frame[1] = 14;
    frame[2] = 1;
    frame[3] = 3;
    constexpr std::string_view label = "OMMesh Demo";
    std::copy(label.begin(), label.end(), frame.begin() + 60);
    return frame;
}

Bytes selfInfo() {
    Bytes frame(58, 0);
    frame[0] = response::SelfInfo;
    frame[2] = 17;
    frame[3] = 22;
    std::copy(SelfKey.begin(), SelfKey.end(), frame.begin() + 4);
    putU32(frame, 36, SelfLatitude);
    putU32(frame, 40, SelfLongitude);
    putU32(frame, 48, 869618);
    putU32(frame, 52, 62500);
    frame[56] = 8;
    frame[57] = 8;
    frame.insert(frame.end(), SelfName.begin(), SelfName.end());
    return frame;
}

Bytes contactFrame(const FixtureContact& contact, std::uint32_t time) {
    Bytes frame(148, 0);
    frame[0] = response::Contact;
    std::copy(contact.key.begin(), contact.key.end(), frame.begin() + 1);
    frame[33] = contact.type;
    frame[35] = contact.type == 2 ? 0xff : 1;
    frame[36] = Contacts[2].key[0];
    std::copy(contact.name.begin(), contact.name.end(), frame.begin() + 100);
    putU32(frame, 132, time);
    putU32(frame, 136, contact.latitude);
    putU32(frame, 140, contact.longitude);
    putU32(frame, 144, time);
    return frame;
}

Bytes directText(const FixtureContact& contact, std::string_view text, std::uint32_t time,
                 std::uint8_t format = 0) {
    Bytes frame(16, 0);
    frame[0] = response::ContactMessageV3;
    frame[1] = 30;
    std::copy_n(contact.key.begin(), 6, frame.begin() + 4);
    frame[10] = 1;
    frame[11] = format;
    putU32(frame, 12, time);
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

Bytes channelText(std::uint8_t channel, std::string_view text, std::uint32_t time) {
    Bytes frame(11, 0);
    frame[0] = response::ChannelMessageV3;
    frame[1] = 27;
    frame[4] = channel;
    frame[5] = 1;
    putU32(frame, 7, time);
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

Bytes sentFrame(std::uint32_t tag) {
    Bytes frame(10, 0);
    frame[0] = response::Sent;
    putU32(frame, 2, tag);
    putU32(frame, 6, 4000);
    return frame;
}

class DemoRadio final : public QTcpServer {
public:
    explicit DemoRadio(Client& client) : QTcpServer(&client) {
        setObjectName(::objectName());
        QSettings settings;
        previousKey_ = settings.value(lastRadioKey());
        previousName_ = settings.value(lastRadioName());
        demoKey_ = QString::fromLatin1(byteArray(SelfKey).toHex().constData());
        channels_[0] = makeChannel(0, "Public", PublicKey);
        std::array<std::uint8_t, 16> expeditionKey{};
        expeditionKey.fill(0x39);
        channels_[1] = makeChannel(1, "Поход", expeditionKey);
        channels_[2] = makeChannel(2, "", {});
    }

    ~DemoRadio() override {
        restoreSelectedRadio();
    }

    bool begin() {
        if (!listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        timer_.start(ServiceInterval, this);
        return true;
    }

protected:
    void timerEvent(QTimerEvent* event) override {
        if (event->timerId() != timer_.timerId()) {
            QTcpServer::timerEvent(event);
            return;
        }
        if (peer_ && peer_->state() == QAbstractSocket::UnconnectedState) {
            delete peer_;
            peer_ = nullptr;
            incoming_.clear();
        }
        if (!peer_ && hasPendingConnections()) {
            peer_ = nextPendingConnection();
            peer_->setReadBufferSize(8192);
            authorized_.fill(false);
            messages_.clear();
            messages_.push_back(channelText(0, "Алиса: Добро пожаловать в деморежим!", SampleTimestamp));
            messages_.push_back(directText(Contacts[0],
                "Откройте вкладку «Сеть», чтобы увидеть учебные узлы на карте.", SampleTimestamp + 30));
            messages_.push_back(channelText(1, "Борис: Связь на маршруте проверена.", SampleTimestamp + 60));
        }
        while (hasPendingConnections()) {
            QTcpSocket* extra = nextPendingConnection();
            extra->abort();
            extra->deleteLater();
        }
        if (!peer_) {
            return;
        }
        incoming_.append(peer_->read(4096));
        while (incoming_.size() >= 3) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(incoming_.constData());
            const int length = bytes[1] | (bytes[2] << 8);
            if (bytes[0] != '<' || length < 1 || length > static_cast<int>(meshcore::MaxFrameSize)) {
                peer_->abort();
                return;
            }
            if (incoming_.size() < length + 3) {
                break;
            }
            handle({bytes + 3, static_cast<std::size_t>(length)});
            incoming_.remove(0, length + 3);
        }
    }

private:
    static Bytes makeChannel(std::uint8_t index, std::string_view title,
                             const std::array<std::uint8_t, 16>& key) {
        auto frame = meshcore::setChannel(index, title, key);
        frame.front() = response::ChannelInfo;
        return frame;
    }

    void restoreSelectedRadio() {
        if (!handshake_) {
            return;
        }
        QSettings settings;
        if (settings.value(lastRadioKey()).toString() != demoKey_) {
            return;
        }
        if (previousKey_.isValid()) settings.setValue(lastRadioKey(), previousKey_);
        else settings.remove(lastRadioKey());
        if (previousName_.isValid()) settings.setValue(lastRadioName(), previousName_);
        else settings.remove(lastRadioName());
        handshake_ = false;
    }

    void reply(const Bytes& payload) {
        Bytes packet = meshcore::frame(payload);
        if (!packet.empty()) {
            packet[0] = '>';
            peer_->write(byteArray(packet));
        }
    }

    void enqueue(Bytes message) {
        if (messages_.size() >= QueueLimit) {
            messages_.pop_front();
        }
        messages_.push_back(std::move(message));
        reply({push::MessageWaiting});
    }

    int findContact(std::span<const std::uint8_t> bytes) const {
        for (std::size_t i = 0; i < Contacts.size(); ++i) {
            if (std::equal(bytes.begin(), bytes.end(), Contacts[i].key.begin())) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void sendDirect(std::span<const std::uint8_t> command) {
        if (command.size() < 14) {
            reply({response::Error, 6});
            return;
        }
        const int index = findContact(command.subspan(7, 6));
        if (index < 0) {
            reply({response::Error, 2});
            return;
        }
        const auto& contact = Contacts[index];
        if (command[1] == 1 && authorized_[index] && command.size() >= 16) {
            reply(sentFrame(0));
            std::string text(command.begin() + 13, command.begin() + 16);
            text += "Ответ учебного узла. Обмен идёт только внутри приложения.";
            enqueue(directText(contact, text, nextMessageTimestamp(), 1));
        } else if (command[1] == 0 && contact.type != 2) {
            const std::uint32_t tag = ++ackTag_;
            reply(sentFrame(tag));
            Bytes ack(9, 0);
            ack[0] = push::SendConfirmed;
            putU32(ack, 1, tag);
            putU32(ack, 5, 200);
            reply(ack);
            enqueue(directText(contact, "Сообщение получено в деморежиме.",
                               nextMessageTimestamp()));
        } else {
            reply({response::Error, 4});
        }
    }

    void login(std::span<const std::uint8_t> command) {
        if (command.size() < 33) {
            reply({response::Error, 6});
            return;
        }
        const int index = findContact(command.subspan(1, 32));
        if (index < 0 || (Contacts[index].type != 2 && Contacts[index].type != 3)) {
            reply({response::Error, 2});
            return;
        }
        Bytes sent = sentFrame(0);
        std::copy_n(Contacts[index].key.begin(), 4, sent.begin() + 2);
        reply(sent);
        constexpr std::string_view password = "demo";
        authorized_[index] = command.size() == 33 + password.size() &&
            std::equal(password.begin(), password.end(), command.begin() + 33);
        Bytes result{static_cast<std::uint8_t>(authorized_[index] ? 0x85 : 0x86),
                     static_cast<std::uint8_t>(authorized_[index] ? 1 : 0)};
        result.insert(result.end(), Contacts[index].key.begin(), Contacts[index].key.begin() + 6);
        reply(result);
    }

    void handle(std::span<const std::uint8_t> command) {
        switch (command[0]) {
        case 0x16:
            reply(deviceInfo());
            break;
        case 0x01:
            handshake_ = true;
            reply(selfInfo());
            break;
        case 0x04: {
            restoreSelectedRadio();
            Bytes count(5, 0);
            count[0] = response::ContactsStart;
            putU32(count, 1, Contacts.size());
            reply(count);
            const auto stamp = unixTime();
            for (const auto& contact : Contacts) {
                reply(contactFrame(contact, stamp));
            }
            count[0] = response::ContactsEnd;
            putU32(count, 1, stamp);
            reply(count);
            break;
        }
        case 0x1f:
            reply(command.size() == 2 && command[1] < channels_.size()
                ? channels_[command[1]] : Bytes{response::Error, 2});
            break;
        case 0x20:
            if (command.size() == 50 && command[1] < channels_.size()) {
                auto& channel = channels_[command[1]];
                channel.assign(command.begin(), command.end());
                channel.front() = response::ChannelInfo;
                reply({response::Ok});
            } else {
                reply({response::Error, 6});
            }
            break;
        case 0x0a:
            if (!messages_.empty()) {
                reply(messages_.front());
                messages_.pop_front();
            } else {
                reply({response::NoMoreMessages});
            }
            break;
        case 0x02:
            sendDirect(command);
            break;
        case 0x03:
            if (command.size() > 7 && command[2] < channels_.size()) {
                reply({response::Ok});
                enqueue(channelText(command[2], "Борис: Ваше сообщение получено в деморежиме.",
                                    nextMessageTimestamp()));
            } else {
                reply({response::Error, 6});
            }
            break;
        case 0x07:
        case 0x0d:
            reply({response::Ok});
            break;
        case 0x14: {
            Bytes battery{response::Battery, 0xb4, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0};
            putU32(battery, 3, 124);
            putU32(battery, 7, 512);
            reply(battery);
            break;
        }
        case 0x1a:
            login(command);
            break;
        case 0x42: {
            Bytes response{0x1d};
            if (command.size() >= 4 && command[3] == '|') {
                response.insert(response.end(), command.begin() + 1, command.begin() + 4);
            }
            constexpr std::string_view text = "Демо-станция · обмен идёт внутри приложения.";
            response.insert(response.end(), text.begin(), text.end());
            reply(response);
            break;
        }
        default:
            reply({response::Error, 1});
            break;
        }
    }

    std::uint32_t nextMessageTimestamp() {
        lastMessage_ = std::max(unixTime(), lastMessage_ + 1);
        return lastMessage_;
    }

    QBasicTimer timer_;
    QTcpSocket* peer_ = nullptr;
    QByteArray incoming_;
    std::deque<Bytes> messages_;
    std::array<Bytes, 3> channels_;
    std::array<bool, Contacts.size()> authorized_{};
    QVariant previousKey_;
    QVariant previousName_;
    QString demoKey_;
    std::uint32_t ackTag_ = 0x0d3e0000;
    std::uint32_t lastMessage_ = 0;
    bool handshake_ = false;
};

DemoRadio* demoFor(const Client& client) {
    return static_cast<DemoRadio*>(client.findChild<QTcpServer*>(objectName()));
}

}

bool startDemoRadio(Client& client) {
    if (demoFor(client) || client.connected() || client.connecting()) {
        return false;
    }
    auto* demo = new DemoRadio(client);
    if (!demo->begin()) {
        delete demo;
        return false;
    }
    client.connectTo(QLatin1String("127.0.0.1"), demo->serverPort());
    return true;
}

void stopDemoRadio(Client& client) {
    auto* demo = demoFor(client);
    if (demo) {
        delete demo;
        client.disconnectFromRadio();
    }
}

bool demoRadioActive(const Client& client) {
    return demoFor(client) != nullptr;
}
