#include "client.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

const QByteArray kPublicKey = QByteArray::fromHex("8b3387e9c5cdea6ac9e5edbaa115cd72");

void check(bool valid, const char* condition) {
    if (!valid) {
        std::cerr << condition << '\n';
        std::exit(1);
    }
}

template<class Predicate>
void waitUntil(Predicate ready, const char* condition, int timeout = 2500) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(ready(), condition);
}

quint32 sendTimestamp(const meshcore::Bytes& command) {
    check(command.size() >= 7, "timestamped command");
    return quint32(command[3]) | (quint32(command[4]) << 8) |
           (quint32(command[5]) << 16) | (quint32(command[6]) << 24);
}

meshcore::Bytes channelRecord(int slot, const std::string& name = {}, const QByteArray& key = {}) {
    meshcore::Bytes row(50, 0);
    row[0] = 0x12;
    row[1] = static_cast<std::uint8_t>(slot);
    std::copy(name.begin(), name.end(), row.begin() + 2);
    for (int i = 0; i < key.size(); ++i)
        row[34 + i] = static_cast<std::uint8_t>(key[i]);
    return row;
}

meshcore::Bytes reactionDatagram(int slot, const meshcore::ReactionHash& hash, quint32 stamp,
                                const std::string& emoji, const std::string& name) {
    meshcore::Bytes frame{0x1b, 0, 0, 0, static_cast<std::uint8_t>(slot), 1, 0x0e, 0xff, 0, 2};
    frame.insert(frame.end(), hash.begin(), hash.end());
    for (unsigned shift = 0; shift < 32; shift += 8)
        frame.push_back(static_cast<std::uint8_t>(stamp >> shift));
    frame.push_back(static_cast<std::uint8_t>(emoji.size()));
    frame.insert(frame.end(), emoji.begin(), emoji.end());
    frame.insert(frame.end(), name.begin(), name.end());
    frame[8] = static_cast<std::uint8_t>(frame.size() - 9);
    return frame;
}

class SimulatedRadio {
public:
    SimulatedRadio() {
        check(server_.listen(QHostAddress::LocalHost), "listen on loopback");
    }

    void connect(Client& client) {
        client.connectTo(QString::fromLatin1("127.0.0.1"), server_.serverPort());
        acceptConnection();
    }

    void acceptConnection() {
        waitUntil([&] { return server_.hasPendingConnections(); }, "accept connection");
        socket_ = server_.nextPendingConnection();
        buffer_.clear();
    }

    meshcore::Bytes command(int timeout = 2500) {
        waitUntil([&] {
            if (socket_->bytesAvailable())
                buffer_ += socket_->readAll();
            return buffer_.size() >= 3 && buffer_.size() >= 3 + length();
        }, "read expected command", timeout);
        check(buffer_.at(0) == '<', "command stream marker");
        const int n = length();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer_.constData());
        meshcore::Bytes command(bytes + 3, bytes + 3 + n);
        buffer_.remove(0, 3 + n);
        return command;
    }

    void reply(const meshcore::Bytes& bytes, bool fragmented = false) {
        auto packet = meshcore::frame(bytes);
        packet[0] = '>';
        const char* raw = reinterpret_cast<const char*>(packet.data());
        if (fragmented) {
            socket_->write(raw, 2);
            socket_->flush();
            QCoreApplication::processEvents();
            socket_->write(raw + 2, packet.size() - 2);
        } else {
            socket_->write(raw, packet.size());
        }
    }

    void close() { socket_->disconnectFromHost(); }

    void initialize(bool includeChats, std::uint8_t keyByte = 1, bool parity = false,
                    std::uint8_t chatType = 1) {
        check(command() == meshcore::Bytes({22, 3}), "protocol v3 query");
        reply({13, 13, 50, static_cast<std::uint8_t>(includeChats ? 2 : 0)}, true);
        const auto start = command();
        check(start.size() > 8 && start[0] == 1, "app start");
        meshcore::Bytes identity(58, 0);
        identity[0] = 5;
        identity[4] = keyByte;
        identity[36] = 42;
        identity[40] = 43;
        identity[48] = 0x1d;
        identity[49] = 0x3f;
        identity[50] = 0x0d;
        identity.insert(identity.end(), {'N', 'o', 'd', 'e'});
        reply(identity);
        check(command() == meshcore::Bytes({4}), "request contacts");
        reply({2, static_cast<std::uint8_t>(includeChats ? 1 : 0), 0, 0, 0});
        if (includeChats) {
            meshcore::Bytes contact(148, 0);
            contact[0] = 3;
            contact[1] = 0xa1;
            contact[33] = chatType;
            const std::string name = "Пётр";
            std::copy(name.begin(), name.end(), contact.begin() + 100);
            reply(contact);
            if (parity) {
                meshcore::Bytes repeater(148, 0);
                repeater[0] = 3;
                repeater[1] = 0xa2;
                repeater[33] = 2;
                repeater[35] = 0x42;
                repeater[132] = 5;
                repeater[136] = 30;
                repeater[140] = 31;
                const std::string repeaterName = "Репитер";
                std::copy(repeaterName.begin(), repeaterName.end(), repeater.begin() + 100);
                reply(repeater);
            }
        }
        reply({4, 0, 0, 0, 0});
        if (includeChats) {
            check(command() == meshcore::Bytes({31, 0}), "read channel zero");
            reply(parity ? channelRecord(0, "Alias", kPublicKey) : channelRecord(0, "P", QByteArray(1, 0x27)));
            check(command() == meshcore::Bytes({31, 1}), "read channel one");
            reply(parity ? channelRecord(1, "Public", kPublicKey) : channelRecord(1));
        }
        check(command() == meshcore::Bytes({10}), "initial sync");
    }

