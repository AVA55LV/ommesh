#ifndef OMMESH_BELLE_CLIENT_HPP
#define OMMESH_BELLE_CLIENT_HPP

#include "meshcore.hpp"
#include "native_echo.hpp"
#include "radio_console.hpp"
#include "message_reaction.hpp"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtNetwork/QAbstractSocket>
#include <deque>
#include <functional>

class QTcpSocket;
class QTimer;
class NativeWlan;

struct ChatTarget {
    QString id;
    QString title;
    bool channel;
    int unread;
};

struct ChatReaction {
    QString emoji;
    int count = 0;
    bool own = false;
};

struct ChatMessage {
    QString author;
    QString text;
    quint32 timestamp;
    bool outgoing;
    QString delivery;
    int relayCount = -1;
    QString routeDetails;
    int hops = -1;
    QVector<ChatReaction> reactions;
    bool canReact = false;
    bool relayCountPartial = false;
    QString roomSigner;
    bool unread = false;
    QString blockId;
    bool blocked = false;
};

struct NetworkNode {
    QString id;
    QString title;
    int role = 0;
    int path = -1;
    quint32 lastAdvert = 0;
    qint32 latitude = 0;
    qint32 longitude = 0;
    QString publicKey;
    bool consoleAuthorized = false;
};

struct RadioChannel {
    int slot;
    QString title;
    QString key;
};

struct NodeNeighbour {
    QString prefix;
    quint32 heardSeconds;
    int snrQuarterDb;
};

struct NodeNeighboursPage {
    int total;
    QVector<NodeNeighbour> rows;
};

struct NodeAccessEntry {
    QString prefix;
    int permissions;
};

struct NodeTelemetryReadings {
    std::optional<int> batteryMillivolts;
    std::optional<int> temperatureTenthsC;
};

struct NodeRouteTraceResult {
    int roundTripMs;
    QVector<int> snrQuarters;
};

class Client : public QObject {
    Q_OBJECT

public:
    enum NodeOperation {
        NodeNeighbours = meshcore::RadioConsole::Neighbours,
        NodeAccess = meshcore::RadioConsole::Access,
        NodeRouteReset = meshcore::RadioConsole::ResetRoute,
        NodeTelemetry = meshcore::RadioConsole::Telemetry,
        NodeLogout = meshcore::RadioConsole::Logout,
        NodeRouteTrace = meshcore::RadioConsole::Trace
    };

    using ConnectionTrace = std::function<void(const char*, int)>;
    static constexpr int TraceWithoutValue = -32768;
    static constexpr int MinimumSendTries = 1;
    static constexpr int DefaultSendTries = 5;
    static constexpr int MaximumSendTries = 8;

    explicit Client(QObject* parent = nullptr, ConnectionTrace trace = {});

    void connectTo(const QString& host, quint16 port);
    void traceConnection(const char* step, int value = TraceWithoutValue) const;
    void disconnectFromRadio();
    void setAutoReconnect(bool enabled);
    void retryConnectionNow();
    void setWlanRecovery(bool enabled);
    int sendTries() const;
    void setSendTries(int tries);
    static qint64 sendTriesSpanMs(int tries);
    void refresh();
    void requestMessages();
    bool send(const QString& id, const QString& text);
    bool react(const QString& targetId, const ChatMessage& message, const QString& emoji);
    void markRead(const QString& id);
    QMap<QString, QString> blockedContacts() const;
    QStringList blockedChannelNames() const;
    int blockedContactMessageCount(const QString& id) const;
    int blockedChannelMessageCount(const QString& name) const;
    void setSenderBlocked(const QString& id, const QString& name, bool blocked);
    void unblockContact(const QString& id);
    void unblockChannelName(const QString& name);
    bool advertise();
    bool requestBattery();
    int nextConsoleRequestId();
    bool runConsole(int requestId, const QString& nodeId, const QString& command);
    bool loginConsole(int requestId, const QString& nodeId, const QString& password);
    bool nodeSignedIn(const QString& nodeId) const;
    bool nodeConsoleAuthorized(const QString& nodeId) const;
    bool requestNodeNeighbours(int requestId, const QString& nodeId, int order, int offset = 0);
    bool requestNodeAccess(int requestId, const QString& nodeId);
    bool requestNodeRouteReset(int requestId, const QString& nodeId);
    bool canTraceNodeRoute(const QString& nodeId, bool direct = false) const;
    bool requestNodeRouteTrace(int requestId, const QString& nodeId, bool direct = false);
    bool requestNodeTelemetry(int requestId, const QString& nodeId);
    bool logOutNode(int requestId, const QString& nodeId);
    static std::optional<NodeNeighboursPage> parseNodeNeighbours(const QByteArray& data);
    static QVector<NodeAccessEntry> parseNodeAccess(const QByteArray& data);
    static NodeTelemetryReadings parseNodeTelemetry(const QByteArray& data);
    static std::optional<NodeRouteTraceResult> parseNodeRouteTrace(const QByteArray& data);
    QVector<RadioChannel> radioChannels() const;
    bool setRadioChannel(int slot, const QString& name, const QString& keyHex);