private:
    int length() const {
        return static_cast<unsigned char>(buffer_[1]) +
               256 * static_cast<unsigned char>(buffer_[2]);
    }

    QTcpServer server_;
    QTcpSocket* socket_ = nullptr;
    QByteArray buffer_;
};

void testNativeParity() {
    Client client;
    SimulatedRadio radio;
    radio.connect(client);
    radio.initialize(true, 3, true);
    radio.reply({0x11, 0xf0, 0, 0, 0, 1, 0, 9, 0, 0, 0, 'N', ':', ' ', 'Q'});
    check(radio.command() == meshcore::Bytes({10}), "continue Public alias sync");
    radio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "parity client ready");
    const auto targets = client.targets();
    check(targets.size() == 2 && targets.first().title == QLatin1String("Public"), "coalesce equal-key Public alias");
    const auto channel = targets.first().id;
    const auto direct = targets.last().id;
    check(client.messages(channel).first().text == QLatin1String("Q") &&
          client.messages(channel).first().hops == 1 && client.messages(channel).first().routeDetails.contains("-4"),
          "Public alias RX reaches canonical chat with hops and signed SNR");
    check(client.radioInfo().contains(QLatin1String("Public: 1")), "Public RX diagnostic count");
    check(client.ownLatitude() == 42 && client.ownLongitude() == 43, "self latitude longitude");
    const auto nodes = client.nodes();
    check(nodes.size() == 2 && nodes.last().role == 2 && nodes.last().path == 2 &&
          nodes.last().lastAdvert == 5 && nodes.last().latitude == 30 && nodes.last().longitude == 31,
          "node role and position metadata");

    const meshcore::ReactionHash qHash{0xf0, 0x08, 0x6c, 0xaf, 0xf1}; // SHA-256 of Q followed by 09 00 00 00.
    const QString thumb = QString::fromUtf8("👍");
    check(client.messages(channel).first().canReact && client.react(channel, client.messages(channel).first(), thumb),
          "make channel reaction for an actual chat record");
    const auto channelReaction = radio.command();
    check(channelReaction.size() == 24 &&
          std::equal(channelReaction.begin(), channelReaction.begin() + 11,
                     meshcore::Bytes{0x3e, 1, 0xff, 0x0e, 0xff, 2, 0xf0, 0x08, 0x6c, 0xaf, 0xf1}.begin()) &&
          channelReaction[15] == 4 &&
          std::string(channelReaction.begin() + 16, channelReaction.end()) == "👍Node",
          "Android reaction datagram, exact target hash and UTF-8 name");
    check(client.messages(channel).first().reactions.isEmpty(), "do not claim reaction before radio accepts");
    radio.reply({0});
    waitUntil([&] { return !client.busy() && !client.messages(channel).first().reactions.isEmpty(); },
              "add accepted own reaction");
    radio.reply(reactionDatagram(0, qHash, 90, "👍", "Bob"));
    waitUntil([&] { return client.messages(channel).first().reactions.first().count == 2; },
              "received alias reaction is added to the Public message");
    check(client.messages(channel).first().reactions.first().own && client.messages(channel).size() == 1,
          "reaction is a badge and not another message");
    radio.reply(reactionDatagram(0, qHash, 91, "", "Bob"));
    radio.reply(reactionDatagram(0, qHash, 90, "👍", "Bob"));
    waitUntil([&] { return client.messages(channel).first().reactions.first().count == 1; },
              "reaction removal preserves own and ignores a duplicate old event");

    check(client.send(channel, QLatin1String("Echo")), "send Public echo test");
    const auto sentGroup = radio.command();
    check(sentGroup[0] == 3 && sentGroup[2] == 1, "canonical Public send slot");
    const auto* key = reinterpret_cast<const std::uint8_t*>(kPublicKey.constData());
    const auto payload = meshcore::groupTextPayload({key, 16}, sendTimestamp(sentGroup), "Node: Echo");
    meshcore::Bytes echo{0x88, 18, 0xb0, 0x15, 1, 0xa2};
    echo.insert(echo.end(), payload.begin(), payload.end());
    auto unrelated = echo;
    unrelated[5] = 0xff;
    unrelated.back() ^= 1;
    radio.reply(unrelated);
    radio.reply(echo);
    radio.reply({0});
    waitUntil([&] { return !client.busy() && client.messages(channel).last().relayCount == 1; }, "count authenticated echo before write reply");
    check(client.messages(channel).last().routeDetails.contains(QString::fromUtf8("Репитер · SNR +4,5")),
          "exact relay name and one-hop signed SNR");
    auto wideCopy = echo;
    wideCopy[4] = 0x41; // Same relay, now advertised by a two-byte prefix.
    wideCopy.insert(wideCopy.begin() + 6, 0);
    auto relayedTwice = echo;
    relayedTwice[1] = 36;
    relayedTwice[4] = 2;
    relayedTwice.insert(relayedTwice.begin() + 6, 0xca);
    auto secondRelay = echo;
    secondRelay[5] = 0xb2;
    radio.reply(wideCopy);
    radio.reply(relayedTwice);
    radio.reply(secondRelay);
    waitUntil([&] { return client.messages(channel).last().relayCount == 2; },
              "two observed first-hop relays without double-counting or later hops");
    const auto routeDetails = client.messages(channel).last().routeDetails;
    check(routeDetails.contains("#B2") && !routeDetails.contains("#CA") && !routeDetails.contains("+9"),
          "unresolved relay uses its true hash and multi-hop SNR is not ascribed to the first relay");

    check(client.requestBattery(), "queue battery");
    check(radio.command() == meshcore::Bytes({20}), "battery protocol");
    radio.reply({12, 0x57, 0x0f, 16, 0, 0, 0, 0, 1, 0, 0});
    waitUntil([&] { return !client.busy(); }, "battery reply processed");
    check(client.batteryInfo().contains("3,927") && client.batteryInfo().contains("256"), "voltage and storage");
    check(client.batteryMillivolts() == 3927, "display uses real battery millivolts");
    check(client.advertise(), "queue advert");
    check(radio.command() == meshcore::Bytes({7, 0}), "zero-hop radio advert");
    radio.reply({0});
    waitUntil([&] { return !client.busy(); }, "advert reply processed");

    check(client.runConsole(1, QString(), QLatin1String("ver")), "queue local radio console");
    const auto console = radio.command();
    check(console.size() == 7 && console[0] == 0x42 && console.back() == 'r', "local console command");
    radio.reply({0x1d, console[1], console[2], console[3], 'O', 'K'});
    waitUntil([&] { return !client.busy(); }, "console returns the command slot");

    check(client.setRadioChannel(0, QString::fromUtf8("Канал"), QString::fromLatin1(kPublicKey.toHex().constData())),
          "queue channel write");
    const auto set = radio.command();
    check(set.size() == 50 && set[0] == 0x20 && set[1] == 0 && set[34] == 0x8b && set[49] == 0x72,
          "channel write envelope includes exact 16-byte key");
    radio.reply({0});
    check(radio.command() == meshcore::Bytes({31, 0}), "read written channel refresh");
    radio.reply(channelRecord(0, "Канал", kPublicKey));
    check(radio.command() == meshcore::Bytes({31, 1}), "refresh other channel");
    radio.reply(channelRecord(1, "Public", kPublicKey));
    check(radio.command() == meshcore::Bytes({10}), "resume polling after channel write");
    radio.reply({10});
    waitUntil([&] { return !client.busy(); }, "channel reload complete");
    check(client.radioChannels().size() == 2 && client.radioChannels().first().title == QString::fromUtf8("Канал"),
          "editor sees exact radio catalog after write");

    check(client.setRadioChannel(0, QString(), QString()), "queue explicit channel remove");
    const auto clear = radio.command();
    check(clear.size() == 50 && clear[0] == 0x20 && clear[1] == 0 &&
          std::all_of(clear.begin() + 2, clear.end(), [](auto byte) { return byte == 0; }),
          "only explicit remove writes blank slot key and name");
    radio.reply({0});
    check(radio.command() == meshcore::Bytes({31, 0}), "reload removed slot");
    radio.reply(channelRecord(0));
    check(radio.command() == meshcore::Bytes({31, 1}), "keep other slot after remove");
    radio.reply(channelRecord(1, "Public", kPublicKey));
    check(radio.command() == meshcore::Bytes({10}), "resume queue after remove");
    radio.reply({10});
    waitUntil([&] { return !client.busy(); }, "removed slot reload complete");
    check(client.radioChannels().first().title.isEmpty() && client.radioChannels().first().key.isEmpty(),
          "removed slot is empty in editor");

    check(client.send(direct, QLatin1String("Retry")), "send direct for fallback");
    const auto first = radio.command();
    radio.reply({6, 0, 45, 0, 0, 0, 0, 0, 0, 0});
    check(radio.command(6500) == meshcore::Bytes({10}), "queue timeout poll before fallback");
    radio.reply({10});
    const auto reset = radio.command();
    check(reset.size() == 33 && reset[0] == 13 && reset[1] == 0xa1, "direct fallback resets complete peer key");
    radio.reply({0});
    const auto repeat = radio.command();
    check(first.size() >= 13 && repeat.size() == first.size() && first[2] == 0 && repeat[2] == 1 &&
          std::equal(first.begin() + 3, first.end(), repeat.begin() + 3),
          "one repeat uses original timestamp and plaintext");
    radio.reply({0x82, 45, 0, 0, 0, 5, 0, 0, 0});
    radio.reply({6, 1, 46, 0, 0, 0, 0, 0, 0, 0});
    waitUntil([&] { return !client.busy() && client.messages(direct).last().delivery == QString::fromUtf8("Доставлено"); },
              "late first acknowledgement after retry preserves one row");
    check(client.messages(direct).size() == 1, "one history row per retried message");
    check(client.messages(direct).last().relayCount == -1 && client.messages(direct).last().routeDetails.isEmpty(),
          "direct ACK cannot prove a relay count");

    check(client.send(direct, QLatin1String("Do not replay")), "send interrupted direct");
    check(radio.command()[0] == 2, "wire sees interrupted direct once");
    radio.close();
    waitUntil([&] { return client.reconnecting(); }, "detect peer closure and schedule reconnect");
    radio.acceptConnection();
    radio.initialize(true, 3, true);
    radio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "reconnect through fresh handshake");
    check(client.messages(direct).last().delivery == QString::fromUtf8("Итог неизвестен"), "no replay of uncertain message");
    client.disconnectFromRadio();
    check(!client.reconnecting(), "explicit disconnect cancels reconnect");
    const Client reopened;
    check(reopened.messages(channel).first().reactions.first().own &&
          reopened.messages(channel).first().reactions.first().count == 1,
          "own and removal state survives local history reload");
    check(reopened.messages(channel).last().relayCount == 2, "observed relay count survives history reload");
}

void testInitialConnectionFailures() {
    QStringList stages;
    Client client(nullptr, [&](const char* stage, int) { stages.append(QString::fromLatin1(stage)); });
    client.setAutoReconnect(false);
    const QString handled = QString::fromLatin1("TCP: handle failure outside socket callback");

    QTcpServer unused;
    check(unused.listen(QHostAddress::LocalHost), "reserve a port for connection refusal");
    const quint16 closedPort = unused.serverPort();
    unused.close();
    client.connectTo(QString::fromLatin1("127.0.0.1"), closedPort);
    waitUntil([&] { return stages.contains(handled) && !client.connecting(); }, "finish refused connection");
    check(!client.connected() && stages.count(handled) == 1, "handle refusal once");

    stages.clear();
    SimulatedRadio droppingRadio;
    droppingRadio.connect(client);
    check(droppingRadio.command() == meshcore::Bytes({22, 3}), "start handshake before peer closes");
    droppingRadio.close();
    waitUntil([&] { return stages.contains(handled) && !client.connecting(); }, "finish interrupted handshake");
    check(!client.connected() && stages.count(handled) == 1, "handle interrupted handshake once");

    SimulatedRadio radio;
    radio.connect(client);
    radio.initialize(false);
    radio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "connect again after initial failures");
    client.disconnectFromRadio();
}