    bool connected() const;
    bool connecting() const;
    bool reconnecting() const;
    int reconnectAttempt() const;
    bool canRetryConnectionNow() const;
    bool busy() const;
    QString status() const;
    QString radioName() const;
    QString radioInfo() const;
    QString radioParameters() const;
    QString radioParametersSummary() const;
    QString batteryInfo() const;
    int batteryMillivolts() const;
    int lastBatteryMillivolts() const;
    NetworkNode selfNode() const;
    qint32 ownLatitude() const;
    qint32 ownLongitude() const;
    QVector<NetworkNode> nodes() const;
    QMap<QString, ChatMessage> latestMessages() const;
    QVector<ChatTarget> targets() const;
    QVector<ChatMessage> messages(const QString& id) const;
    int maxTextBytes(const QString& id) const;

signals:
    void changed();
    void incomingMessage();
    void incomingChatMessage(const QString& id, const QString& text, bool room);
    void consoleResult(int requestId, int operation, bool success, const QString& text);
    void nodeManagementResult(int requestId, int operation, bool success, const QByteArray& data, const QString& error);
    void channelResult(int slot, bool success, const QString& message);

private slots:
    void socketConnected();
    void readSocket();
    void socketDisconnected();
    void socketError(QAbstractSocket::SocketError error);
    void applySocketFailure();
    void connectRadioSocket();
    void wlanFailed(const QString& reason);
    void tick();

private:
    enum class Wait { None, Device, Identity, Contacts, Channel, Sync, Send, ResetPath, Advert, Battery, Console, SetChannel, Reaction };
    struct ReactionEntry {
        QString emoji;
        QString actor;
        quint32 stamp;
        bool own;
    };
    struct StoredMessage {
        QString target;
        ChatMessage message;
        QVector<quint32> ackTags;
        qint64 deadline = 0;
        qint64 retryAt = 0;
        meshcore::Key recipient{};
        int attempts = 1;
        int attemptLimit = 0;
        bool flood = true;
        meshcore::ReactionHash reactionHash{};
        QVector<ReactionEntry> reactions;
    };
    struct PendingReaction {
        QString id;
        meshcore::ReactionHash target;
        ReactionEntry reaction;
        QString recipient;
        bool replace = true;
        qint64 heardAt = 0;
    };
    struct TextRequest {
        QString id;
        QString text;
    };
    struct ConsoleRequest {
        int id;
        int operation;
        QString nodeId;
        QString input;
        int order = 0;
        int offset = 0;
        bool directTrace = false;
    };
    struct HeardSample {
        meshcore::HeardGroup packet;
        qint64 at;
    };
    struct ChannelWatch {
        QString id;
        quint32 stamp;
        meshcore::Bytes payload;
        qint64 sentAt;
        QMap<QString, int> relays;
        bool overflow = false;
    };

    QString targetTitle(const QString& id) const;
    bool write(const meshcore::Bytes& body, Wait wait);
    void frameReceived(const meshcore::Bytes& frame);
    void openLink(bool retrying);
    void loadNextChannel();
    void requestContacts();
    void syncQueue();
    void proceed();
    bool sendNext();
    bool retryExpired();
    void sendRetry();
    void settleSend(const QString& delivery);
    StoredMessage* pendingReceipt();
    bool acknowledge(quint32 tag);
    quint32 nextTimestamp();
    const meshcore::Contact* contactForId(const QString& id) const;
    const meshcore::Channel* channelForId(const QString& id) const;
    const meshcore::Channel* channelForIndex(int index) const;
    void assignPublicAliases();
    void finishChannelChange(bool success, const QString& explanation);
    bool submitConsole(int requestId, int operation, const QString& nodeId, const QString& input,
                       int order = 0, int offset = 0, bool directTrace = false);
    void finishConsoleRequest(int id, int operation, bool success, const QByteArray& data, const QString& message);
    void startConsole();
    void onRadioLog(const meshcore::Bytes& frame);
    void pruneEchoes(qint64 now);
    bool addEcho(ChannelWatch& watch, const HeardSample& sample);
    bool showEcho(const ChannelWatch& watch);
    const meshcore::Contact* relayForHash(const QString& hash) const;
    QString relayName(const QString& hash) const;
    QString receiveRoute(const meshcore::Incoming& message, const meshcore::Channel* channel) const;
    void onMessage(const meshcore::Incoming& message);
    ChatMessage displayMessage(const StoredMessage& row) const;
    QString roomAuthor(const QString& prefix) const;
    int senderMessageCount(const QString& id) const;
    void refreshUnread();
    bool canReact(const StoredMessage& row) const;
    void onChannelData(const meshcore::Bytes& frame);
    void startReaction();
    void onReactionResult(const meshcore::Bytes& frame);
    void takeReaction(PendingReaction reaction, bool deferIfMissing = true);
    bool appliesTo(const PendingReaction& reaction, const StoredMessage& row) const;
    bool applyReaction(const PendingReaction& reaction, StoredMessage& row);
    void adoptReactions(StoredMessage& row);
    void onSendResult(const meshcore::Bytes& frame);
    void deferSocketFailure(const QString& status, bool retryable = false);
    void finish(const QString& status, bool retryable = false);
    void addMessage(const QString& target, const ChatMessage& message,
                    quint32 tag = 0, qint64 deadline = 0);
    void invalidatePending();
    void loadHistory();
    void saveHistory();