void testConfiguredSendTries() {
    Client client;
    check(client.sendTries() == 5 && Client::sendTriesSpanMs(5) == 465000,
          "Android attempt defaults and nominal retry span");
    client.setSendTries(100);
    check(Client().sendTries() == 8, "send attempts capped and saved across instances");
    client.setSendTries(0);
    check(Client().sendTries() == 1, "one attempt selected and saved");

    const QString target = QLatin1String("d/a10000000000");
    auto sendWithoutAck = [&](SimulatedRadio& radio, const char* text) {
        check(client.send(target, QLatin1String(text)), "send to an unconfirmed recipient");
        const auto command = radio.command();
        check(command[0] == 2 && command[2] == 0, "first attempt goes out without resetting the route");
        radio.reply({6, 0, 51, 0, 0, 0, 0, 0, 0, 0});
        check(radio.command(6500) == meshcore::Bytes({10}), "idle poll precedes the acknowledgement deadline");
        radio.reply({10});
        waitUntil([&] { return client.messages(target).last().delivery == QString::fromUtf8("Нет подтверждения"); },
                  "single attempt is left unconfirmed without automatic resend");
    };
    {
        SimulatedRadio radio;
        radio.connect(client);
        radio.initialize(true, 4);
        radio.reply({10});
        waitUntil([&] { return client.connected() && !client.busy(); }, "direct retry client ready");
        sendWithoutAck(radio, "Send once");
        radio.reply({0x82, 51, 0, 0, 0, 5, 0, 0, 0});
        waitUntil([&] { return client.messages(target).last().delivery == QString::fromUtf8("Доставлено"); },
                  "late ACK can still confirm the finished attempt");
        client.disconnectFromRadio();
    }
    client.setSendTries(5);
    SimulatedRadio roomRadio;
    roomRadio.connect(client);
    roomRadio.initialize(true, 5, false, 3);
    roomRadio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "room retry client ready");
    sendWithoutAck(roomRadio, "Room messages are manually resent");
    check(client.messages(target).size() == 1, "room stays at one send even when five tries are configured");
    client.disconnectFromRadio();
}

void testBlockedSenders() {
    Client client;
    SimulatedRadio radio;
    radio.connect(client);
    radio.initialize(true, 6, true, 3);
    radio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "block test ready");
    const QString channel = client.targets().first().id;
    const QString room = QLatin1String("d/a10000000000");
    int notifications = 0;
    QObject::connect(&client, &Client::incomingMessage, [&] { ++notifications; });

    auto receiveChannel = [&](std::uint8_t stamp, const std::string& body, int size) {
        meshcore::Bytes frame{0x11, 0, 0, 0, 1, 0xff, 0, stamp, 0, 0, 0};
        frame.insert(frame.end(), body.begin(), body.end());
        radio.reply(frame);
        waitUntil([&] { return client.messages(channel).size() == size; }, "receive channel history");
    };
    receiveChannel(1, "Ольга: one", 1);
    receiveChannel(2, "Other: two", 2);
    const auto author = client.messages(channel).first();
    client.setSenderBlocked(author.blockId, author.author, true);
    receiveChannel(3, "oЛЬГa: three", 3);
    const auto messages = client.messages(channel);
    check(messages.first().blocked && messages.last().blocked && !messages.last().unread &&
          !messages.first().unread && messages.at(1).unread && notifications == 2,
          "blocking lookalike channel names hides retained and new messages without unread");
    check(client.latestMessages().value(channel).text == QLatin1String("two") &&
          client.blockedChannelMessageCount(author.author) == 2,
          "blocked channel messages do not replace the chat preview");

    auto receiveRoom = [&](std::uint8_t signer, int size) {
        radio.reply({0x10, 0, 0, 0, 0xa1, 0, 0, 0, 0, 0, 0xff, 2,
                     static_cast<std::uint8_t>(size), 0, 0, 0, signer, 0, 0, 0, 'P'});
        waitUntil([&] { return client.messages(room).size() == size; }, "receive signed room history");
    };
    receiveRoom(0xa2, 1);
    const auto signedPost = client.messages(room).first();
    check(signedPost.blockId == QLatin1String("k:a2000000") &&
          signedPost.author == QString::fromUtf8("Репитер"), "identify room author by signer key");
    client.setSenderBlocked(signedPost.blockId, signedPost.author, true);
    receiveRoom(0xa2, 2);
    receiveRoom(0xde, 3);
    const auto posts = client.messages(room);
    check(posts.at(0).blocked && posts.at(1).blocked && !posts.at(2).blocked &&
          client.blockedContactMessageCount(QLatin1String("a2000000")) == 2 && notifications == 4,
          "block only the room signer and retain posts without notifications");
    client.disconnectFromRadio();

    Client reopened;
    check(reopened.messages(channel).last().blocked && reopened.messages(room).first().blocked,
          "restore both block kinds and room signer history");
    reopened.unblockContact(QLatin1String("a2000000"));
    reopened.unblockChannelName(author.author);
    check(!reopened.messages(room).first().blocked && !reopened.messages(room).first().unread &&
          !reopened.messages(channel).last().blocked && !reopened.messages(channel).last().unread,
          "unblocking restores visibility without reviving unread markers");
}