    QTcpSocket* socket_;
#ifdef Q_OS_SYMBIAN
    NativeWlan* wlan_ = nullptr;
#endif
    ConnectionTrace connectionTrace_;
    QElapsedTimer clock_;
    meshcore::RadioConsole console_;
    meshcore::Decoder decoder_;
    Wait wait_ = Wait::None;
    qint64 timeout_ = 0;
    qint64 nextPoll_ = 0;
    qint64 nextInventoryRefresh_ = 0;
    qint64 reconnectAt_ = 0;
    qint64 readySince_ = 0;
    qint64 reconnectDelay_ = 1000;
    int reconnectAttempt_ = 0;
    QString lastFailure_;
    QString host_;
    quint16 port_ = 0;
    bool autoReconnect_ = true;
    int sendTries_ = DefaultSendTries;
    bool wlanRecovery_ = false;
    bool wantsConnection_ = false;
    bool retryingConnection_ = false;
    bool ready_ = false;
    bool syncNeeded_ = false;
    bool refreshNeeded_ = false;
    bool inventoryChanged_ = false;
    bool manualSync_ = false;
    bool currentManualSync_ = false;
    bool advertNeeded_ = false;
    bool batteryNeeded_ = false;
    bool closing_ = false;
    bool logFirstReply_ = false;
    QString pendingSocketFailure_;
    bool retrySocketFailure_ = false;
    QString status_;
    QString radioName_;
    QString radioKey_;
    QString firmware_;
    QString parameters_;
    QString battery_;
    int batteryMillivolts_ = 0;
    int lastBatteryMillivolts_ = 0;
    QString publicStatus_;
    QString queueStatus_;
    meshcore::SelfInfo self_;
    int directReceived_ = 0;
    int channelReceived_ = 0;
    int publicReceived_ = 0;
    int dataReceived_ = 0;
    int groupHeard_ = 0;
    int syncStartCount_ = 0;
    std::size_t radioNameBytes_ = 0;
    int channelCount_ = 0;
    int pathHashMode_ = 0;
    int channelIndex_ = 0;
    int channelChangeSlot_ = -1;
    meshcore::Bytes channelChangeCommand_;
    bool channelChangeReload_ = false;
    std::vector<meshcore::Contact> contacts_;
    std::vector<meshcore::Contact> nextContacts_;
    std::vector<meshcore::Channel> channels_;
    std::vector<meshcore::Channel> nextChannels_;
    std::array<int, 256> channelAliases_{};
    QVector<StoredMessage> history_;
    QMap<QString, int> unread_;
    QMap<QString, QString> blocks_;
    QMap<QString, QString> targetNames_;
    quint32 lastOutgoingTimestamp_ = 0;
    QVector<quint32> earlyAcks_;
    std::deque<TextRequest> outgoing_;
    int consoleRequestId_ = 0;
    std::optional<ConsoleRequest> consoleRequest_;
    std::deque<HeardSample> heard_;
    std::deque<ChannelWatch> echoWatches_;
    std::deque<meshcore::Incoming> deferredIncoming_;
    std::deque<PendingReaction> deferredReactions_;
    std::deque<meshcore::Bytes> deferredChannelData_;
    std::optional<PendingReaction> reactionRequest_;
    QString pendingId_;
    ChatMessage pendingMessage_;
    meshcore::Key pendingKey_{};
    meshcore::Bytes pendingEcho_;
    qint64 pendingEchoFrom_ = 0;
    bool pendingRetry_ = false;
};

#endif