void testReloadDifferentRadioHistory() {
    Client client;
    SimulatedRadio radio;
    radio.connect(client);
    radio.initialize(true, 0x41);
    radio.reply({10});
    waitUntil([&] { return client.connected() && !client.busy(); }, "source radio is ready before offline restore");
    check(!client.nodes().isEmpty(), "source radio has network inventory");

    const QString importedKey(64, QLatin1Char('b'));
    const QString importedChat = QLatin1String("d/123456789abc");
    const QString importedName = QString::fromUtf8("Нода из копии");
    const QString importedMessage = QString::fromUtf8("Сообщение из копии");
    const QString signer = QLatin1String("1234abcd");
    QByteArray history;
    QDataStream stream(&history, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_4_6);
    stream << quint32(4) << quint32(1) << importedChat << QString::fromUtf8("Комната")
           << QString::fromUtf8("Автор") << importedMessage << quint32(10) << false << QString()
           << true << qint32(-1) << qint32(-1) << QString() << false << quint16(0) << signer;
    QSettings settings;
    settings.setValue(QLatin1String("radio/last"), importedKey);
    settings.setValue(QLatin1String("radio/lastName"), importedName);
    settings.setValue(QString::fromLatin1("radios/%1/history").arg(importedKey), history);
    QVariantMap blocked;
    blocked.insert(QLatin1String("k:") + signer, QString::fromUtf8("Автор"));
    settings.setValue(QLatin1String("ui/blockedSenders"), blocked);
    settings.sync();

    client.reloadStoredHistory();
    check(client.radioName() != importedName, "do not change the identity of an active connection");
    client.disconnectFromRadio();
    client.reloadStoredHistory();
    check(client.radioName() == importedName && client.localHistoryRadioKey() == importedKey,
          "select the radio identified by the imported copy immediately");
    check(client.nodes().isEmpty(), "do not associate prior radio contacts with the imported history");
    const auto messages = client.messages(importedChat);
    check(messages.size() == 1 && messages.first().text == importedMessage &&
          messages.first().blocked && !messages.first().unread,
          "hydrate the transferred chat and its imported block rule without a restart");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QString::fromLatin1("OMMeshBelle-test"));
    QCoreApplication::setApplicationName(QString::fromLatin1("test"));
    const QTemporaryDir config;
    check(config.isValid(), "isolated config directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());

    QString direct;
    QString channel;
    {
        Client client;
        SimulatedRadio radio;
        radio.connect(client);
        radio.initialize(true);
        radio.reply({27});
        check(radio.command() == meshcore::Bytes({10}), "skip unsupported channel data");
        radio.reply({16, 0, 0, 0, 0xa1, 0, 0, 0, 0, 0, 0xff, 0, 5, 0, 0, 0, 'H', 'i'}, true);
        check(radio.command() == meshcore::Bytes({10}), "drain pending messages");
        radio.reply({10});
        waitUntil([&] { return client.connected() && !client.busy(); }, "client ready");
        const auto targets = client.targets();
        check(targets.size() == 2, "empty channel slot is hidden");
        check(targets[0].channel && targets[0].title == QString::fromLatin1("P"), "configured channel");
        check(!targets[1].channel && targets[1].title == QString::fromUtf8("Пётр"), "UTF-8 contact");
        check(targets[1].unread == 1, "unread message count");
        channel = targets[0].id;
        direct = targets[1].id;
        check(client.maxTextBytes(channel) == 154, "channel reserves radio name prefix");
        check(client.maxTextBytes(direct) == 158, "direct message limit");
        check(client.messages(direct).at(0).text == QString::fromLatin1("Hi"), "receive direct");
        const auto incomingDirect = client.messages(direct).first();
        check(incomingDirect.canReact && client.react(direct, incomingDirect, QString::fromUtf8("❤️")),
              "send Android-compatible direct reaction");
        const auto directReaction = radio.command();
        check(directReaction.size() >= 13 && directReaction[0] == 2 && directReaction[1] == 1 &&
              directReaction[7] == 0xa1 && std::string(directReaction.begin() + 13, directReaction.end()) == "❤️\nx1rw1901",
              "CLI_DATA with exact Crockford message hash");
        radio.reply({6, 1, 0, 0, 0, 0, 0, 0, 0, 0});
        waitUntil([&] { return !client.busy() && !client.messages(direct).first().reactions.isEmpty(); },
                  "direct no-ACK radio acceptance still shows own reaction");
        auto remote = meshcore::Bytes{16, 0, 0, 0, 0xa1, 0, 0, 0, 0, 0, 0xff, 1, 7, 0, 0, 0};
        const std::string remoteText = "👍\nx1rw1901";
        remote.insert(remote.end(), remoteText.begin(), remoteText.end());
        radio.reply(remote);
        waitUntil([&] { return client.messages(direct).first().reactions.size() == 2; },
                  "direct CLI_DATA from an ordinary contact attaches as a reaction");
        check(client.messages(direct).size() == 1, "received reaction stays out of text history");
        client.markRead(direct);
        check(client.targets().at(1).unread == 0, "mark read");
        check(!client.send(direct, QString(80, QChar(0x410))), "reject oversized UTF-8");

        const QByteArray outgoing = QString::fromUtf8("Привет").toUtf8();
        check(client.send(direct, QString::fromUtf8(outgoing)), "send direct");
        const auto sent = radio.command();
        check(sent.front() == 2 && sent[7] == 0xa1 && sent[12] == 0, "direct command addressing");
        check(QByteArray(reinterpret_cast<const char*>(sent.data() + 13), sent.size() - 13) ==
                  outgoing, "Cyrillic wire text");
        radio.reply({0x82, 8, 0, 0, 0, 15, 0, 0, 0});
        radio.reply({0x82, 7, 0, 0, 0, 15, 0, 0, 0});
        radio.reply({6, 1, 7, 0, 0, 0, 0xe8, 3, 0, 0});
        waitUntil([&] { return client.messages(direct).size() == 2 &&
                                client.messages(direct).last().delivery == QString::fromUtf8("Доставлено"); },
                  "early direct acknowledgement matches sent tag");

        check(client.send(channel, QString::fromLatin1("hello everyone")), "send channel");
        const auto group = radio.command();
        check(group.front() == 3 && group[2] == 0, "channel command addressing");
        check(sendTimestamp(group) > sendTimestamp(sent), "outgoing timestamps increase across rapid sends");
        const auto pendingChannel = client.messages(channel);
        check(pendingChannel.size() == 1 && pendingChannel.last().outgoing && !pendingChannel.last().canReact &&
              pendingChannel.last().delivery == QString::fromUtf8("Отправка…") &&
              pendingChannel.last().timestamp == sendTimestamp(group), "show one preview while radio reply is pending");
        const std::string quotedReply = "@[Node] >hello everyone\nReply";
        meshcore::Bytes earlyReply{8, 0, 0, 0, 5, 0, 0, 0, 'P', ':', ' '};
        earlyReply.insert(earlyReply.end(), quotedReply.begin(), quotedReply.end());
        radio.reply(earlyReply);
        waitUntil([&] { return client.messages(channel).size() == 2; }, "receive reply before radio acknowledges the original");
        const auto pendingWithReply = client.messages(channel);
        check(pendingWithReply.first().outgoing && pendingWithReply.last().timestamp == 5 &&
              pendingWithReply.last().text == QLatin1String(quotedReply.c_str()),
              "reply below pending original even with an older sender timestamp");
        radio.reply({0});
        waitUntil([&] { return !client.busy(); }, "channel send finishes");
        const auto acceptedWithReply = client.messages(channel);
        check(acceptedWithReply.size() == 2 && acceptedWithReply.first().outgoing,
              "replace pending preview in its existing position");
        check(acceptedWithReply.first().delivery == QString::fromUtf8("Принято рацией"),
              "channel send reports acceptance only");
        check(acceptedWithReply.first().relayCount == -1, "no echoed channel copy, relay count unavailable");

        radio.reply({0x83});
        check(radio.command() == meshcore::Bytes({10}), "message waiting triggers sync");
        radio.reply({8, 0, 0, 0, 4, 0, 0, 0, 'P', ':', ' ', 'O', 'K'});
        check(radio.command() == meshcore::Bytes({10}), "receive legacy channel");
        radio.reply({10});
        waitUntil([&] { return !client.busy(); }, "channel sync finishes");
        check(client.messages(channel).last().text == QLatin1String("OK") &&
              client.messages(channel).last().author == QLatin1String("P") &&
              client.latestMessages().value(channel).text == QLatin1String("OK"),
              "channel history and preview use arrival order, keeping the wire timestamp intact");
        radio.close();
        waitUntil([&] { return !client.connected(); }, "handle peer closing");
        check(client.messages(direct).size() == 2, "offline history");
    }

    Client reopened;
    check(reopened.messages(direct).size() == 2 && reopened.messages(channel).size() == 3 &&
          reopened.messages(channel).first().outgoing && reopened.messages(channel).last().timestamp == 4,
          "persist history through restart");
    check(reopened.targets().at(0).unread == 0, "read status persists");
    SimulatedRadio secondRadio;
    secondRadio.connect(reopened);
    secondRadio.initialize(false, 2);
    secondRadio.reply({10});

    waitUntil([&] { return reopened.connected() && !reopened.busy(); }, "second radio ready");
    check(reopened.targets().isEmpty() && reopened.messages(direct).isEmpty(),
          "history isolated by radio identity");
    check(secondRadio.command(6500) == meshcore::Bytes({10}), "idle queue poll without a radio push");
    secondRadio.reply({10});
    reopened.disconnectFromRadio();

    testNativeParity();
    testInitialConnectionFailures();
    testConfiguredSendTries();
    testBlockedSenders();
    testReloadDifferentRadioHistory();

    std::cout << "TCP chat workflow passed\n";
}
